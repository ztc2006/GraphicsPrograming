#include "texture.hpp"
#include "pch.hpp"
#include "upload_batch.hpp"
#include <fstream>
#include <limits>
#include <stb_image.h>
#include <stdexcept>

namespace {
struct TextureImageResources {
  GpuImage allocation;
  vk::Format format = vk::Format::eUndefined;
};
} // namespace
struct TextureSamplerResources {
  ResourceLedger::Lease accounting;
  vk::raii::Sampler handle = nullptr;
};
struct TextureViewResources {
  ResourceLedger::Lease accounting;
  std::shared_ptr<TextureImageResources> storage;
  vk::raii::ImageView view = nullptr;
};

vk::Image TextureResources::image() const {
  return view_ ? *view_->storage->allocation.image : vk::Image{};
}
vk::ImageView TextureResources::imageView() const {
  return view_ ? *view_->view : vk::ImageView{};
}
vk::Sampler TextureResources::sampler() const {
  return sampler_ ? *sampler_->handle : vk::Sampler{};
}
vk::Format TextureResources::format() const {
  return view_ ? view_->storage->format : vk::Format::eUndefined;
}

TextureResources
TextureLoader::createFromFile(std::string const &path,
                              TextureColorSpace colorSpace) const {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() <= 0 ||
      file.tellg() > std::numeric_limits<int>::max())
    throw std::runtime_error("Failed to read texture: " + path);
  std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
    throw std::runtime_error("Failed to read complete texture: " + path);
  return createFromEncodedBytes(bytes, path, colorSpace);
}

std::shared_ptr<TextureViewResources> TextureLoader::decodeImage(
    std::span<std::byte const> bytes, std::string const &label,
    TextureColorSpace colorSpace, UploadBatch &batch) const {
  if (bytes.empty() ||
      bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::runtime_error("Encoded texture size is invalid: " + label);
  int width = 0, height = 0, channels = 0;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
      stbi_load_from_memory(reinterpret_cast<stbi_uc const *>(bytes.data()),
                            static_cast<int>(bytes.size()), &width, &height,
                            &channels, STBI_rgb_alpha),
      stbi_image_free);
  if (!pixels || width <= 0 || height <= 0)
    throw std::runtime_error("Failed to decode texture: " + label);
  return uploadImage(
      std::as_bytes(std::span(pixels.get(),
                              static_cast<std::size_t>(width) * height * 4)),
      width, height, textureFormat(colorSpace), batch);
}

TextureResources
TextureLoader::createFromEncodedBytes(std::span<std::byte const> bytes,
                                      std::string const &label,
                                      TextureColorSpace colorSpace) const {
  UploadBatch batch(device_);
  TextureResources result;
  result.view_ = decodeImage(bytes, label, colorSpace, batch);
  result.sampler_ = createSampler({});
  batch.finish();
  return result;
}

TextureResources
TextureLoader::createSolidColor(std::array<std::uint8_t, 4> const &color,
                                TextureColorSpace colorSpace) const {
  UploadBatch batch(device_);
  TextureResources result;
  result.view_ = uploadImage(std::as_bytes(std::span(color)), 1, 1,
                             textureFormat(colorSpace), batch);
  result.sampler_ = createSampler({});
  batch.finish();
  return result;
}

TextureResources
TextureLoader::createFromHdrPixels(std::span<float const> rgba,
                                   std::uint32_t width,
                                   std::uint32_t height) const {
  if (!width || !height ||
      rgba.size() != static_cast<std::uint64_t>(width) * height * 4)
    throw std::runtime_error("HDR pixel buffer dimensions do not match data.");
  UploadBatch batch(device_);
  TextureResources result;
  result.view_ = uploadImage(std::as_bytes(rgba), width, height,
                             vk::Format::eR32G32B32A32Sfloat, batch);
  result.sampler_ = createSampler({});
  batch.finish();
  return result;
}

std::shared_ptr<TextureViewResources>
TextureLoader::uploadImage(std::span<std::byte const> pixels,
                           std::uint32_t width, std::uint32_t height,
                           vk::Format format, UploadBatch &batch) const {
  auto storage = std::make_shared<TextureImageResources>();
  storage->format = format;
  storage->allocation = device_.createImage(
      vk::ImageCreateInfo{.imageType = vk::ImageType::e2D,
                          .format = format,
                          .extent = {width, height, 1},
                          .mipLevels = 1,
                          .arrayLayers = 1,
                          .samples = vk::SampleCountFlagBits::e1,
                          .tiling = vk::ImageTiling::eOptimal,
                          .usage = vk::ImageUsageFlagBits::eTransferDst |
                                   vk::ImageUsageFlagBits::eTransferSrc |
                                   vk::ImageUsageFlagBits::eSampled,
                          .sharingMode = vk::SharingMode::eExclusive,
                          .initialLayout = vk::ImageLayout::eUndefined},
      pixels.size_bytes(), vk::MemoryPropertyFlagBits::eDeviceLocal, scope_);
  auto view = std::make_shared<TextureViewResources>();
  view->storage = std::move(storage);
  view->view = vk::raii::ImageView(
      device_.logicalDevice(),
      vk::ImageViewCreateInfo{
          .image = *view->storage->allocation.image,
          .viewType = vk::ImageViewType::e2D,
          .format = format,
          .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
  view->accounting = scope_.track({.imageViews = 1});
  batch.copyImage(pixels, *view->storage->allocation.image, width, height);
  return view;
}

std::shared_ptr<TextureSamplerResources> TextureLoader::createSampler(
    TextureSamplerDescription const &description) const {
  auto filter = [](TextureFilter f) {
    return f == TextureFilter::Linear ? vk::Filter::eLinear
                                      : vk::Filter::eNearest;
  };
  auto wrap = [](TextureWrap w) {
    switch (w) {
    case TextureWrap::ClampToEdge:
      return vk::SamplerAddressMode::eClampToEdge;
    case TextureWrap::MirroredRepeat:
      return vk::SamplerAddressMode::eMirroredRepeat;
    default:
      return vk::SamplerAddressMode::eRepeat;
    }
  };
  auto result = std::make_shared<TextureSamplerResources>();
  result->handle = vk::raii::Sampler(
      device_.logicalDevice(),
      vk::SamplerCreateInfo{.magFilter = filter(description.mag),
                            .minFilter = filter(description.min),
                            .mipmapMode =
                                description.mip == TextureMipFilter::Linear
                                    ? vk::SamplerMipmapMode::eLinear
                                    : vk::SamplerMipmapMode::eNearest,
                            .addressModeU = wrap(description.u),
                            .addressModeV = wrap(description.v),
                            .addressModeW = wrap(description.w),
                            .maxAnisotropy = 1.0f,
                            .compareOp = vk::CompareOp::eAlways,
                            // Mip generation is a separate milestone. Current
                            // images have only LOD 0.
                            .minLod = 0.0f,
                            .maxLod = 0.0f,
                            .borderColor = vk::BorderColor::eIntOpaqueBlack});
  result->accounting = scope_.track({.samplers = 1});
  return result;
}
