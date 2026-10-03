#include "hdr_image.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

#include <stb_image.h>

HdrImage loadHdrImage(std::filesystem::path const &path) {
  std::string const pathString = path.string();
  if (stbi_is_hdr(pathString.c_str()) == 0) {
    throw std::runtime_error("Environment image is not Radiance HDR: " +
                             pathString);
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  std::unique_ptr<float, decltype(&stbi_image_free)> pixels(
      stbi_loadf(pathString.c_str(), &width, &height, &channels,
                 STBI_rgb_alpha),
      stbi_image_free);
  if (!pixels) {
    throw std::runtime_error("Failed to load HDR environment: " + pathString +
                             " (" + stbi_failure_reason() + ")");
  }
  if (width <= 0 || height <= 0) {
    throw std::runtime_error("HDR environment has invalid dimensions: " +
                             pathString);
  }

  std::size_t const valueCount =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
  HdrImage image{
      .width = static_cast<std::uint32_t>(width),
      .height = static_cast<std::uint32_t>(height),
      .rgba = std::vector<float>(pixels.get(), pixels.get() + valueCount),
  };

  std::size_t const rowValues = static_cast<std::size_t>(width) * 4;
  for (int row = 0; row < height / 2; ++row) {
    auto top = image.rgba.begin() + static_cast<std::size_t>(row) * rowValues;
    auto bottom = image.rgba.begin() +
                  static_cast<std::size_t>(height - 1 - row) * rowValues;
    std::swap_ranges(top, top + rowValues, bottom);
  }
  return image;
}
