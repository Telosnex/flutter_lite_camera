// Camera access through the Camera portal and PipeWire.
//
// A sandboxed app (Flatpak) has no /dev/video*. It asks the portal
// (org.freedesktop.portal.Camera) for access, receives a PipeWire remote
// with the cameras the user allowed, and reads frames from a PipeWire
// stream. Frames are YUY2 or NV12, converted by pixel_format.h.
//
// Built only when libpipewire-0.3 is found (FLC_HAVE_PIPEWIRE).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lite_camera {

// Called on PipeWire's thread with an RGBA frame.
using RgbaCallback =
    std::function<void(const unsigned char *rgba, uint32_t width, uint32_t height)>;

class PipeWireCamera {
 public:
  // Whether to use the portal: in a Flatpak sandbox, or with
  // FLUTTER_LITE_CAMERA_PORTAL=1 (=0 disables it). Always false without
  // PipeWire support in the build.
  static bool ShouldUse();

  // Names of the cameras the portal grants. Asks for access the first time.
  static std::vector<std::string> ListDevices();

  PipeWireCamera();
  ~PipeWireCamera();
  PipeWireCamera(const PipeWireCamera &) = delete;
  PipeWireCamera &operator=(const PipeWireCamera &) = delete;

  // Opens camera [index] (order of ListDevices) and starts streaming at the
  // size closest to width x height. Waits until the format is negotiated.
  bool Open(int index, uint32_t width, uint32_t height, RgbaCallback onFrame);
  void Close();

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

  struct Impl;

 private:
  Impl *impl_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

}  // namespace lite_camera
