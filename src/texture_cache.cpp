#include "texture_cache.hpp"
#include "upload_batch.hpp"
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

TextureResources
TextureCache::bind(std::shared_ptr<TextureViewResources> view,
                   TextureSamplerDescription const &description) {
  auto &entry = samplers_[description];
  auto sampler = entry.lock();
  if (!sampler) {
    sampler = loader_.createSampler(description);
    entry = sampler;
  }
  TextureResources result;
  result.view_ = std::move(view);
  result.sampler_ = std::move(sampler);
  return result;
}

std::shared_ptr<TextureViewResources>
TextureCache::reusable(ImageEntry const &entry,
                       UploadBatch const &batch) const {
  if (!entry.upload ||
      (entry.upload != batch.ticket() &&
       !entry.upload->submitted.load(std::memory_order_acquire)))
    return {};
  return entry.view.lock();
}

TextureResources TextureCache::encoded(std::span<std::byte const> bytes,
                                       std::string const &label,
                                       TextureColorSpace colorSpace,
                                       TextureSamplerDescription const &sampler,
                                       UploadBatch &batch,
                                       TextureMipPolicy policy,
                                       TextureAlphaCoverage coverage) {
  if (bytes.empty() ||
      bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::runtime_error("Encoded texture size is invalid: " + label);
  std::string key{static_cast<char>(0), static_cast<char>(colorSpace),
                  static_cast<char>(policy),
                  static_cast<char>(textureMipAlgorithmVersion)};
  if (policy == TextureMipPolicy::AlphaCoverage) {
    if (!std::isfinite(coverage.cutoff) || coverage.cutoff < 0)
      throw std::runtime_error("Invalid alpha coverage cutoff: " + label);
    auto bits = std::bit_cast<std::uint32_t>(
        coverage.cutoff == 0 ? 0.0f : coverage.cutoff);
    for (unsigned shift = 0; shift < 32; shift += 8)
      key.push_back(static_cast<char>((bits >> shift) & 255));
    key.push_back(static_cast<char>(coverage.channel));
  }
  key.append(reinterpret_cast<char const *>(bytes.data()), bytes.size());
  auto &entry = views_[key];
  auto view = reusable(entry, batch);
  if (!view) {
    view =
        loader_.decodeImage(bytes, label, colorSpace, batch, policy, coverage);
    entry = ImageEntry{view, batch.ticket()};
  }
  return bind(std::move(view), sampler);
}

TextureResources TextureCache::file(std::string const &path,
                                    TextureColorSpace colorSpace,
                                    TextureSamplerDescription const &sampler,
                                    UploadBatch &batch, TextureMipPolicy policy,
                                    TextureAlphaCoverage coverage) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  auto size = input ? input.tellg() : std::streampos(-1);
  if (size <= 0 || size > std::numeric_limits<int>::max())
    throw std::runtime_error("Texture file size is invalid: " + path);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
    throw std::runtime_error("Failed to read complete texture: " + path);
  return encoded(bytes, path, colorSpace, sampler, batch, policy, coverage);
}

TextureResources
TextureCache::baseLevelBinding(TextureResources const &source,
                               TextureSamplerDescription sampler) {
  auto &entry = baseViews_[source.image()];
  auto view = entry.lock();
  if (!view) {
    view = loader_.baseLevelView(source.view_);
    entry = view;
  }
  sampler.mip = TextureMipFilter::None;
  sampler.maxAnisotropy = 1;
  return bind(std::move(view), sampler);
}

TextureResources TextureCache::solid(std::array<std::uint8_t, 4> const &color,
                                     TextureColorSpace colorSpace,
                                     UploadBatch &batch) {
  std::string key{static_cast<char>(1), static_cast<char>(colorSpace),
                  static_cast<char>(TextureMipPolicy::BaseOnly),
                  static_cast<char>(textureMipAlgorithmVersion)};
  key.append(reinterpret_cast<char const *>(color.data()), color.size());
  auto &entry = views_[key];
  auto view = reusable(entry, batch);
  if (!view) {
    view = loader_.uploadImage(
        std::as_bytes(std::span(color)), textureMipLayout(1, 1, 4, false),
        textureFormat(colorSpace), batch, color[3] == 255);
    entry = ImageEntry{view, batch.ticket()};
  }
  return bind(std::move(view), {});
}

void TextureCache::pruneExpired() {
  std::erase_if(views_,
                [](auto const &entry) { return entry.second.view.expired(); });
  std::erase_if(samplers_,
                [](auto const &entry) { return entry.second.expired(); });
  std::erase_if(baseViews_,
                [](auto const &entry) { return entry.second.expired(); });
}
