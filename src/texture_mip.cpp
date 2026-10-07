#include "texture_mip.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

std::vector<TextureMipLevel> textureMipLayout(std::uint32_t width,
                                              std::uint32_t height,
                                              std::size_t texelBytes,
                                              bool generate) {
  if (!width || !height || !texelBytes)
    throw std::runtime_error("Texture dimensions/texel size must be nonzero.");
  std::vector<TextureMipLevel> result;
  std::size_t offset = 0;
  for (;;) {
    auto limit = std::numeric_limits<std::size_t>::max();
    if (texelBytes > limit / width || texelBytes * width > limit / height)
      throw std::runtime_error("Texture mip size overflows.");
    auto size = texelBytes * width * height;
    if (size > limit - offset)
      throw std::runtime_error("Texture mip chain size overflows.");
    result.push_back({width, height, offset, size});
    offset += size;
    if (!generate || (width == 1 && height == 1))
      break;
    width = std::max(1u, width / 2);
    height = std::max(1u, height / 2);
  }
  return result;
}

namespace {
std::array<double, 256> const linearSrgb = [] {
  std::array<double, 256> table{};
  for (unsigned i = 0; i < table.size(); ++i) {
    double c = i / 255.0;
    table[i] = c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
  }
  return table;
}();
std::byte encode(double c) {
  return std::byte(
      static_cast<unsigned char>(std::lround(std::clamp(c, 0.0, 1.0) * 255)));
}
using Histogram = std::array<std::uint64_t, 256>;
Histogram histogram(std::span<std::byte const> pixels, unsigned channel) {
  Histogram result{};
  for (std::size_t i = channel; i < pixels.size(); i += 4)
    ++result[std::to_integer<unsigned>(pixels[i])];
  return result;
}
void preserveCoverage(std::span<std::byte> pixels, unsigned channel,
                      unsigned threshold, std::uint64_t basePassing,
                      std::uint64_t baseCount) {
  auto bins = histogram(pixels, channel);
  double bestScale = 1;
  long double bestError = std::numeric_limits<long double>::infinity();
  auto evaluate = [&](double scale) {
    std::uint64_t passing = 0;
    for (unsigned value = 0; value < bins.size(); ++value)
      if (std::lround(std::min(255.0, value * scale)) >= threshold)
        passing += bins[value];
    long double error =
        std::abs(static_cast<long double>(passing) * baseCount -
                 static_cast<long double>(basePassing) * (pixels.size() / 4));
    if (error < bestError ||
        (error == bestError && std::abs(scale - 1) < std::abs(bestScale - 1))) {
      bestError = error;
      bestScale = scale;
    }
  };
  evaluate(1);
  evaluate(0);
  // Coverage changes only at quantized byte crossings. A small positive
  // margin makes the half-integer rounding boundary independent of FP error.
  for (unsigned value = 1; value < bins.size(); ++value)
    if (bins[value])
      evaluate((threshold - 0.499999) / value);
  for (std::size_t i = channel; i < pixels.size(); i += 4)
    pixels[i] = encode(std::to_integer<unsigned>(pixels[i]) * bestScale / 255);
}
// Preserve the first moment independently of quantized unit-direction mips.
// glTF ignores normal-map alpha: we own it as length loss, including LOD0=0.
void normalMips(TextureMipChain &chain) {
  using Moment = std::array<float, 3>;
  std::vector<Moment> previous;
  for (std::size_t i = 3; i < chain.levels.front().size; i += 4)
    chain.pixels[i] = std::byte{0};
  for (std::size_t level = 1; level < chain.levels.size(); ++level) {
    auto const &src = chain.levels[level - 1], &dst = chain.levels[level];
    std::vector<Moment> next(std::size_t(dst.width) * dst.height);
    for (unsigned y = 0; y < dst.height; ++y)
      for (unsigned x = 0; x < dst.width; ++x) {
        double left = double(x) * src.width / dst.width;
        double right = double(x + 1) * src.width / dst.width;
        double top = double(y) * src.height / dst.height;
        double bottom = double(y + 1) * src.height / dst.height;
        std::array<double, 3> sum{};
        for (unsigned sy = unsigned(top); sy < unsigned(std::ceil(bottom));
             ++sy)
          for (unsigned sx = unsigned(left); sx < unsigned(std::ceil(right));
               ++sx) {
            double weight =
                (std::min(bottom, double(sy + 1)) - std::max(top, double(sy))) *
                (std::min(right, double(sx + 1)) - std::max(left, double(sx)));
            auto index = std::size_t(sy) * src.width + sx;
            std::array<double, 3> value{};
            if (level == 1) {
              for (unsigned c = 0; c < 3; ++c)
                value[c] =
                    (2 * std::to_integer<int>(chain.pixels[index * 4 + c]) -
                     255) /
                    255.0;
              double length =
                  std::sqrt(value[0] * value[0] + value[1] * value[1] +
                            value[2] * value[2]);
              if (length < 1e-12)
                value = {0, 0, 1};
              else
                for (auto &v : value)
                  v /= length;
            } else
              for (unsigned c = 0; c < 3; ++c)
                value[c] = previous[index][c];
            for (unsigned c = 0; c < 3; ++c)
              sum[c] += weight * value[c];
          }
        for (auto &v : sum)
          v /= (right - left) * (bottom - top);
        auto index = std::size_t(y) * dst.width + x;
        for (unsigned c = 0; c < 3; ++c)
          next[index][c] = float(sum[c]);
        double length =
            std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
        auto output = dst.offset + index * 4;
        chain.pixels[output + 3] = encode(1 - length);
        if (length < 1e-12)
          sum = {0, 0, 1};
        else
          for (auto &v : sum)
            v /= length;
        for (unsigned c = 0; c < 3; ++c)
          chain.pixels[output + c] = encode(sum[c] * .5 + .5);
      }
    previous = std::move(next);
  }
}
} // namespace
TextureMipChain generateTextureMips(std::span<std::byte const> rgba,
                                    std::uint32_t width, std::uint32_t height,
                                    TextureColorSpace colorSpace,
                                    TextureMipPolicy policy,
                                    TextureAlphaCoverage coverage) {
  if (policy == TextureMipPolicy::Normal &&
      colorSpace != TextureColorSpace::Linear)
    throw std::runtime_error("Normal mips require linear texture storage.");
  if (policy == TextureMipPolicy::AlphaCoverage &&
      (!std::isfinite(coverage.cutoff) || coverage.cutoff < 0 ||
       (coverage.channel != TextureAlphaChannel::Alpha &&
        coverage.channel != TextureAlphaChannel::Red) ||
       (coverage.channel == TextureAlphaChannel::Red &&
        colorSpace != TextureColorSpace::Linear)))
    throw std::runtime_error("Invalid alpha coverage channel/cutoff/storage.");
  TextureMipChain result;
  result.levels =
      textureMipLayout(width, height, 4, policy != TextureMipPolicy::BaseOnly);
  if (rgba.size() != result.levels.front().size)
    throw std::runtime_error("RGBA8 mip source dimensions do not match data.");
  result.pixels.resize(result.levels.back().offset + result.levels.back().size);
  std::copy(rgba.begin(), rgba.end(), result.pixels.begin());
  if (policy == TextureMipPolicy::Normal) {
    normalMips(result);
    return result;
  }
  unsigned threshold = 0;
  while (threshold < 256 && float(threshold) / 255 < coverage.cutoff)
    ++threshold;
  auto channel = static_cast<unsigned>(coverage.channel);
  std::uint64_t basePassing = 0;
  if (policy == TextureMipPolicy::AlphaCoverage && threshold > 0 &&
      threshold < 256) {
    auto bins = histogram(rgba, channel);
    for (unsigned i = threshold; i < bins.size(); ++i)
      basePassing += bins[i];
  }
  for (std::size_t level = 1; level < result.levels.size(); ++level) {
    auto const &src = result.levels[level - 1], &dst = result.levels[level];
    for (std::uint32_t y = 0; y < dst.height; ++y) {
      double top = double(y) * src.height / dst.height;
      double bottom = double(y + 1) * src.height / dst.height;
      for (std::uint32_t x = 0; x < dst.width; ++x) {
        double left = double(x) * src.width / dst.width;
        double right = double(x + 1) * src.width / dst.width;
        std::array<double, 4> sum{};
        for (auto sy = std::uint32_t(top);
             sy < std::uint32_t(std::ceil(bottom)); ++sy) {
          double wy =
              std::min(bottom, double(sy + 1)) - std::max(top, double(sy));
          for (auto sx = std::uint32_t(left);
               sx < std::uint32_t(std::ceil(right)); ++sx) {
            double weight = wy * (std::min(right, double(sx + 1)) -
                                  std::max(left, double(sx)));
            auto index = src.offset + (std::size_t(sy) * src.width + sx) * 4;
            for (unsigned c = 0; c < 4; ++c) {
              int byte = std::to_integer<int>(result.pixels[index + c]);
              double value = byte / 255.0;
              if (c < 3 && colorSpace == TextureColorSpace::Srgb)
                value = linearSrgb[byte];
              sum[c] += value * weight;
            }
          }
        }
        for (auto &v : sum)
          v /= (right - left) * (bottom - top);
        if (colorSpace == TextureColorSpace::Srgb) {
          for (unsigned c = 0; c < 3; ++c)
            sum[c] = sum[c] <= .0031308
                         ? 12.92 * sum[c]
                         : 1.055 * std::pow(sum[c], 1.0 / 2.4) - .055;
        }
        auto index = dst.offset + (std::size_t(y) * dst.width + x) * 4;
        for (unsigned c = 0; c < 4; ++c)
          result.pixels[index + c] = encode(sum[c]);
      }
    }
    if (policy == TextureMipPolicy::AlphaCoverage && threshold > 0 &&
        threshold < 256)
      preserveCoverage(std::span(result.pixels).subspan(dst.offset, dst.size),
                       channel, threshold, basePassing, rgba.size() / 4);
  }
  return result;
}
