#pragma once
#include "texture.hpp"
#include "upload_batch.hpp"
#include <map>
#include <unordered_map>

// Renderer-local cache used by serialized preparation tasks. Main-thread upload
// publication uses atomic tickets; weak entries never pin unused GPU resources.
// Candidates retain strong bindings until upload/commit or rollback.
class TextureCache {
public:
  explicit TextureCache(Device const &device) : loader_(device) {}
  TextureResources encoded(std::span<std::byte const> bytes,
                           std::string const &label,
                           TextureColorSpace colorSpace,
                           TextureSamplerDescription const &sampler,
                           UploadBatch &batch);
  TextureResources file(std::string const &path, TextureColorSpace colorSpace,
                        TextureSamplerDescription const &sampler,
                        UploadBatch &batch);
  TextureResources solid(std::array<std::uint8_t, 4> const &color,
                         TextureColorSpace colorSpace, UploadBatch &batch);
  void pruneExpired();
  std::size_t imageEntries() const { return views_.size(); }
  std::size_t samplerEntries() const { return samplers_.size(); }

private:
  TextureResources bind(std::shared_ptr<TextureViewResources> view,
                        TextureSamplerDescription const &description);
  TextureLoader loader_;
  // Exact bytes avoid hash-collision aliases and detect files edited in place.
  // Encoded/solid and color space are part of the key; current mip policy is
  // LOD 0.
  struct ImageEntry {
    std::weak_ptr<TextureViewResources> view;
    std::shared_ptr<UploadBatch::Ticket const> upload;
  };
  // Other candidates can reuse only submitted uploads on our same queue.
  std::shared_ptr<TextureViewResources> reusable(ImageEntry const &,
                                                 UploadBatch const &) const;
  std::unordered_map<std::string, ImageEntry> views_;
  std::map<TextureSamplerDescription, std::weak_ptr<TextureSamplerResources>>
      samplers_;
};
