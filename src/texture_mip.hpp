#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

enum class TextureColorSpace { Linear, Srgb };
// Normal RGB: authored LOD0 / normalized mip direction. A: 1-|mean|; LOD0=0.
// All other policies retain authored alpha/coverage semantics.
enum class TextureMipPolicy { Average, Normal, BaseOnly, AlphaCoverage };
enum class TextureAlphaChannel : std::uint8_t { Red = 0, Alpha = 3 };
struct TextureAlphaCoverage {
  float cutoff = 0.5f; // Effective cutoff: material cutoff / constant alpha.
  TextureAlphaChannel channel = TextureAlphaChannel::Alpha;
};
inline constexpr std::uint8_t textureMipAlgorithmVersion = 3;

// Tightly packed levels; RGBA8 offsets are also Vulkan copy-aligned.
struct TextureMipLevel {
  std::uint32_t width, height;
  std::size_t offset, size;
  auto operator<=>(TextureMipLevel const &) const = default;
};
struct TextureMipChain {
  std::vector<std::byte> pixels;
  std::vector<TextureMipLevel> levels;
};
std::vector<TextureMipLevel> textureMipLayout(std::uint32_t width,
                                              std::uint32_t height,
                                              std::size_t texelBytes,
                                              bool generate);
TextureMipChain
generateTextureMips(std::span<std::byte const> rgba, std::uint32_t width,
                    std::uint32_t height, TextureColorSpace colorSpace,
                    TextureMipPolicy policy = TextureMipPolicy::Average,
                    TextureAlphaCoverage coverage = {});
