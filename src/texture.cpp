#include "texture.hpp"
#include "pch.hpp"
#include "upload_batch.hpp"
#include <algorithm>
#include <fstream>
#include <limits>
#include <stb_image.h>
#include <stdexcept>

namespace {
struct TextureImageResources {
  GpuImage allocation;
  vk::Format format = vk::Format::eUndefined;
  std::uint32_t mipLevels = 0;
  bool opaqueAlpha = true;
};
} // namespace
struct TextureSamplerResources {
  ResourceLedger::Lease accounting;
  vk::raii::Sampler handle = nullptr;
  float maxAnisotropy = 1.0f;
};
struct TextureViewResources {
  ResourceLedger::Lease accounting;
  std::shared_ptr<TextureImageResources> storage;
  vk::raii::ImageView view = nullptr;
  std::uint32_t mipLevels = 0;
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
std::uint32_t TextureResources::mipLevels() const {
  return view_ ? view_->mipLevels : 0;
}
bool TextureResources::opaqueAlpha() const {
  return view_ && view_->storage->opaqueAlpha;
}
float TextureResources::maxAnisotropy() const {
  return sampler_ ? sampler_->maxAnisotropy : 1.0f;
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
    TextureColorSpace colorSpace, UploadBatch &batch, TextureMipPolicy policy,
    TextureAlphaCoverage coverage) const {
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
  auto limit =
      device_.physicalDevice().getProperties().limits.maxImageDimension2D;
  if (std::uint32_t(width) > limit || std::uint32_t(height) > limit)
    throw std::runtime_error("Texture exceeds device image dimension limit: " +
                             label);
  bool opaque = true;
  for (std::size_t i = 3; i < static_cast<std::size_t>(width) * height * 4;
       i += 4)
    opaque &= pixels.get()[i] == 255;
  auto chain = generateTextureMips(
      std::as_bytes(std::span(pixels.get(),
                              static_cast<std::size_t>(width) * height * 4)),
      width, height, colorSpace, policy, coverage);
  pixels.reset(); // Reclaim decoded LOD 0 before allocating the staging copy.
  return uploadImage(chain.pixels, chain.levels, textureFormat(colorSpace),
                     batch, opaque);
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
  result.view_ = uploadImage(std::as_bytes(std::span(color)),
                             textureMipLayout(1, 1, 4, false),
                             textureFormat(colorSpace), batch, color[3] == 255);
  result.sampler_ = createSampler({});
  batch.finish();
  return result;
}

TextureResources
TextureLoader::createFromHdrPixels(std::span<float const> rgba,
                                   std::uint32_t width,
                                   std::uint32_t height) const {
  UploadBatch batch(device_);
  auto result = createFromHdrPixels(rgba, width, height, batch);
  batch.finish();
  return result;
}

TextureResources TextureLoader::createFromHdrPixels(std::span<float const> rgba,
                                                    std::uint32_t width,
                                                    std::uint32_t height,
                                                    UploadBatch &batch,
                                                    TextureWrap wrapU) const {
  if (!width || !height ||
      rgba.size() != static_cast<std::uint64_t>(width) * height * 4)
    throw std::runtime_error("HDR pixel buffer dimensions do not match data.");
  TextureResources result;
  bool opaque = true;
  for (std::size_t i = 3; i < rgba.size(); i += 4)
    opaque &= rgba[i] == 1.0f;
  result.view_ = uploadImage(std::as_bytes(rgba),
                             textureMipLayout(width, height, 16, false),
                             vk::Format::eR32G32B32A32Sfloat, batch, opaque);
  result.sampler_ = createSampler({.mip = TextureMipFilter::None,
                                   .u = wrapU,
                                   .v = TextureWrap::ClampToEdge,
                                   .maxAnisotropy = 1});
  return result;
}

TextureResources TextureLoader::createFromHdrCube(std::span<float const> rgba,
                                                  std::uint32_t faceSize,
                                                  UploadBatch &batch) const {
  auto levels = textureMipLayout(faceSize, faceSize, 16, true);
  for (auto &level : levels) {
    level.offset *= 6;
    level.size *= 6;
  }
  if (rgba.size_bytes() != levels.back().offset + levels.back().size)
    throw std::runtime_error(
        "HDR cube requires six faces and a complete chain");
  TextureResources result;
  result.view_ = uploadImage(std::as_bytes(rgba), levels,
                             vk::Format::eR32G32B32A32Sfloat, batch, true, 6);
  result.sampler_ = createSampler({.u = TextureWrap::ClampToEdge,
                                   .v = TextureWrap::ClampToEdge,
                                   .w = TextureWrap::ClampToEdge,
                                   .maxAnisotropy = 1});
  return result;
}

std::shared_ptr<TextureViewResources>
TextureLoader::uploadImage(std::span<std::byte const> pixels,
                           std::span<TextureMipLevel const> levels,
                           vk::Format format, UploadBatch &batch,
                           bool opaqueAlpha, std::uint32_t arrayLayers) const {
  if (levels.empty())
    throw std::runtime_error("Texture requires at least one mip.");
  bool cube = arrayLayers == 6;
  auto limit = device_.physicalDevice().getProperties().limits;
  if (levels[0].width >
          (cube ? limit.maxImageDimensionCube : limit.maxImageDimension2D) ||
      levels[0].height >
          (cube ? limit.maxImageDimensionCube : limit.maxImageDimension2D))
    throw std::runtime_error("Texture exceeds device image dimension limit");
  if (format == vk::Format::eR32G32B32A32Sfloat) {
    auto required = vk::FormatFeatureFlagBits::eSampledImage |
                    vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
    if ((device_.physicalDevice()
             .getFormatProperties(format)
             .optimalTilingFeatures &
         required) != required)
      throw std::runtime_error(
          "Device cannot linearly sample RGBA32F HDR textures");
  }
  auto count = static_cast<std::uint32_t>(levels.size());
  auto storage = std::make_shared<TextureImageResources>();
  storage->format = format;
  storage->mipLevels = count;
  storage->opaqueAlpha = opaqueAlpha;
  storage->allocation = device_.createImage(
      vk::ImageCreateInfo{
          .flags = cube ? vk::ImageCreateFlags(
                              vk::ImageCreateFlagBits::eCubeCompatible)
                        : vk::ImageCreateFlags{},
          .imageType = vk::ImageType::e2D,
          .format = format,
          .extent = {levels[0].width, levels[0].height, 1},
          .mipLevels = count,
          .arrayLayers = arrayLayers,
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
  view->mipLevels = count;
  view->view = vk::raii::ImageView(
      device_.logicalDevice(),
      vk::ImageViewCreateInfo{
          .image = *view->storage->allocation.image,
          .viewType = cube ? vk::ImageViewType::eCube : vk::ImageViewType::e2D,
          .format = format,
          .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, count, 0,
                               arrayLayers}});
  view->accounting = scope_.track({.imageViews = 1});
  batch.copyImage(pixels, *view->storage->allocation.image, levels,
                  format == vk::Format::eR32G32B32A32Sfloat ? 16 : 4,
                  arrayLayers);
  return view;
}

std::shared_ptr<TextureViewResources> TextureLoader::baseLevelView(
    std::shared_ptr<TextureViewResources> const &source) const {
  if (source->mipLevels == 1)
    return source;
  auto view = std::make_shared<TextureViewResources>();
  view->storage = source->storage;
  view->mipLevels = 1;
  view->view = vk::raii::ImageView(
      device_.logicalDevice(),
      vk::ImageViewCreateInfo{
          .image = *view->storage->allocation.image,
          .viewType = vk::ImageViewType::e2D,
          .format = view->storage->format,
          .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
  view->accounting = scope_.track({.imageViews = 1});
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
  bool linearMips = description.mag == TextureFilter::Linear &&
                    description.min == TextureFilter::Linear &&
                    description.mip == TextureMipFilter::Linear;
  result->maxAnisotropy = linearMips
                              ? std::clamp(float(description.maxAnisotropy),
                                           1.0f, device_.maxSamplerAnisotropy())
                              : 1.0f;
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
                            .anisotropyEnable = result->maxAnisotropy > 1.0f,
                            .maxAnisotropy = result->maxAnisotropy,
                            .compareOp = vk::CompareOp::eAlways,
                            // View limits the chain. 0.25 preserves min/mag
                            // selection for a non-mip glTF sampler.
                            .minLod = 0.0f,
                            .maxLod = description.mip == TextureMipFilter::None
                                          ? .25f
                                          : VK_LOD_CLAMP_NONE,
                            .borderColor = vk::BorderColor::eIntOpaqueBlack});
  result->accounting = scope_.track({.samplers = 1});
  return result;
}
