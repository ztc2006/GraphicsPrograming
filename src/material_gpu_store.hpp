#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "device.hpp"
#include "scene.hpp"
#include "texture.hpp"

class MaterialGpuStore {
public:
  struct MaterialGpuResources {
    TextureResources albedoTexture;
    vk::DescriptorSet descriptorSet = nullptr;
    glm::vec4 tint{1.0f};
  };

  explicit MaterialGpuStore(Device const &device);

  void setMaterials(std::vector<Material> const &materials);
  void setMaterialTint(MaterialId materialId, glm::vec4 const &tint);

  vk::DescriptorSetLayout descriptorSetLayout() const {
    return *materialDescriptorSetLayout_;
  }

  bool empty() const { return materialGpuResources_.empty(); }

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
  std::vector<MaterialGpuResources> materialGpuResources_;
};
