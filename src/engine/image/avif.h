// Kite Engine - AVIF images (HEIF container + AV1 via dav1d)
#ifndef KITE_IMAGE_AVIF_H
#define KITE_IMAGE_AVIF_H

#include <cstdint>
#include <string>
#include <vector>

namespace kite {

bool LooksLikeAvif(const std::string& data);
// Decodes the primary image into premultiplied BGRA.
bool DecodeAvif(const std::string& data, int& width, int& height, std::vector<uint32_t>& pixels,
                bool& hasAlpha);

}  // namespace kite

#endif
