#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "device.hpp"
#include "scene.hpp"
#include "texture.hpp"

class MaterialGpuStore {
public:
  struct MaterialGpuResources {
    TextureResources albedoTexture;
    std::optional<TextureResources> normalTexture;
    std::optional<TextureResources> heightTexture;
    std::optional<TextureResources> alphaTexture;
    vk::DescriptorSet descriptorSet = nullptr;
    glm::vec4 tint{1.0f};
    glm::vec4 surfaceParams{1.0f, 0.04f, 0.0f, 0.0f};
    glm::vec4 alphaParams{0.0f, 0.5f, 0.0f, 0.0f};
    AlphaMode alphaMode = AlphaMode::Opaque;
  };

  explicit MaterialGpuStore(Device const &device);

  void setMaterials(std::vector<Material> const &materials);
  void setMaterialTint(MaterialId materialId, glm::vec4 const &tint);
  void setMaterialSurfaceParams(MaterialId materialId, float normalScale,
                                float parallaxScale);
  void setMaterialAlphaParams(MaterialId materialId, AlphaMode alphaMode,
                              float alphaCutoff);
  void setSurfaceDebugEnabled(bool normalMapsEnabled, bool parallaxEnabled);

  vk::DescriptorSetLayout descriptorSetLayout() const {
    return *materialDescriptorSetLayout_;
  }

  bool empty() const { return materialGpuResources_.empty(); }
  bool normalMapsEnabled() const { return normalMapsEnabled_; }
  bool parallaxEnabled() const { return parallaxEnabled_; }

  MaterialGpuResources const &material(MaterialId materialId) const;

private:
  std::vector<MaterialGpuResources>
  createMaterialResources(std::vector<Material> const &materials) const;

  vk::raii::DescriptorPool
  createMaterialDescriptorPool(std::uint32_t materialCount) const;
  void writeMaterialDescriptorSets();

private:
  Device const &device_;
  vk::raii::DescriptorSetLayout materialDescriptorSetLayout_ = nullptr;
  vk::raii::DescriptorPool materialDescriptorPool_ = nullptr;
  TextureResources flatNormalTexture_;
  TextureResources flatHeightTexture_;
  TextureResources flatAlphaTexture_;
  bool normalMapsEnabled_ = true;
  bool parallaxEnabled_ = true;
  std::vector<MaterialGpuResources> materialGpuResources_;
};
