#include "pipewire_camera.h"

#ifdef FLC_HAVE_PIPEWIRE

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/result.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>

#include "include/pixel_format.h"

namespace lite_camera {
namespace {

constexpr int kPortalTimeoutSeconds = 120;  // the user may need to answer

// ---------------------------------------------------------------------------
// Portal: AccessCamera, then OpenPipeWireRemote. Runs a private GMainContext,
// so the app's main loop is not re-entered.

struct PortalWait {
  GMainLoop *loop;
  guint32 response = 2;
};

void OnPortalResponse(GDBusConnection *, const gchar *, const gchar *,
                      const gchar *, const gchar *, GVariant *parameters,
                      gpointer data) {
  auto *wait = static_cast<PortalWait *>(data);
  g_variant_get(parameters, "(u@a{sv})", &wait->response, nullptr);
  g_main_loop_quit(wait->loop);
}

gboolean OnPortalTimeout(gpointer data) {
  g_main_loop_quit(static_cast<PortalWait *>(data)->loop);
  return G_SOURCE_REMOVE;
}

std::mutex accessMutex;
bool accessGranted = false;

// Asks for camera access once per process. Returns false if denied.
bool AccessCamera(GDBusConnection *bus) {
  std::lock_guard<std::mutex> lock(accessMutex);
  if (accessGranted) return true;

  GMainContext *context = g_main_context_new();
  g_main_context_push_thread_default(context);
  PortalWait wait{g_main_loop_new(context, FALSE)};

  // The request object path is known before the call; subscribe first.
  std::string sender = g_dbus_connection_get_unique_name(bus) + 1;
  std::replace(sender.begin(), sender.end(), '.', '_');
  const std::string token = "flutter_lite_camera_" + std::to_string(getpid());
  const std::string request =
      "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
  const guint subscription = g_dbus_connection_signal_subscribe(
      bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request",
      "Response", request.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      OnPortalResponse, &wait, nullptr);

  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "handle_token",
                        g_variant_new_string(token.c_str()));
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.Camera", "AccessCamera",
      g_variant_new("(a{sv})", &options), G_VARIANT_TYPE("(o)"),
      G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
  if (reply) {
    g_variant_unref(reply);
    GSource *timeout = g_timeout_source_new_seconds(kPortalTimeoutSeconds);
    g_source_set_callback(timeout, OnPortalTimeout, &wait, nullptr);
    g_source_attach(timeout, context);
    g_main_loop_run(wait.loop);
    g_source_destroy(timeout);
    g_source_unref(timeout);
  } else {
    std::cerr << "Camera portal: AccessCamera failed: " << error->message << std::endl;
    g_error_free(error);
  }
  g_dbus_connection_signal_unsubscribe(bus, subscription);
  g_main_loop_unref(wait.loop);
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);

  accessGranted = wait.response == 0;
  if (!accessGranted) {
    std::cerr << "Camera portal: access denied (response " << wait.response
              << ")" << std::endl;
  }
  return accessGranted;
}

// A PipeWire remote with the cameras the user allowed, or -1.
int OpenRemote() {
  GError *error = nullptr;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus) {
    std::cerr << "Camera portal: no session bus: " << error->message << std::endl;
    g_error_free(error);
    return -1;
  }
  int fd = -1;
  if (AccessCamera(bus)) {
    GUnixFDList *fds = nullptr;
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
        bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Camera", "OpenPipeWireRemote",
        g_variant_new("(a{sv})", nullptr), G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fds, nullptr, &error);
    if (reply) {
      gint32 handle = -1;
      g_variant_get(reply, "(h)", &handle);
      fd = g_unix_fd_list_get(fds, handle, &error);
      g_variant_unref(reply);
      g_object_unref(fds);
    }
    if (fd < 0 && error) {
      std::cerr << "Camera portal: OpenPipeWireRemote failed: " << error->message
                << std::endl;
      g_error_free(error);
    }
  }
  g_object_unref(bus);
  return fd;
}

// ---------------------------------------------------------------------------
// PipeWire connection and camera nodes.

struct Node {
  uint32_t id;
  std::string serial;
  std::string name;
};

struct Connection {
  pw_thread_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  pw_registry *registry = nullptr;
  spa_hook coreListener{};
  spa_hook registryListener{};
  std::vector<Node> nodes;
  int syncSeq = -1;
  bool synced = false;

  ~Connection() {
    if (loop) pw_thread_loop_stop(loop);
    if (registry) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    if (core) pw_core_disconnect(core);
    if (context) pw_context_destroy(context);
    if (loop) pw_thread_loop_destroy(loop);
  }
};

void OnGlobal(void *data, uint32_t id, uint32_t, const char *type, uint32_t,
              const spa_dict *props) {
  auto *c = static_cast<Connection *>(data);
  if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) return;
  const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!mediaClass || strcmp(mediaClass, "Video/Source") != 0) return;
  const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
  const char *name = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
  if (!name) name = spa_dict_lookup(props, PW_KEY_NODE_NICK);
  if (!name) name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  if (getenv("FLUTTER_LITE_CAMERA_DEBUG")) {
    std::cerr << "Camera portal: node " << id << " serial " << (serial ? serial : "-")
              << " " << (name ? name : "-") << std::endl;
  }
  for (const Node &n : c->nodes) {
    if (n.id == id) return;
  }
  c->nodes.push_back({id, serial ? serial : std::to_string(id), name ? name : "Camera"});
}

void OnCoreDone(void *data, uint32_t id, int seq) {
  auto *c = static_cast<Connection *>(data);
  if (id == PW_ID_CORE && seq == c->syncSeq) {
    c->synced = true;
    pw_thread_loop_signal(c->loop, false);
  }
}

const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = OnGlobal,
};
const pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .done = OnCoreDone,
};

// Connects to the portal's PipeWire remote and lists its cameras. The loop
// is running and unlocked on return.
std::unique_ptr<Connection> Connect() {
  static std::once_flag init;
  std::call_once(init, [] { pw_init(nullptr, nullptr); });
  const int fd = OpenRemote();
  if (fd < 0) return nullptr;
  auto c = std::make_unique<Connection>();
  c->loop = pw_thread_loop_new("flutter_lite_camera", nullptr);
  c->context = pw_context_new(pw_thread_loop_get_loop(c->loop), nullptr, 0);
  if (!c->loop || !c->context || pw_thread_loop_start(c->loop) < 0) {
    close(fd);
    return nullptr;
  }
  pw_thread_loop_lock(c->loop);
  c->core = pw_context_connect_fd(c->context, fd, nullptr, 0);
  if (!c->core) {
    pw_thread_loop_unlock(c->loop);
    std::cerr << "Camera portal: cannot connect to PipeWire" << std::endl;
    return nullptr;
  }
  pw_core_add_listener(c->core, &c->coreListener, &kCoreEvents, c.get());
  c->registry = pw_core_get_registry(c->core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(c->registry, &c->registryListener, &kRegistryEvents,
                           c.get());
  c->syncSeq = pw_core_sync(c->core, PW_ID_CORE, 0);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!c->synced && std::chrono::steady_clock::now() < deadline) {
    pw_thread_loop_timed_wait(c->loop, 1);
  }
  std::sort(c->nodes.begin(), c->nodes.end(),
            [](const Node &a, const Node &b) { return a.id < b.id; });
  pw_thread_loop_unlock(c->loop);
  return c;
}

}  // namespace

// ---------------------------------------------------------------------------

struct PipeWireCamera::Impl {
  std::unique_ptr<Connection> connection;
  pw_stream *stream = nullptr;
  spa_hook streamListener{};
  RgbaCallback onFrame;
  uint32_t fourcc = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool formatReady = false;
  bool failed = false;
  std::vector<unsigned char> rgba;
  std::vector<unsigned char> planes;  // NV12 split across two buffers
};

namespace {

// The pods are built with explicit builder calls: spa_pod_builder_add_object's
// varargs come out wrong when compiled with clang (Flutter's Linux
// toolchain), and the camera link then fails with -22.

// Buffers the stream can map: memory pointers and memfd. Without this the
// camera may pick DMA-BUF, which this client cannot use (link error -22).
const spa_pod *BufferTypes(spa_pod_builder *b) {
  spa_pod_frame object, choice;
  spa_pod_builder_push_object(b, &object, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
  spa_pod_builder_prop(b, SPA_PARAM_BUFFERS_buffers, 0);
  spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Range, 0);
  spa_pod_builder_int(b, 4);
  spa_pod_builder_int(b, 2);
  spa_pod_builder_int(b, 16);
  spa_pod_builder_pop(b, &choice);
  spa_pod_builder_prop(b, SPA_PARAM_BUFFERS_dataType, 0);
  spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Flags, 0);
  spa_pod_builder_int(b, (1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd));
  spa_pod_builder_pop(b, &choice);
  return static_cast<const spa_pod *>(spa_pod_builder_pop(b, &object));
}

// Raw video, YUY2 or NV12, size and rate ranges around the preference.
const spa_pod *VideoFormats(spa_pod_builder *b, uint32_t width, uint32_t height) {
  spa_pod_frame object, choice;
  spa_pod_builder_push_object(b, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_prop(b, SPA_FORMAT_mediaType, 0);
  spa_pod_builder_id(b, SPA_MEDIA_TYPE_video);
  spa_pod_builder_prop(b, SPA_FORMAT_mediaSubtype, 0);
  spa_pod_builder_id(b, SPA_MEDIA_SUBTYPE_raw);
  spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_format, 0);
  spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
  spa_pod_builder_id(b, SPA_VIDEO_FORMAT_YUY2);  // default
  spa_pod_builder_id(b, SPA_VIDEO_FORMAT_YUY2);
  spa_pod_builder_id(b, SPA_VIDEO_FORMAT_NV12);
  spa_pod_builder_pop(b, &choice);
  spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_size, 0);
  spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Range, 0);
  spa_pod_builder_rectangle(b, width, height);
  spa_pod_builder_rectangle(b, 1, 1);
  spa_pod_builder_rectangle(b, 8192, 8192);
  spa_pod_builder_pop(b, &choice);
  spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_framerate, 0);
  spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Range, 0);
  spa_pod_builder_fraction(b, 30, 1);
  spa_pod_builder_fraction(b, 0, 1);
  spa_pod_builder_fraction(b, 120, 1);
  spa_pod_builder_pop(b, &choice);
  return static_cast<const spa_pod *>(spa_pod_builder_pop(b, &object));
}

void OnParamChanged(void *data, uint32_t id, const spa_pod *param) {
  auto *impl = static_cast<PipeWireCamera::Impl *>(data);
  if (!param || id != SPA_PARAM_Format) return;
  uint32_t mediaType, mediaSubtype;
  spa_video_info_raw info{};
  if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 ||
      mediaType != SPA_MEDIA_TYPE_video ||
      mediaSubtype != SPA_MEDIA_SUBTYPE_raw ||
      spa_format_video_raw_parse(param, &info) < 0) {
    return;
  }
  impl->fourcc = info.format == SPA_VIDEO_FORMAT_YUY2   ? kFourccYUYV
                 : info.format == SPA_VIDEO_FORMAT_NV12 ? kFourccNV12
                                                        : 0;
  impl->width = info.size.width;
  impl->height = info.size.height;
  impl->rgba.assign(size_t(impl->width) * impl->height * 4, 0);
  impl->formatReady = impl->fourcc != 0;
  impl->failed = impl->fourcc == 0;
  pw_thread_loop_signal(impl->connection->loop, false);
}

void OnStateChanged(void *data, pw_stream_state, pw_stream_state state,
                    const char *error) {
  auto *impl = static_cast<PipeWireCamera::Impl *>(data);
  if (state == PW_STREAM_STATE_ERROR) {
    std::cerr << "Camera portal: stream error: " << (error ? error : "?") << std::endl;
    impl->failed = true;
    pw_thread_loop_signal(impl->connection->loop, false);
  }
}

void OnProcess(void *data) {
  auto *impl = static_cast<PipeWireCamera::Impl *>(data);
  pw_buffer *b = pw_stream_dequeue_buffer(impl->stream);
  if (!b) return;
  spa_buffer *buf = b->buffer;
  const unsigned char *src = nullptr;
  size_t size = 0;
  uint32_t stride = 0;
  if (buf->n_datas >= 1 && buf->datas[0].data && buf->datas[0].chunk) {
    const spa_data &d0 = buf->datas[0];
    src = static_cast<const unsigned char *>(d0.data) + d0.chunk->offset;
    size = d0.chunk->size;
    stride = d0.chunk->stride > 0 ? uint32_t(d0.chunk->stride) : 0;
    // NV12 with the UV plane in a second buffer: make it contiguous.
    if (impl->fourcc == kFourccNV12 && buf->n_datas >= 2 && buf->datas[1].data &&
        buf->datas[1].chunk) {
      const spa_data &d1 = buf->datas[1];
      impl->planes.assign(src, src + size);
      const auto *uv = static_cast<const unsigned char *>(d1.data) + d1.chunk->offset;
      impl->planes.insert(impl->planes.end(), uv, uv + d1.chunk->size);
      src = impl->planes.data();
      size = impl->planes.size();
    }
  }
  if (src && ConvertFrame(impl->fourcc, src, size, impl->width, impl->height,
                          stride, impl->rgba.data(), 4) &&
      impl->onFrame) {
    impl->onFrame(impl->rgba.data(), impl->width, impl->height);
  }
  pw_stream_queue_buffer(impl->stream, b);
}

const pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = OnStateChanged,
    .param_changed = OnParamChanged,
    .process = OnProcess,
};

}  // namespace

bool PipeWireCamera::ShouldUse() {
  const char *force = getenv("FLUTTER_LITE_CAMERA_PORTAL");
  if (force && strcmp(force, "1") == 0) return true;
  if (force && strcmp(force, "0") == 0) return false;
  return access("/.flatpak-info", F_OK) == 0;
}

std::vector<std::string> PipeWireCamera::ListDevices() {
  std::vector<std::string> names;
  auto c = Connect();
  if (c) {
    for (const Node &n : c->nodes) names.push_back(n.name);
  }
  return names;
}

PipeWireCamera::PipeWireCamera() : impl_(new Impl()) {}

PipeWireCamera::~PipeWireCamera() {
  Close();
  delete impl_;
}

bool PipeWireCamera::Open(int index, uint32_t width, uint32_t height,
                          RgbaCallback onFrame) {
  Close();
  impl_->connection = Connect();
  Connection *c = impl_->connection.get();
  if (!c) return false;
  if (index < 0 || size_t(index) >= c->nodes.size()) {
    std::cerr << "Camera portal: no camera " << index << " (" << c->nodes.size()
              << " allowed)" << std::endl;
    impl_->connection.reset();
    return false;
  }
  impl_->onFrame = std::move(onFrame);
  impl_->formatReady = false;
  impl_->failed = false;

  pw_thread_loop_lock(c->loop);
  pw_properties *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Camera", PW_KEY_TARGET_OBJECT,
      c->nodes[index].serial.c_str(), nullptr);
  impl_->stream = pw_stream_new(c->core, "flutter_lite_camera", props);
  pw_stream_add_listener(impl_->stream, &impl_->streamListener, &kStreamEvents, impl_);

  uint8_t podBuffer[1536];
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
  const spa_pod *params[2];
  params[0] = VideoFormats(&builder, width, height);
  // Mappable buffers only, from the start: the camera allocates as soon as
  // a format is set, before param_changed could ask for them.
  params[1] = BufferTypes(&builder);
  const int result = pw_stream_connect(
      impl_->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                   PW_STREAM_FLAG_MAP_BUFFERS),
      params, 2);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (result >= 0 && !impl_->formatReady && !impl_->failed &&
         std::chrono::steady_clock::now() < deadline) {
    pw_thread_loop_timed_wait(c->loop, 1);
  }
  const bool ok = result >= 0 && impl_->formatReady;
  width_ = impl_->width;
  height_ = impl_->height;
  pw_thread_loop_unlock(c->loop);
  if (!ok) {
    std::cerr << "Camera portal: no YUY2 or NV12 format from "
              << c->nodes[index].name << std::endl;
    Close();
  }
  return ok;
}

void PipeWireCamera::Close() {
  Connection *c = impl_->connection.get();
  if (!c) return;
  pw_thread_loop_lock(c->loop);
  if (impl_->stream) {
    pw_stream_destroy(impl_->stream);
    impl_->stream = nullptr;
  }
  pw_thread_loop_unlock(c->loop);
  impl_->connection.reset();
  impl_->onFrame = nullptr;
}

}  // namespace lite_camera

#else  // !FLC_HAVE_PIPEWIRE

namespace lite_camera {
struct PipeWireCamera::Impl {};
bool PipeWireCamera::ShouldUse() { return false; }
std::vector<std::string> PipeWireCamera::ListDevices() { return {}; }
PipeWireCamera::PipeWireCamera() = default;
PipeWireCamera::~PipeWireCamera() = default;
bool PipeWireCamera::Open(int, uint32_t, uint32_t, RgbaCallback) { return false; }
void PipeWireCamera::Close() {}
}  // namespace lite_camera

#endif  // FLC_HAVE_PIPEWIRE
