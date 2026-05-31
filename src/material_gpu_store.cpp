#include "material_gpu_store.hpp"

#include <array>
#include <stdexcept>
#include <utility>

MaterialGpuStore::MaterialGpuStore(Device const &device) : device_(device) {
  std::array bindings = {
      vk::DescriptorSetLayoutBinding{
          .binding = 0,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
      vk::DescriptorSetLayoutBinding{
          .binding = 1,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
      vk::DescriptorSetLayoutBinding{
          .binding = 2,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
      vk::DescriptorSetLayoutBinding{
          .binding = 3,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
  };

  vk::DescriptorSetLayoutCreateInfo createInfo{
      .bindingCount = static_cast<std::uint32_t>(bindings.size()),
      .pBindings = bindings.data(),
  };

  materialDescriptorSetLayout_ =
      vk::raii::DescriptorSetLayout(device_.logicalDevice(), createInfo);

  TextureLoader textureLoader(device_);
  flatNormalTexture_ = textureLoader.createSolidColor({128, 128, 255, 255});
  flatHeightTexture_ = textureLoader.createSolidColor({0, 0, 0, 255});
  flatAlphaTexture_ = textureLoader.createSolidColor({255, 255, 255, 255});
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
    if (!material.normalPath.empty()) {
      resources.normalTexture = textureLoader.createFromFile(material.normalPath);
    }
    if (!material.heightPath.empty()) {
      resources.heightTexture = textureLoader.createFromFile(material.heightPath);
    }
    if (!material.alphaPath.empty()) {
      resources.alphaTexture = textureLoader.createFromFile(material.alphaPath);
    }
    resources.tint = material.tint;
    resources.surfaceParams = {
        material.normalScale,
        material.parallaxScale,
        material.normalPath.empty() ? 0.0f : 1.0f,
        material.heightPath.empty() ? 0.0f : 1.0f,
    };
    resources.alphaMode = material.alphaMode;
    resources.alphaParams = {
        static_cast<float>(material.alphaMode),
        material.alphaCutoff,
        0.0f,
        0.0f,
    };
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

void MaterialGpuStore::setMaterialSurfaceParams(MaterialId materialId,
                                                float normalScale,
                                                float parallaxScale) {
  if (materialId >= materialGpuResources_.size()) {
    throw std::runtime_error("Material GPU store material id is out of range.");
  }

  glm::vec4 &params = materialGpuResources_[materialId].surfaceParams;
  params.x = normalScale;
  params.y = parallaxScale;
}

void MaterialGpuStore::setMaterialAlphaParams(MaterialId materialId,
                                              AlphaMode alphaMode,
                                              float alphaCutoff) {
  if (materialId >= materialGpuResources_.size()) {
    throw std::runtime_error("Material GPU store material id is out of range.");
  }

  auto &material = materialGpuResources_[materialId];
  material.alphaMode = alphaMode;
  material.alphaParams.x = static_cast<float>(alphaMode);
  material.alphaParams.y = alphaCutoff;
}

void MaterialGpuStore::setSurfaceDebugEnabled(bool normalMapsEnabled,
                                              bool parallaxEnabled) {
  normalMapsEnabled_ = normalMapsEnabled;
  parallaxEnabled_ = parallaxEnabled;
}

vk::raii::DescriptorPool MaterialGpuStore::createMaterialDescriptorPool(
    std::uint32_t materialCount) const {
  vk::DescriptorPoolSize poolSize{
      .type = vk::DescriptorType::eCombinedImageSampler,
      .descriptorCount = materialCount * 4,
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

    TextureResources const &normalTexture =
        material.normalTexture.has_value() ? *material.normalTexture
                                           : flatNormalTexture_;
    TextureResources const &heightTexture =
        material.heightTexture.has_value() ? *material.heightTexture
                                           : flatHeightTexture_;
    TextureResources const &alphaTexture =
        material.alphaTexture.has_value() ? *material.alphaTexture
                                          : flatAlphaTexture_;
    std::array imageInfos = {
        vk::DescriptorImageInfo{
            .sampler = *material.albedoTexture.sampler,
            .imageView = *material.albedoTexture.imageView,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = *normalTexture.sampler,
            .imageView = *normalTexture.imageView,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = *heightTexture.sampler,
            .imageView = *heightTexture.imageView,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = *alphaTexture.sampler,
            .imageView = *alphaTexture.imageView,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
    };

    std::array writes = {
        vk::WriteDescriptorSet{
            .dstSet = material.descriptorSet,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &imageInfos[0],
        },
        vk::WriteDescriptorSet{
            .dstSet = material.descriptorSet,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &imageInfos[1],
        },
        vk::WriteDescriptorSet{
            .dstSet = material.descriptorSet,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &imageInfos[2],
        },
        vk::WriteDescriptorSet{
            .dstSet = material.descriptorSet,
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &imageInfos[3],
        },
    };

    device_.logicalDevice().updateDescriptorSets(writes, {});
  }
}
