#include "material_gpu_store.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

vk::raii::DescriptorSetLayout
MaterialGpuStore::createDescriptorSetLayout(Device const &device) {
  std::array bindings = {
      vk::DescriptorSetLayoutBinding{
          .binding = 0,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
      vk::DescriptorSetLayoutBinding{
          .binding = 4,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
      vk::DescriptorSetLayoutBinding{
          .binding = 5,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
      vk::DescriptorSetLayoutBinding{
          .binding = 6,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
      vk::DescriptorSetLayoutBinding{
          .binding = 7,
          .descriptorType = vk::DescriptorType::eUniformBuffer,
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

  return vk::raii::DescriptorSetLayout(device.logicalDevice(), createInfo);
}

MaterialGpuStore::MaterialGpuStore(Device const &device,
                                   vk::DescriptorSetLayout layout,
                                   std::vector<Material> const &materials,
                                   TextureCache &textures, UploadBatch &uploads, ResourceLedger::Scope scope)
    : device_(device), materialDescriptorSetLayout_(layout), resourceScope_(std::move(scope)) {
  flatNormalTexture_ =
      textures.solid({128, 128, 255, 255}, TextureColorSpace::Linear, uploads);
  flatHeightTexture_ =
      textures.solid({0, 0, 0, 255}, TextureColorSpace::Linear, uploads);
  whiteDataTexture_ =
      textures.solid({255, 255, 255, 255}, TextureColorSpace::Linear, uploads);
  whiteColorTexture_ =
      textures.solid({255, 255, 255, 255}, TextureColorSpace::Srgb, uploads);
  materialGpuResources_ = createMaterialResources(materials, textures, uploads);
  materialDescriptorPool_ = createMaterialDescriptorPool(
      static_cast<std::uint32_t>(materialGpuResources_.size()));
  writeMaterialDescriptorSets();
}

std::vector<MaterialGpuStore::MaterialGpuResources>
MaterialGpuStore::createMaterialResources(
    std::vector<Material> const &materials, TextureCache &textures,
    UploadBatch &uploads) const {
  if (materials.empty()) {
    throw std::runtime_error(
        "Material GPU store requires at least one material.");
  }

  std::vector<MaterialGpuResources> newMaterials;
  newMaterials.reserve(materials.size());

  auto load = [&](std::string const &path, std::vector<std::byte> const &bytes,
                  char const *label, TextureColorSpace colorSpace,
                  TextureSamplerDescription const &sampler)
      -> std::optional<TextureResources> {
    if (!bytes.empty())
      return textures.encoded(bytes, label, colorSpace, sampler, uploads);
    if (!path.empty())
      return textures.file(path, colorSpace, sampler, uploads);
    return std::nullopt;
  };
  for (Material const &material : materials) {
    MaterialGpuResources resources{};
    auto albedo =
        load(material.albedoPath, material.albedoBytes, "material albedo",
             TextureColorSpace::Srgb, material.albedoSampler);
    resources.albedoTexture = albedo ? *albedo : whiteColorTexture_;
    resources.normalTexture =
        load(material.normalPath, material.normalBytes, "material normal",
             TextureColorSpace::Linear, material.normalSampler);
    resources.heightTexture =
        load(material.heightPath, {}, "material height",
             TextureColorSpace::Linear, material.heightSampler);
    resources.alphaTexture =
        load(material.alphaPath, {}, "material alpha",
             TextureColorSpace::Linear, material.alphaSampler);
    resources.metallicRoughnessTexture =
        load(material.metallicRoughnessPath, material.metallicRoughnessBytes,
             "material metallic-roughness", TextureColorSpace::Linear,
             material.metallicRoughnessSampler);
    resources.occlusionTexture = load(
        material.occlusionPath, material.occlusionBytes, "material occlusion",
        TextureColorSpace::Linear, material.occlusionSampler);
    resources.emissiveTexture =
        load(material.emissivePath, material.emissiveBytes, "material emissive",
             TextureColorSpace::Srgb, material.emissiveSampler);

    resources.uniform = device_.createBuffer(
        sizeof(MaterialUniformBufferObject), vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible,
        resourceScope_);
    MaterialUniformBufferObject materialUbo{
        .pbrParams = {material.metallicFactor, material.roughnessFactor,
                      material.occlusionStrength, 0.0f},
        .emissiveFactor = glm::vec4(material.emissiveFactor, 0.0f),
    };
    resources.uniform.write(std::as_bytes(std::span{&materialUbo, 1}));
    resources.tint = material.tint;
    resources.surfaceParams = {
        material.normalScale,
        material.parallaxScale,
        material.normalPath.empty() && material.normalBytes.empty() ? 0.0f
                                                                    : 1.0f,
        material.heightPath.empty() ? 0.0f : 1.0f,
    };
    resources.alphaMode = material.alphaMode;
    resources.doubleSided = material.doubleSided;
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

vk::raii::DescriptorPool MaterialGpuStore::createMaterialDescriptorPool(
    std::uint32_t materialCount) const {
  std::array poolSizes = {
      vk::DescriptorPoolSize{
          .type = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = materialCount * 7,
      },
      vk::DescriptorPoolSize{
          .type = vk::DescriptorType::eUniformBuffer,
          .descriptorCount = materialCount,
      },
  };

  vk::DescriptorPoolCreateInfo createInfo{
      .maxSets = materialCount,
      .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
      .pPoolSizes = poolSizes.data(),
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
                                               materialDescriptorSetLayout_);

  vk::DescriptorSetAllocateInfo allocateInfo{
      .descriptorPool = *materialDescriptorPool_,
      .descriptorSetCount = static_cast<std::uint32_t>(layouts.size()),
      .pSetLayouts = layouts.data(),
  };

  auto descriptorSets =
      (*device_.logicalDevice()).allocateDescriptorSets(allocateInfo);
  descriptorAccounting_ = resourceScope_.track(
      {.descriptorPools = 1, .descriptorSets = descriptorSets.size()});

  for (std::size_t index = 0; index < materialGpuResources_.size(); ++index) {
    auto &material = materialGpuResources_[index];
    material.descriptorSet = descriptorSets[index];

    TextureResources const &normalTexture = material.normalTexture.has_value()
                                                ? *material.normalTexture
                                                : flatNormalTexture_;
    TextureResources const &heightTexture = material.heightTexture.has_value()
                                                ? *material.heightTexture
                                                : flatHeightTexture_;
    TextureResources const &alphaTexture = material.alphaTexture.has_value()
                                               ? *material.alphaTexture
                                               : whiteDataTexture_;
    TextureResources const &metallicRoughnessTexture =
        material.metallicRoughnessTexture.has_value()
            ? *material.metallicRoughnessTexture
            : whiteDataTexture_;
    TextureResources const &occlusionTexture =
        material.occlusionTexture.has_value() ? *material.occlusionTexture
                                              : whiteDataTexture_;
    TextureResources const &emissiveTexture =
        material.emissiveTexture.has_value() ? *material.emissiveTexture
                                             : whiteColorTexture_;
    std::array imageInfos = {
        vk::DescriptorImageInfo{
            .sampler = material.albedoTexture.sampler(),
            .imageView = material.albedoTexture.imageView(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = normalTexture.sampler(),
            .imageView = normalTexture.imageView(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = heightTexture.sampler(),
            .imageView = heightTexture.imageView(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = alphaTexture.sampler(),
            .imageView = alphaTexture.imageView(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = metallicRoughnessTexture.sampler(),
            .imageView = metallicRoughnessTexture.imageView(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = occlusionTexture.sampler(),
            .imageView = occlusionTexture.imageView(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = emissiveTexture.sampler(),
            .imageView = emissiveTexture.imageView(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
    };
    vk::DescriptorBufferInfo bufferInfo{
        .buffer = *material.uniform.buffer,
        .offset = 0,
        .range = sizeof(MaterialUniformBufferObject),
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
        vk::WriteDescriptorSet{
            .dstSet = material.descriptorSet,
            .dstBinding = 4,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &imageInfos[4],
        },
        vk::WriteDescriptorSet{
            .dstSet = material.descriptorSet,
            .dstBinding = 5,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &imageInfos[5],
        },
        vk::WriteDescriptorSet{
            .dstSet = material.descriptorSet,
            .dstBinding = 6,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &imageInfos[6],
        },
        vk::WriteDescriptorSet{
            .dstSet = material.descriptorSet,
            .dstBinding = 7,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &bufferInfo,
        },
    };

    device_.logicalDevice().updateDescriptorSets(writes, {});
  }
}
