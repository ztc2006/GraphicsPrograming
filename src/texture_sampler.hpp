#pragma once
#include <compare>

enum class TextureFilter { Nearest, Linear };
enum class TextureMipFilter { None, Nearest, Linear };
enum class TextureWrap { Repeat, ClampToEdge, MirroredRepeat };

// CPU asset semantics; independent of image storage and Vulkan descriptors.
struct TextureSamplerDescription {
  TextureFilter mag = TextureFilter::Linear;
  TextureFilter min = TextureFilter::Linear;
  TextureMipFilter mip = TextureMipFilter::Nearest;
  TextureWrap u = TextureWrap::Repeat;
  TextureWrap v = TextureWrap::Repeat;
  TextureWrap w = TextureWrap::Repeat;
  auto operator<=>(TextureSamplerDescription const &) const = default;
};
