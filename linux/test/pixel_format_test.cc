// Unit tests for linux/include/pixel_format.h. No V4L2 or Flutter needed:
//   c++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined \
//     linux/test/pixel_format_test.cc -o /tmp/pixel_format_test && /tmp/pixel_format_test
#include "../include/pixel_format.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace lite_camera;

static int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                \
    }                                                            \
  } while (0)

static bool Near(int a, int b) { return a - b <= 1 && b - a <= 1; }

int main() {
  // Negotiation: MJPG (the driver's answer on Parallels) is not accepted.
  constexpr uint32_t kMJPG = 0x47504a4d;
  CHECK(IsSupportedFormat(kFourccYUYV));
  CHECK(IsSupportedFormat(kFourccNV12));
  CHECK(!IsSupportedFormat(kMJPG));

  // The crash: 1280x720 treated as YUYV needs 1843200 bytes; the MJPG
  // buffer had 460800. The converter must refuse, not read past the end.
  {
    std::vector<unsigned char> small(460800), out(1280 * 720 * 4);
    CHECK(FrameBytes(kFourccYUYV, 1280, 720, 0) == 1843200);
    CHECK(!ConvertFrame(kFourccYUYV, small.data(), small.size(), 1280, 720, 0,
                        out.data(), 4));
    CHECK(!ConvertFrame(kMJPG, small.data(), small.size(), 1280, 720, 0,
                        out.data(), 4));
  }

  // NV12 sizes, with and without row padding.
  CHECK(FrameBytes(kFourccNV12, 1280, 720, 0) == 1280 * 720 * 3 / 2);
  CHECK(FrameBytes(kFourccNV12, 1280, 720, 1344) == 1344 * 720 * 3 / 2);
  CHECK(FrameBytes(kFourccNV12, 4, 3, 0) == 4 * 3 + 4 * 2);

  // NV12 colors: a 2x2 frame, one chroma sample.
  {
    struct Case { int y, u, v, r, g, b; } cases[] = {
        {128, 128, 128, 128, 128, 128},  // grey
        {76, 85, 255, 254, 0, 0},        // red
        {149, 43, 21, 0, 255, 0},        // green
        {29, 255, 107, 0, 0, 255},       // blue
    };
    for (const Case &c : cases) {
      std::vector<unsigned char> nv12 = {
          (unsigned char)c.y, (unsigned char)c.y,
          (unsigned char)c.y, (unsigned char)c.y,
          (unsigned char)c.u, (unsigned char)c.v};
      std::vector<unsigned char> rgba(2 * 2 * 4);
      CHECK(ConvertFrame(kFourccNV12, nv12.data(), nv12.size(), 2, 2, 0,
                         rgba.data(), 4));
      for (int p = 0; p < 4; ++p) {
        CHECK(Near(rgba[p * 4 + 0], c.r));
        CHECK(Near(rgba[p * 4 + 1], c.g));
        CHECK(Near(rgba[p * 4 + 2], c.b));
        CHECK(rgba[p * 4 + 3] == 255);
      }
    }
  }

  // NV12 with padded rows: the padding is skipped, not converted.
  {
    const uint32_t w = 2, h = 2, stride = 8;
    std::vector<unsigned char> nv12(FrameBytes(kFourccNV12, w, h, stride), 0xAA);
    nv12[0] = nv12[1] = nv12[stride] = nv12[stride + 1] = 128;  // Y
    nv12[stride * h] = 128;                                      // U
    nv12[stride * h + 1] = 128;                                  // V
    std::vector<unsigned char> rgb(w * h * 3);
    CHECK(ConvertFrame(kFourccNV12, nv12.data(), nv12.size(), w, h, stride,
                       rgb.data(), 3));
    for (unsigned char c : rgb) CHECK(Near(c, 128));
  }

  // YUYV: same results as the old double-based converter for one pair.
  {
    std::vector<unsigned char> yuyv = {76, 85, 149, 255};  // Y0 U Y1 V
    std::vector<unsigned char> rgb(2 * 3);
    CHECK(ConvertFrame(kFourccYUYV, yuyv.data(), yuyv.size(), 2, 1, 0,
                       rgb.data(), 3));
    CHECK(Near(rgb[0], 254));  // 76 + 1.402 * 127
    CHECK(Near(rgb[1], 0));
    CHECK(Near(rgb[2], 0));    // 76 + 1.772 * -43 = -0.2, clamped
    CHECK(Near(rgb[3], 255));  // 149 + 178 clamps
  }

  // Odd width: no read past a row (ASan reports it if there is one).
  {
    for (uint32_t fourcc : kSupportedFormats) {
      const uint32_t w = 3, h = 3;
      std::vector<unsigned char> frame(FrameBytes(fourcc, w, h, 0), 128);
      std::vector<unsigned char> rgba(w * h * 4);
      CHECK(ConvertFrame(fourcc, frame.data(), frame.size(), w, h, 0,
                         rgba.data(), 4));
    }
  }

  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("pixel_format_test: all passed\n");
  return 0;
}
