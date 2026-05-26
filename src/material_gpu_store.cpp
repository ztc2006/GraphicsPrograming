#include "material_gpu_store.hpp"

#include <stdexcept>
#include <utility>

MaterialGpuStore::MaterialGpuStore(Device const &device) : device_(device) {
  vk::DescriptorSetLayoutBinding binding{
      .binding = 0,
      .descriptorType = vk::DescriptorType::eCombinedImageSampler,
      .descriptorCount = 1,
      .stageFlags = vk::ShaderStageFlagBits::eFragment,
  };

  vk::DescriptorSetLayoutCreateInfo createInfo{
      .bindingCount = 1,
      .pBindings = &binding,
  };

  materialDescriptorSetLayout_ =
      vk::raii::DescriptorSetLayout(device_.logicalDevice(), createInfo);
}

void MaterialGpuStore::setMaterials(std::vector<Material> const &materials) {
  std::vector<MaterialGpuResources> newMaterials =
      createMaterialResources(materials);

  vk::raii::DescriptorPool newPool = createMaterialDescriptorPool(
      static_cast<std::uint32_t>(newMaterials.size()));

  materialGpuResources_.swap(newMaterials);
  materialDescriptorPool_ = std::move(newPool);
  writeMaterialDescriptorSets();
}

std::vector<MaterialGpuStore::MaterialGpuResources>
MaterialGpuStore::createMaterialResources(
    std::vector<Material> const &materials) const {
  if (materials.empty()) {
    throw std::runtime_error(
        "Material GPU store requires at least one material.");
  }

  std::vector<MaterialGpuResources> newMaterials;
  newMaterials.reserve(materials.size());

  TextureLoader textureLoader(device_);

  for (Material const &material : materials) {
    if (material.albedoPath.empty()) {
      throw std::runtime_error("Material albedo path is empty.");
    }

    MaterialGpuResources resources{};
    resources.albedoTexture = textureLoader.createFromFile(material.albedoPath);
    resources.tint = material.tint;
    newMaterials.push_back(std::move(resources));
  }

  return newMaterials;
}

void MaterialGpuStore::setMaterialTint(MaterialId materialId,
                                       glm::vec4 const &tint) {
  if (materialId >= materialGpuResources_.size()) {
    throw std::runtime_error("Material GPU store material id is out of range.");
  }

  materialGpuResources_[materialId].tint = tint;
}

vk::raii::DescriptorPool MaterialGpuStore::createMaterialDescriptorPool(
    std::uint32_t materialCount) const {
  vk::DescriptorPoolSize poolSize{
      .type = vk::DescriptorType::eCombinedImageSampler,
      .descriptorCount = materialCount,
  };

  vk::DescriptorPoolCreateInfo createInfo{
      .maxSets = materialCount,
      .poolSizeCount = 1,
      .pPoolSizes = &poolSize,
  };

  return vk::raii::DescriptorPool(device_.logicalDevice(), createInfo);
}

MaterialGpuStore::MaterialGpuResources const &
MaterialGpuStore::material(MaterialId materialId) const {
  if (materialId >= materialGpuResources_.size()) {
    throw std::runtime_error("Material GPU store material id is out of range.");
  }
  return materialGpuResources_[materialId];
}

void MaterialGpuStore::writeMaterialDescriptorSets() {
  std::vector<vk::DescriptorSetLayout> layouts(materialGpuResources_.size(),
                                               *materialDescriptorSetLayout_);

  vk::DescriptorSetAllocateInfo allocateInfo{
      .descriptorPool = *materialDescriptorPool_,
      .descriptorSetCount = static_cast<std::uint32_t>(layouts.size()),
      .pSetLayouts = layouts.data(),
  };

  auto descriptorSets =
      (*device_.logicalDevice()).allocateDescriptorSets(allocateInfo);

  for (std::size_t index = 0; index < materialGpuResources_.size(); ++index) {
    auto &material = materialGpuResources_[index];
    material.descriptorSet = descriptorSets[index];

    vk::DescriptorImageInfo imageInfo{
        .sampler = *material.albedoTexture.sampler,
        .imageView = *material.albedoTexture.imageView,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };

    vk::WriteDescriptorSet write{
        .dstSet = material.descriptorSet,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &imageInfo,
    };

    device_.logicalDevice().updateDescriptorSets({write}, {});
  }
}
