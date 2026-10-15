// Pixel formats the Linux backend can convert, and the conversions.
//
// V4L2's VIDIOC_S_FMT is a request: the driver may answer with another
// format. Cameras that only offer NV12 and MJPG exist (Parallels' virtual
// camera, some laptop and phone cameras). The backend must use the format
// the driver returned, and must not read past the end of a frame.
//
// Header-only and free of V4L2 calls, so tests can run on any host.
#pragma once

#include <cstddef>
#include <cstdint>

namespace lite_camera {

// Same values as V4L2_PIX_FMT_YUYV and V4L2_PIX_FMT_NV12.
constexpr uint32_t kFourccYUYV = 0x56595559; // 'YUYV'
constexpr uint32_t kFourccNV12 = 0x3231564e; // 'NV12'

// Formats in order of preference: YUYV first, as before.
constexpr uint32_t kSupportedFormats[] = {kFourccYUYV, kFourccNV12};

inline bool IsSupportedFormat(uint32_t fourcc) {
  for (uint32_t f : kSupportedFormats) {
    if (f == fourcc) return true;
  }
  return false;
}

// Bytes per row of the first plane. A driver reports 0 when it has no
// padding; then the row is exactly the pixels.
inline size_t RowStride(uint32_t fourcc, uint32_t width, uint32_t bytesPerLine) {
  // Rounded up to whole pixel pairs: both formats share chroma per pair.
  const size_t pairs = (size_t(width) + 1) / 2;
  const size_t minimum = fourcc == kFourccYUYV ? pairs * 4 : pairs * 2;
  return bytesPerLine >= minimum ? bytesPerLine : minimum;
}

// Bytes a complete frame needs. 0 for an unsupported format.
inline size_t FrameBytes(uint32_t fourcc, uint32_t width, uint32_t height,
                         uint32_t bytesPerLine) {
  const size_t stride = RowStride(fourcc, width, bytesPerLine);
  switch (fourcc) {
    case kFourccYUYV:
      return stride * height;
    case kFourccNV12:
      // Y plane, then the interleaved UV plane at half height.
      return stride * height + stride * ((height + 1) / 2);
    default:
      return 0;
  }
}

inline unsigned char ClampByte(int v) {
  return static_cast<unsigned char>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// BT.601 full range, as the old double-based converter: R = Y + 1.402 V',
// G = Y - 0.344 U' - 0.714 V', B = Y + 1.772 U'. Fixed point, scale 2^16.
inline void YuvToRgb(int y, int u, int v, unsigned char *out) {
  const int du = u - 128, dv = v - 128;
  out[0] = ClampByte(y + ((91881 * dv) >> 16));
  out[1] = ClampByte(y - ((22553 * du + 46802 * dv) >> 16));
  out[2] = ClampByte(y + ((116130 * du) >> 16));
}

// Converts one frame to RGB (channels 3) or RGBA (channels 4, alpha 255).
// Returns false, and writes nothing, if the format is unsupported or the
// frame is shorter than FrameBytes(): a truncated or wrong-format buffer
// must not be read past its end.
inline bool ConvertFrame(uint32_t fourcc, const unsigned char *src,
                         size_t srcBytes, uint32_t width, uint32_t height,
                         uint32_t bytesPerLine, unsigned char *dst,
                         int channels) {
  const size_t needed = FrameBytes(fourcc, width, height, bytesPerLine);
  if (needed == 0 || srcBytes < needed || (channels != 3 && channels != 4)) {
    return false;
  }
  const size_t stride = RowStride(fourcc, width, bytesPerLine);
  for (uint32_t row = 0; row < height; ++row) {
    unsigned char *out = dst + size_t(row) * width * channels;
    if (fourcc == kFourccYUYV) {
      const unsigned char *in = src + row * stride;
      for (uint32_t x = 0; x < width; ++x) {
        const unsigned char *pair = in + (x & ~1u) * 2;
        YuvToRgb(pair[(x & 1) ? 2 : 0], pair[1], pair[3], out);
        if (channels == 4) out[3] = 255;
        out += channels;
      }
    } else {
      const unsigned char *yRow = src + row * stride;
      const unsigned char *uvRow = src + stride * height + (row / 2) * stride;
      for (uint32_t x = 0; x < width; ++x) {
        const unsigned char *uv = uvRow + (x & ~1u);
        YuvToRgb(yRow[x], uv[0], uv[1], out);
        if (channels == 4) out[3] = 255;
        out += channels;
      }
    }
  }
  return true;
}

}  // namespace lite_camera
