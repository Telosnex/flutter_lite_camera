#include "include/Camera.h"
#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <cstring>
#include <poll.h>

#include "include/pixel_format.h"
#include "pipewire_camera.h"

#include <condition_variable>
#include <chrono>

// Bytes of frame data in a dequeued buffer. Some drivers leave bytesused at
// 0; then the mapped length is the bound.
static size_t FrameBytesUsed(const v4l2_buffer &buf, const Buffer &mapped)
{
    const size_t used = buf.bytesused ? buf.bytesused : mapped.length;
    return used < mapped.length ? used : mapped.length;
}


static lite_camera::PipeWireCamera *Portal(void *p)
{
    return static_cast<lite_camera::PipeWireCamera *>(p);
}

bool Camera::OpenPortal(int cameraIndex)
{
    if (!portalCamera)
        portalCamera = new lite_camera::PipeWireCamera();
    portalIndex = cameraIndex;
    if (!Portal(portalCamera)->Open(cameraIndex, frameWidth, frameHeight,
                                    [this](const unsigned char *rgba, uint32_t w, uint32_t h)
                                    { OnPortalFrame(rgba, w, h); }))
    {
        delete Portal(portalCamera);
        portalCamera = nullptr;
        return false;
    }
    frameWidth = Portal(portalCamera)->width();
    frameHeight = Portal(portalCamera)->height();
    return true;
}

// PipeWire's thread. The stream runs while the camera is open; the preview
// callback gets frames only between StartCaptureLoop and StopCaptureLoop.
void Camera::OnPortalFrame(const unsigned char *rgba, uint32_t width, uint32_t height)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (width != frameWidth || height != frameHeight)
        return;
    cachedRgba.assign(rgba, rgba + size_t(width) * height * 4);
    if (streaming.load() && frameCallback)
        frameCallback(rgba, (int)width, (int)height);
}

bool Camera::Open(int cameraIndex)
{
    if (lite_camera::PipeWireCamera::ShouldUse())
        return OpenPortal(cameraIndex);

    std::string devicePath = "/dev/video" + std::to_string(cameraIndex);
    fd = open(devicePath.c_str(), O_RDWR);
    if (fd < 0)
    {
        perror("Error opening video device");
        return false;
    }

    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
    {
        perror("Error querying device capabilities");
        close(fd);
        return false;
    }

    std::cout << "Driver: " << cap.driver << "\nCard: " << cap.card << std::endl;

    if (!NegotiateFormat(frameWidth, frameHeight))
    {
        close(fd);
        fd = -1;
        return false;
    }

    if (!InitDevice() || !StartCapture())
    {
        Release();
        return false;
    }

    return true;
}

void Camera::Release()
{
    StopCaptureLoop();
    if (portalCamera)
    {
        // Close stops PipeWire's thread, so no frame arrives after it.
        delete Portal(portalCamera);
        portalCamera = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cachedRgba.clear();
        cachedRgba.shrink_to_fit();
    }

    StopCapture();
    UninitDevice();
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
}

bool Camera::StartCaptureLoop(FrameCallback callback)
{
    if (portalCamera)
    {
        if (streaming.load())
            return false;
        std::lock_guard<std::mutex> lock(cacheMutex);
        frameCallback = std::move(callback);
        streaming.store(true);
        return true;
    }
    if (fd < 0 || streaming.load())
        return false;

    frameCallback = std::move(callback);
    streaming.store(true);
    captureThread = std::thread(&Camera::CaptureLoop, this);
    return true;
}

void Camera::StopCaptureLoop()
{
    if (portalCamera)
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        streaming.store(false);
        frameCallback = nullptr;
        return;
    }
    streaming.store(false);
    if (captureThread.joinable())
    {
        captureThread.join();
    }
    frameCallback = nullptr;
}

void Camera::CaptureLoop()
{
    std::vector<unsigned char> rgba(static_cast<size_t>(frameWidth) * frameHeight * 4);

    while (streaming.load())
    {
        // Wait for a frame with a timeout so StopCaptureLoop() stays responsive.
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ready = poll(&pfd, 1, 200);
        if (ready <= 0 || !(pfd.revents & POLLIN))
            continue;

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0)
        {
            if (streaming.load())
            {
                perror("Capture loop: failed to dequeue buffer");
            }
            break;
        }

        const bool converted = lite_camera::ConvertFrame(
            pixelFormat, reinterpret_cast<unsigned char *>(buffers[buf.index].start),
            FrameBytesUsed(buf, buffers[buf.index]), frameWidth, frameHeight,
            bytesPerLine, rgba.data(), 4);

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
        {
            perror("Capture loop: failed to queue buffer");
        }
        if (!converted)
        {
            // A short or corrupt frame: skip it rather than read past it.
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            cachedRgba = rgba;
        }

        if (frameCallback)
        {
            frameCallback(rgba.data(), (int)frameWidth, (int)frameHeight);
        }
    }

    streaming.store(false);
}

bool Camera::SetResolution(int width, int height)
{
    if (portalCamera)
    {
        // Reopen the stream at the new size; keep the preview callback.
        const uint32_t oldWidth = frameWidth, oldHeight = frameHeight;
        FrameCallback callback;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            callback = frameCallback;
        }
        const bool wasStreaming = streaming.load();
        delete Portal(portalCamera);
        portalCamera = nullptr;
        frameWidth = width;
        frameHeight = height;
        bool ok = OpenPortal(portalIndex);
        if (!ok)
        {
            frameWidth = oldWidth;
            frameHeight = oldHeight;
            OpenPortal(portalIndex);
        }
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            cachedRgba.clear();
            frameCallback = callback;
        }
        streaming.store(wasStreaming && portalCamera != nullptr);
        return ok;
    }
    if (fd == -1)
    {
        std::cerr << "Device not opened." << std::endl;
        return false;
    }

    // Changing the format while buffers are queued and the stream is on is
    // unsafe: the mmap'd buffers are sized for the old format and the
    // capture loop converts with frameWidth/frameHeight, so a mid-stream
    // switch reads/writes out of bounds. Tear the pipeline down first and
    // rebuild it at the new size.
    bool wasStreaming = streaming.load();
    FrameCallback savedCallback = frameCallback;
    if (wasStreaming)
    {
        StopCaptureLoop();
    }

    uint32_t oldWidth = frameWidth;
    uint32_t oldHeight = frameHeight;

    StopCapture();  // STREAMOFF
    UninitDevice(); // munmap

    // Free the driver-side buffers; most drivers require this before S_FMT
    // with a different size.
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 0;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_REQBUFS, &req);

    bool success = NegotiateFormat(width, height) && InitDevice() && StartCapture();

    if (!success)
    {
        // Roll back to the previous format so the camera keeps working.
        StopCapture();
        UninitDevice();
        req.count = 0;
        ioctl(fd, VIDIOC_REQBUFS, &req);
        if (NegotiateFormat(oldWidth, oldHeight))
        {
            if (InitDevice() && StartCapture() && wasStreaming)
            {
                StartCaptureLoop(savedCallback);
            }
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cachedRgba.clear(); // discard stale frames at the old size
    }

    if (wasStreaming)
    {
        StartCaptureLoop(savedCallback);
    }

    return true;
}

FrameData Camera::CaptureFrame()
{
    // The portal stream always runs: wait briefly for its first frame.
    if (portalCamera)
    {
        for (int i = 0; i < 100; ++i)
        {
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                if (!cachedRgba.empty())
                    break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // While the preview stream is running, serve the latest cached frame
    // instead of dequeuing a new buffer.
    if (streaming.load() || portalCamera)
    {
        FrameData frame;
        frame.width = frameWidth;
        frame.height = frameHeight;
        frame.rgbData = nullptr;

        std::lock_guard<std::mutex> lock(cacheMutex);
        if (cachedRgba.empty())
        {
            return frame;
        }

        frame.size = static_cast<size_t>(frameWidth) * frameHeight * 3;
        frame.rgbData = new unsigned char[frame.size];

        const unsigned char *src = cachedRgba.data();
        unsigned char *dst = frame.rgbData;
        for (size_t i = 0, j = 0; j < frame.size; i += 4, j += 3)
        {
            dst[j] = src[i];
            dst[j + 1] = src[i + 1];
            dst[j + 2] = src[i + 2];
        }
        return frame;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    // Dequeue a buffer from the video capture queue
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0)
    {
        perror("Failed to dequeue buffer");
        return {};
    }

    // FILE *file = fopen("raw_frame.yuyv", "wb");
    // fwrite(buffers[buf.index].start, 1, buffers[buf.index].length, file);
    // fclose(file);

    // Prepare FrameData structure
    FrameData frame;
    frame.width = frameWidth;
    frame.height = frameHeight;
    frame.size = frameWidth * frameHeight * 3; // 3 bytes per pixel (RGB)
    frame.rgbData = new unsigned char[frame.size];

    const bool converted = lite_camera::ConvertFrame(
        pixelFormat, reinterpret_cast<unsigned char *>(buffers[buf.index].start),
        FrameBytesUsed(buf, buffers[buf.index]), frameWidth, frameHeight,
        bytesPerLine, frame.rgbData, 3);

    // Requeue the buffer
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
    {
        perror("Failed to queue buffer");
    }
    if (!converted)
    {
        ReleaseFrame(frame);
        return {};
    }

    return frame;
}

// Asks for each supported format in order, and keeps the first one the
// driver grants. VIDIOC_S_FMT can answer with another format (MJPG on
// Parallels' and some laptop cameras); treating that as YUYV read past the
// end of the frame buffer.
bool Camera::NegotiateFormat(uint32_t width, uint32_t height)
{
    uint32_t offered = 0;
    for (uint32_t fourcc : lite_camera::kSupportedFormats)
    {
        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
        fmt.fmt.pix.pixelformat = fourcc;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
            continue;
        offered = fmt.fmt.pix.pixelformat;
        if (offered != fourcc)
            continue;
        pixelFormat = fourcc;
        frameWidth = fmt.fmt.pix.width;
        frameHeight = fmt.fmt.pix.height;
        bytesPerLine = fmt.fmt.pix.bytesperline;
        return true;
    }
    std::cerr << "Camera offers no supported pixel format (YUYV, NV12); driver answered "
              << std::string(reinterpret_cast<const char *>(&offered), 4) << std::endl;
    return false;
}

bool Camera::InitDevice()
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0)
    {
        perror("Failed to request buffers");
        return false;
    }

    buffers = new Buffer[req.count];
    bufferCount = req.count;

    for (unsigned int i = 0; i < bufferCount; ++i)
    {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            perror("Failed to query buffer");
            return false;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);

        if (buffers[i].start == MAP_FAILED)
        {
            perror("Failed to map buffer");
            return false;
        }

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
        {
            perror("Failed to queue buffer");
            return false;
        }
    }
    return true;
}

void Camera::UninitDevice()
{
    if (buffers)
    {
        for (unsigned int i = 0; i < bufferCount; ++i)
        {
            munmap(buffers[i].start, buffers[i].length);
        }
        delete[] buffers;
        buffers = nullptr;
    }
}

bool Camera::StartCapture()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return ioctl(fd, VIDIOC_STREAMON, &type) >= 0;
}

void Camera::StopCapture()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
}

std::vector<MediaTypeInfo> Camera::ListSupportedMediaTypes()
{
    std::vector<MediaTypeInfo> mediaTypes;
    if (portalCamera)
    {
        // PipeWire negotiates the size; report the current one.
        MediaTypeInfo info = {};
        info.width = frameWidth;
        info.height = frameHeight;
        strncpy(info.subtypeName, "PipeWire", sizeof(info.subtypeName) - 1);
        mediaTypes.push_back(info);
        return mediaTypes;
    }

    if (fd == -1)
    {
        std::cerr << "Device not opened.\n";
        return mediaTypes;
    }

    struct v4l2_fmtdesc fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0)
    {
        struct v4l2_frmsizeenum frmsize;
        memset(&frmsize, 0, sizeof(frmsize));
        frmsize.pixel_format = fmt.pixelformat;

        // Iterate over supported frame sizes for this format
        while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0)
        {
            MediaTypeInfo info = {};
            info.width = 0;
            info.height = 0;

            // Handle discrete frame sizes
            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
            {
                info.width = frmsize.discrete.width;
                info.height = frmsize.discrete.height;
            }
            // Handle stepwise frame sizes
            else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE)
            {
                // Use the minimum resolution as an example
                info.width = frmsize.stepwise.min_width;
                info.height = frmsize.stepwise.min_height;
            }

            // Copy the format description
            strncpy(info.subtypeName, reinterpret_cast<const char *>(fmt.description), sizeof(info.subtypeName) - 1);
            info.subtypeName[sizeof(info.subtypeName) - 1] = '\0'; // Ensure null-termination

            mediaTypes.push_back(info);
            frmsize.index++;
        }

        fmt.index++;
    }

    if (mediaTypes.empty())
    {
        std::cerr << "No supported media types found.\n";
    }

    return mediaTypes;
}

std::vector<CaptureDeviceInfo> ListCaptureDevices()
{
    std::vector<CaptureDeviceInfo> devices;

    if (lite_camera::PipeWireCamera::ShouldUse())
    {
        for (const std::string &name : lite_camera::PipeWireCamera::ListDevices())
        {
            CaptureDeviceInfo deviceInfo = {};
            strncpy(deviceInfo.friendlyName, name.c_str(), sizeof(deviceInfo.friendlyName) - 1);
            devices.push_back(deviceInfo);
        }
        return devices;
    }

    // Scan for /dev/videoX devices
    for (int i = 0; i < 10; ++i) // Check first 10 devices (can be increased if needed)
    {
        std::string devicePath = "/dev/video" + std::to_string(i);

        // Open the device
        int fd = open(devicePath.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd == -1)
        {
            continue; // Skip if the device cannot be opened
        }

        // Query device capabilities
        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
        {
            // Check if it's a video capture device
            if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
            {
                CaptureDeviceInfo deviceInfo = {};

                // Copy the friendly name directly into the `friendlyName` buffer
                strncpy(deviceInfo.friendlyName, reinterpret_cast<const char *>(cap.card), sizeof(deviceInfo.friendlyName) - 1);

                // Null-terminate the friendlyName string
                deviceInfo.friendlyName[sizeof(deviceInfo.friendlyName) - 1] = '\0';

                devices.push_back(deviceInfo);
            }
        }

        // Close the device
        close(fd);
    }

    return devices;
}

void ReleaseFrame(FrameData &frame)
{
    if (frame.rgbData)
    {
        delete[] frame.rgbData;
        frame.rgbData = nullptr;
        frame.size = 0;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Save a frame as a JPEG image using the STB library
// https://github.com/nothings/stb/blob/master/stb_image_write.h
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "include/stb_image_write.h"
void saveFrameAsJPEG(const unsigned char *data, int width, int height, const std::string &filename)
{
    // Simple image saving using STB library or another JPEG encoding method
    if (stbi_write_jpg(filename.c_str(), width, height, 3, data, 90))
    {
        std::cout << "Saved frame to " << filename << std::endl;
    }
    else
    {
        std::cerr << "Error saving frame as JPEG." << std::endl;
    }
}
///////////////////////////////////////////////////////////////////////////////