// A PNG or JPEG picture decoded to RGBA (8 bits a channel, rows top first), for the launcher's pictures: More from TRG
// Studios' tiles (SteamGridDB's and GitHub's are PNG or JPEG) and any picture file it draws. The SDK's own decoder
// (rex::ui::DecodeImageRGBA) reads PNG only. Empty when it is not one it can read.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bf1943 {
std::vector<uint8_t> DecodePicture(const uint8_t* data, size_t size, int& width, int& height);
}
