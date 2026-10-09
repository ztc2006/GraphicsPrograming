#include "material_gpu_store.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

vk::raii::DescriptorSetLayout
MaterialGpuStore::createDescriptorSetLayout(Device const &device) {
  // The full frame/material layout requires 5 + 11 combined samplers.
  auto const limits = device.physicalDevice().getProperties().limits;
  if (limits.maxPerStageDescriptorSamplers < 16 ||
      limits.maxPerStageDescriptorSampledImages < 16 ||
      limits.maxDescriptorSetSamplers < 16 ||
      limits.maxDescriptorSetSampledImages < 16 ||
      limits.maxPerStageDescriptorStorageBuffers < 4 ||
      limits.maxDescriptorSetStorageBuffers < 5 ||
      limits.maxPerStageResources < 22)
    throw std::runtime_error(
        "Renderer requires 16 sampled images/samplers and "
        "4 storage buffers per stage, 5 across the pipeline layout and "
        "22 per-stage resources for PBR, lighting and motion.");
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
      vk::DescriptorSetLayoutBinding{
          .binding = 8,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
      vk::DescriptorSetLayoutBinding{
          .binding = 9,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
      vk::DescriptorSetLayoutBinding{
          .binding = 10,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
      vk::DescriptorSetLayoutBinding{
          .binding = 11,
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
                                   TextureCache &textures, UploadBatch &uploads,
                                   ResourceLedger::Scope scope)
    : device_(device), materialDescriptorSetLayout_(layout),
      resourceScope_(std::move(scope)) {
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
                  TextureSamplerDescription const &sampler,
                  TextureMipPolicy policy = TextureMipPolicy::Average,
                  TextureAlphaCoverage coverage = {})
      -> std::optional<TextureResources> {
    if (!bytes.empty())
      return textures.encoded(bytes, label, colorSpace, sampler, uploads,
                              policy, coverage);
    if (!path.empty())
      return textures.file(path, colorSpace, sampler, uploads, policy,
                           coverage);
    return std::nullopt;
  };
  for (Material const &material : materials) {
    MaterialGpuResources resources{};
    float effectiveCutoff =
        material.tint.a > 0 ? material.alphaCutoff / material.tint.a : 0;
    bool canBake = material.alphaMode == AlphaMode::Mask &&
                   material.tint.a > 0 && std::isfinite(effectiveCutoff);
    bool separateAlpha = !material.alphaPath.empty();
    auto albedo =
        load(material.albedoPath, material.albedoBytes, "material albedo",
             TextureColorSpace::Srgb, material.albedoSampler,
             canBake && !separateAlpha ? TextureMipPolicy::AlphaCoverage
                                       : TextureMipPolicy::Average,
             {.cutoff = effectiveCutoff});
    resources.albedoTexture = albedo ? *albedo : whiteColorTexture_;
    canBake &= !separateAlpha || resources.albedoTexture.opaqueAlpha();
    resources.hasCoverageMips = canBake;
    resources.coverageCutoff = material.alphaCutoff;
    resources.coverageFactor = material.tint.a;
    resources.normalTexture =
        load(material.normalPath, material.normalBytes, "material normal",
             TextureColorSpace::Linear, material.normalSampler,
             TextureMipPolicy::Normal);
    resources.heightTexture =
        load(material.heightPath, {}, "material height",
             TextureColorSpace::Linear, material.heightSampler);
    resources.alphaTexture = load(
        material.alphaPath, {}, "material alpha", TextureColorSpace::Linear,
        material.alphaSampler,
        canBake ? TextureMipPolicy::AlphaCoverage : TextureMipPolicy::Average,
        {.cutoff = effectiveCutoff, .channel = TextureAlphaChannel::Red});
    resources.baseAlbedoTexture = textures.baseLevelBinding(
        resources.albedoTexture, material.albedoSampler);
    resources.baseAlphaTexture = textures.baseLevelBinding(
        resources.alphaTexture ? *resources.alphaTexture : whiteDataTexture_,
        material.alphaSampler);
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

    resources.specularTexture =
        load(material.specularPath, material.specularBytes,
             "material specular strength", TextureColorSpace::Linear,
             material.specularSampler);
    resources.specularColorTexture =
        load(material.specularColorPath, material.specularColorBytes,
             "material specular color", TextureColorSpace::Srgb,
             material.specularColorSampler);

    resources.uniform = device_.createBuffer(
        sizeof(MaterialUniformBufferObject),
        vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible, resourceScope_);
    MaterialUniformBufferObject materialUbo{
        .pbrParams = {material.metallicFactor, material.roughnessFactor,
                      material.occlusionStrength, 0.0f},
        .emissiveFactor = glm::vec4(material.emissiveFactor, 0.0f),
        .specularColorAndWeight =
            glm::vec4(material.specularColorFactor, material.specularFactor),
    };
    materialUbo.optical={material.optical.ior,material.optical.transmission,float(material.optical.solid),
      float(material.optical.enabled ? 1u|(material.optical.coverage<<1) : 0u)};
    materialUbo.absorptionThickness=glm::vec4(material.optical.absorption,material.optical.thickness);
    resources.uniform.write(std::as_bytes(std::span{&materialUbo, 1}));
    validateOpticalMaterial(material.optical);
    resources.optical = material.optical;
    resources.tint = material.tint;
    resources.surfaceParams = {
        material.normalScale,
        material.parallaxScale,
        material.normalPath.empty() && material.normalBytes.empty() ? 0.0f
                                                                    : 1.0f,
        material.heightPath.empty() ? 0.0f : 1.0f,
    };
    resources.alphaMode = material.optical.enabled ? AlphaMode::Blend : material.alphaMode;
    resources.doubleSided = material.optical.enabled ? true : material.doubleSided;
    resources.sourceAreaLight = material.sourceAreaLight;
    resources.alphaParams = {
        static_cast<float>(resources.alphaMode),
        material.alphaCutoff,
        0.0f,
        0.0f,
    };
    updateCoverageState(resources);
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
  updateCoverageState(materialGpuResources_[materialId]);
}

void MaterialGpuStore::updateCoverageState(MaterialGpuResources &material) {
  // 0: ordinary mips; 1: matching baked coverage; 2: edited baked material.
  // State 2 must use source alpha even after changing MASK to BLEND.
  material.alphaParams.z =
      !material.hasCoverageMips ? 0.0f
      : material.alphaMode == AlphaMode::Mask &&
              material.alphaParams.y == material.coverageCutoff &&
              material.tint.a == material.coverageFactor
          ? 1.0f
          : 2.0f;
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
  updateCoverageState(material);
}

vk::raii::DescriptorPool MaterialGpuStore::createMaterialDescriptorPool(
    std::uint32_t materialCount) const {
  std::array poolSizes = {
      vk::DescriptorPoolSize{
          .type = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = materialCount * 11,
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
    TextureResources const &specularTexture = material.specularTexture
                                                  ? *material.specularTexture
                                                  : whiteDataTexture_;
    TextureResources const &specularColorTexture =
        material.specularColorTexture ? *material.specularColorTexture
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
        vk::DescriptorImageInfo{
            .sampler = material.baseAlbedoTexture.sampler(),
            .imageView = material.baseAlbedoTexture.imageView(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = material.baseAlphaTexture.sampler(),
            .imageView = material.baseAlphaTexture.imageView(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = specularTexture.sampler(),
            .imageView = specularTexture.imageView(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = specularColorTexture.sampler(),
            .imageView = specularColorTexture.imageView(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
    };
    vk::DescriptorBufferInfo bufferInfo{
        .buffer = *material.uniform.buffer,
        .offset = 0,
        .range = sizeof(MaterialUniformBufferObject),
    };

    std::array<vk::WriteDescriptorSet, 12> writes{};
    constexpr std::array<std::uint32_t, 11> textureBindings{0, 1, 2, 3,  4, 5,
                                                            6, 8, 9, 10, 11};
    for (std::size_t i = 0; i < imageInfos.size(); ++i)
      writes[i] = vk::WriteDescriptorSet{
          .dstSet = material.descriptorSet,
          .dstBinding = textureBindings[i],
          .descriptorCount = 1,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .pImageInfo = &imageInfos[i]};
    writes.back() = vk::WriteDescriptorSet{
        .dstSet = material.descriptorSet,
        .dstBinding = 7,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eUniformBuffer,
        .pBufferInfo = &bufferInfo};

    device_.logicalDevice().updateDescriptorSets(writes, {});
  }
}
