// PNG and JPEG pictures for the launcher (picture_decode.h): stb_image from the SDK copy's thirdparty/stb, with its
// functions static (STB_IMAGE_STATIC), so they never meet the SDK's own PNG only copy (rex::ui::DecodeImageRGBA).

#include "picture_decode.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_FAILURE_STRINGS
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cstring>

namespace bf1943 {

std::vector<uint8_t> DecodePicture(const uint8_t* data, size_t size, int& width, int& height) {
  width = height = 0;
  if (!data || size == 0 || size > 0x7fffffff) return {};
  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(data, int(size), &w, &h, &channels, 4);
  if (!pixels || w <= 0 || h <= 0) {
    if (pixels) stbi_image_free(pixels);
    return {};
  }
  std::vector<uint8_t> out(size_t(w) * size_t(h) * 4);
  std::memcpy(out.data(), pixels, out.size());
  stbi_image_free(pixels);
  width = w, height = h;
  return out;
}

}  // namespace bf1943
