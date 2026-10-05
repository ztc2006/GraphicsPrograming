#pragma once
#include <compare>
#include <cstdint>

enum class TextureFilter { Nearest, Linear };
enum class TextureMipFilter { None, Nearest, Linear };
enum class TextureWrap { Repeat, ClampToEdge, MirroredRepeat };

// CPU asset semantics; independent of image storage and Vulkan descriptors.
struct TextureSamplerDescription {
  TextureFilter mag = TextureFilter::Linear;
  TextureFilter min = TextureFilter::Linear;
  TextureMipFilter mip = TextureMipFilter::Linear;
  TextureWrap u = TextureWrap::Repeat;
  TextureWrap v = TextureWrap::Repeat;
  TextureWrap w = TextureWrap::Repeat;
  // Engine quality request, not a glTF property. Nearest/non-mip modes ignore
  // it.
  std::uint8_t maxAnisotropy = 8;
  auto operator<=>(TextureSamplerDescription const &) const = default;
};
