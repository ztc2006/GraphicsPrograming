#pragma once
#include "device.hpp"
#include "texture_mip.hpp"
#include "texture_sampler.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

class UploadBatch;
class TextureCache;
struct TextureViewResources;
struct TextureSamplerResources;

constexpr vk::Format textureFormat(TextureColorSpace colorSpace) {
  return colorSpace == TextureColorSpace::Srgb ? vk::Format::eR8G8B8A8Srgb
                                               : vk::Format::eR8G8B8A8Unorm;
}

// A material binding keeps the image/view and sampler alive independently.
class TextureResources {
public:
  TextureResources() = default;
  vk::Image image() const;
  vk::ImageView imageView() const;
  vk::Sampler sampler() const;
  vk::Format format() const;
  std::uint32_t mipLevels() const;
  float maxAnisotropy() const;
  bool opaqueAlpha() const;

private:
  friend class TextureLoader;
  friend class TextureCache;
  std::shared_ptr<TextureViewResources> view_;
  std::shared_ptr<TextureSamplerResources> sampler_;
};

class TextureLoader {
public:
  explicit TextureLoader(Device const &device, ResourceLedger::Scope scope = {})
      : device_(device),
        scope_(scope ? std::move(scope)
                     : device.resourceLedger().scope(
                           ResourceLedger::Domain::SharedTextures)) {}
  TextureResources createFromFile(
      std::string const &path,
      TextureColorSpace colorSpace = TextureColorSpace::Linear) const;
  TextureResources createFromEncodedBytes(
      std::span<std::byte const> bytes, std::string const &label,
      TextureColorSpace colorSpace = TextureColorSpace::Linear) const;
  TextureResources createSolidColor(
      std::array<std::uint8_t, 4> const &color,
      TextureColorSpace colorSpace = TextureColorSpace::Linear) const;
  TextureResources createFromHdrPixels(std::span<float const> rgba,
                                       std::uint32_t width,
                                       std::uint32_t height) const;
  TextureResources
  createFromHdrPixels(std::span<float const> rgba, std::uint32_t width,
                      std::uint32_t height, UploadBatch &batch,
                      TextureWrap wrapU = TextureWrap::Repeat) const;
  // Six faces per level, full chain; scene-linear float values, no mip
  // generation.
  TextureResources createFromHdrCube(std::span<float const> rgba,
                                     std::uint32_t faceSize,
                                     UploadBatch &batch) const;

private:
  friend class TextureCache;
  std::shared_ptr<TextureViewResources>
  decodeImage(std::span<std::byte const> bytes, std::string const &label,
              TextureColorSpace colorSpace, UploadBatch &batch,
              TextureMipPolicy policy = TextureMipPolicy::Average,
              TextureAlphaCoverage coverage = {}) const;
  std::shared_ptr<TextureViewResources>
  uploadImage(std::span<std::byte const> pixels,
              std::span<TextureMipLevel const> levels, vk::Format format,
              UploadBatch &batch, bool opaqueAlpha = true,
              std::uint32_t arrayLayers = 1) const;
  std::shared_ptr<TextureViewResources>
  baseLevelView(std::shared_ptr<TextureViewResources> const &) const;
  std::shared_ptr<TextureSamplerResources>
  createSampler(TextureSamplerDescription const &) const;
  Device const &device_;
  ResourceLedger::Scope scope_;
};
