#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

struct HdrImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<float> rgba;
};

HdrImage loadHdrImage(std::filesystem::path const &path);
