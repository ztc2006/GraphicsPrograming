#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>

#include <stb_image.h>

#include "hdr_image.hpp"

int main() {
  int rawWidth = 0;
  int rawHeight = 0;
  int rawChannels = 0;
  std::unique_ptr<float, decltype(&stbi_image_free)> rawPixels(
      stbi_loadf(TEST_HDR_PATH, &rawWidth, &rawHeight, &rawChannels,
                 STBI_rgb_alpha),
      stbi_image_free);
  if (!rawPixels) {
    std::cerr << "Failed to load raw HDR fixture\n";
    return 1;
  }

  HdrImage const image = loadHdrImage(TEST_HDR_PATH);
  if (image.width != 2048 || image.height != 1024) {
    std::cerr << "Unexpected HDR dimensions: " << image.width << 'x'
              << image.height << '\n';
    return 1;
  }
  if (image.rgba.size() !=
      static_cast<std::size_t>(image.width) * image.height * 4) {
    std::cerr << "HDR RGBA buffer size is incorrect\n";
    return 1;
  }
  float const maximum = *std::max_element(image.rgba.begin(), image.rgba.end());
  if (maximum <= 1.0f) {
    std::cerr << "HDR dynamic range was lost; maximum=" << maximum << '\n';
    return 1;
  }

  // The renderer follows the established OpenGL reference orientation: HDR
  // scanlines are flipped during decoding while regular material images are
  // not.
  std::size_t const rowValues = static_cast<std::size_t>(image.width) * 4;
  std::size_t const rawLastRow =
      static_cast<std::size_t>(rawHeight - 1) * rowValues;
  for (std::size_t value = 0; value < rowValues; ++value) {
    if (std::abs(image.rgba[value] - rawPixels.get()[rawLastRow + value]) >
        0.000001f) {
      std::cerr << "HDR scanlines were not vertically flipped\n";
      return 1;
    }
  }
  return 0;
}
