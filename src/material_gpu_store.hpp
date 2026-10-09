#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "asset_ids.hpp"
#include "device.hpp"
#include "scene.hpp"
#include "texture_cache.hpp"
#include "upload_batch.hpp"

class MaterialGpuStore {
public:
  struct MaterialUniformBufferObject {
    glm::vec4 pbrParams{0.0f, 1.0f, 1.0f, 0.0f};
    glm::vec4 emissiveFactor{0.0f};
    glm::vec4 specularColorAndWeight{1.0f};
    glm::vec4 optical{1.5f,1.f,0,0};
    glm::vec4 absorptionThickness{0};
  };
  static_assert(sizeof(MaterialUniformBufferObject) == 80 &&
                offsetof(MaterialUniformBufferObject, specularColorAndWeight) ==
                    32);

  struct MaterialGpuResources {
    TextureResources albedoTexture;
    TextureResources baseAlbedoTexture, baseAlphaTexture;
    std::optional<TextureResources> normalTexture;
    std::optional<TextureResources> heightTexture;
    std::optional<TextureResources> alphaTexture;
    std::optional<TextureResources> metallicRoughnessTexture;
    std::optional<TextureResources> occlusionTexture;
    std::optional<TextureResources> emissiveTexture;
    std::optional<TextureResources> specularTexture, specularColorTexture;
    Device::BufferResources uniform;
    vk::DescriptorSet descriptorSet = nullptr;
    glm::vec4 tint{1.0f};
    glm::vec4 surfaceParams{1.0f, 0.04f, 0.0f, 0.0f};
    glm::vec4 alphaParams{0.0f, 0.5f, 0.0f, 0.0f};
    OpticalMaterial optical;
    AlphaMode alphaMode = AlphaMode::Opaque;
    bool doubleSided = false, sourceAreaLight = false;
    bool hasCoverageMips = false;
    float coverageCutoff = 0.5f, coverageFactor = 1.0f;
  };

  // The renderer owns the stable layout; this store owns one scene's resources.
  static vk::raii::DescriptorSetLayout
  createDescriptorSetLayout(Device const &device);
  MaterialGpuStore(Device const &device, vk::DescriptorSetLayout layout,
                   std::vector<Material> const &materials,
                   TextureCache &textures, UploadBatch &uploads,
                   ResourceLedger::Scope scope);
  void setMaterialTint(MaterialId materialId, glm::vec4 const &tint);
  void setMaterialSurfaceParams(MaterialId materialId, float normalScale,
                                float parallaxScale);
  void setMaterialAlphaParams(MaterialId materialId, AlphaMode alphaMode,
                              float alphaCutoff);
  MaterialGpuResources const &material(MaterialId materialId) const;

private:
  static void updateCoverageState(MaterialGpuResources &);
  std::vector<MaterialGpuResources>
  createMaterialResources(std::vector<Material> const &materials,
                          TextureCache &textures, UploadBatch &uploads) const;

  vk::raii::DescriptorPool
  createMaterialDescriptorPool(std::uint32_t materialCount) const;
  void writeMaterialDescriptorSets();

private:
  Device const &device_;
  vk::DescriptorSetLayout materialDescriptorSetLayout_ = nullptr;
  ResourceLedger::Scope resourceScope_;
  ResourceLedger::Lease descriptorAccounting_;
  vk::raii::DescriptorPool materialDescriptorPool_ = nullptr;
  TextureResources flatNormalTexture_;
  TextureResources flatHeightTexture_;
  TextureResources whiteDataTexture_;
  TextureResources whiteColorTexture_;
  std::vector<MaterialGpuResources> materialGpuResources_;
};
