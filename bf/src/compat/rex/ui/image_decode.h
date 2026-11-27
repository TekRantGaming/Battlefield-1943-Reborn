// Battlefield 1943 Reborn, SDK v0.11+ port (phase 3): v0.10's rex/ui/image_decode.h. v0.11 keeps its decoder private
// to its UI module; the game's own (picture_decode.cpp, stb_image: PNG and JPEG) decodes the same pictures.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../../picture_decode.h"

namespace rex::ui {
inline std::vector<uint8_t> DecodeImageRGBA(const uint8_t* data, size_t size, int& width, int& height) {
  return bf1943::DecodePicture(data, size, width, height);
}
}  // namespace rex::ui
