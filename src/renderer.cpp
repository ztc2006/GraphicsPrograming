#include "pch.hpp"

#include "renderer.hpp"

#include "hdr_image.hpp"
#include "material_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
struct PushConstants {
  glm::mat4 transform{1.0f};
  glm::vec4 materialTint{1.0f};
  glm::vec4 surfaceParams{1.0f, 0.04f, 0.0f, 0.0f};
  glm::vec4 alphaParams{0.0f, 0.5f, 0.0f, 0.0f};
};

struct FrameUniformBufferObject {
  glm::mat4 viewProj{1.0f};
  glm::vec4 cameraPosition{0.0f};
  glm::vec4 lightDirection{0.0f, 1.0f, 0.3f, 0.0f};
  glm::vec4 lightColor{1.0f, 0.98f, 0.92f, 1.0f};
  glm::vec4 ambientColor{0.08f, 0.08f, 0.1f, 1.0f};
  glm::vec4 lightingParams{1.0f, 0.35f, 32.0f, 0.0f};
  glm::mat4 lightViewProj{1.0f};
  glm::vec4 shadowParams{0.0025f, 0.0007f, 1.0f, 1.0f};
  glm::mat4 inverseViewProj{1.0f};
  glm::vec4 environmentParams{1.0f, 0.0f, 0.0f, 0.0f};
  std::array<glm::vec4, 9> environmentSh{};
};

glm::mat4 computeLightViewProj(glm::vec3 direction,
                               LightingSettings const &lighting) {
  if (glm::length(direction) <= 0.0001f) {
    direction = {0.0f, 1.0f, 0.0f};
  }

  glm::vec3 const lightDir = glm::normalize(direction);
  glm::vec3 const target = lighting.shadowTarget;
  float const lightDistance = std::max(lighting.shadowLightDistance, 0.1f);
  glm::vec3 const eye = target + lightDir * lightDistance;
  glm::vec3 up{0.0f, 1.0f, 0.0f};
  if (std::abs(glm::dot(lightDir, up)) > 0.95f) {
    up = {0.0f, 0.0f, 1.0f};
  }

  glm::mat4 view = glm::lookAt(eye, target, up);
  float const extent = std::max(lighting.shadowOrthoExtent, 0.1f);
  float const nearPlane = std::max(lighting.shadowNearPlane, 0.001f);
  float const farPlane = std::max(lighting.shadowFarPlane, nearPlane + 0.001f);
  glm::mat4 proj =
      glm::ortho(-extent, extent, -extent, extent, nearPlane, farPlane);
  proj[1][1] *= -1.0f;
  return proj * view;
}

Mesh makeUnitAabbLineMesh() {
  std::array<glm::vec3, 8> const positions = {
      glm::vec3{-0.5f, -0.5f, -0.5f}, glm::vec3{0.5f, -0.5f, -0.5f},
      glm::vec3{0.5f, 0.5f, -0.5f},   glm::vec3{-0.5f, 0.5f, -0.5f},
      glm::vec3{-0.5f, -0.5f, 0.5f},  glm::vec3{0.5f, -0.5f, 0.5f},
      glm::vec3{0.5f, 0.5f, 0.5f},    glm::vec3{-0.5f, 0.5f, 0.5f},
  };

  Mesh mesh{};
  mesh.vertices.reserve(positions.size());
  for (glm::vec3 const &position : positions) {
    mesh.vertices.push_back(Vertex{
        .position = position,
        .color = {1.0f, 1.0f, 1.0f},
        .normal = {0.0f, 0.0f, 1.0f},
        .uv = {0.0f, 0.0f},
    });
  }

  mesh.indices = {
      0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7,
  };
  return mesh;
}
} // namespace

Renderer::Renderer(Device const &device)
    : device_(device),
      materialDescriptorSetLayout_(
          MaterialGpuStore::createDescriptorSetLayout(device)),
      textureCache_(device) {
  createPersistentResources();
}

void Renderer::recreateForSwapChain(SwapChain const &swapChain) {
  if (activeFrame_.has_value()) {
    throw std::runtime_error("Cannot recreate renderer swapchain resources "
                             "while a frame is in progress.");
  }

  validateSwapChainCandidate(swapChain);

  vk::PushConstantRange pushConstantRange{
      .stageFlags =
          vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
      .offset = 0,
      .size = sizeof(PushConstants),
  };

  std::array layouts = {
      *frameDescriptorSetLayout_,
      *materialDescriptorSetLayout_,
  };

  vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo{
      .setLayoutCount = static_cast<std::uint32_t>(layouts.size()),
      .pSetLayouts = layouts.data(),
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pushConstantRange,
  };
  vk::raii::PipelineLayout newPipelineLayout(device_.logicalDevice(),
                                             pipelineLayoutCreateInfo);

  vk::raii::Pipeline newGraphicsPipeline = createGraphicsPipeline(
      swapChain, newPipelineLayout, rasterizerDebugSettings_.cullMode);
  vk::raii::Pipeline newDoubleSidedGraphicsPipeline = createGraphicsPipeline(
      swapChain, newPipelineLayout, vk::CullModeFlagBits::eNone);
  vk::raii::Pipeline newTransparentPipeline = createTransparentPipeline(
      swapChain, newPipelineLayout, rasterizerDebugSettings_.cullMode);
  vk::raii::Pipeline newDoubleSidedTransparentPipeline =
      createTransparentPipeline(swapChain, newPipelineLayout,
                                vk::CullModeFlagBits::eNone);
  vk::raii::Pipeline newDebugLinePipeline =
      createDebugLinePipeline(swapChain, newPipelineLayout);
  vk::raii::Pipeline newEnvironmentPipeline =
      createEnvironmentPipeline(swapChain, newPipelineLayout);
  vk::raii::Pipeline newShadowPipeline = createShadowPipeline(
      newPipelineLayout, rasterizerDebugSettings_.cullMode);
  vk::raii::Pipeline newDoubleSidedShadowPipeline =
      createShadowPipeline(newPipelineLayout, vk::CullModeFlagBits::eNone);

  std::vector<vk::raii::Semaphore> newRenderFinishedSemaphores;
  newRenderFinishedSemaphores.reserve(swapChain.images().size());
  for (std::size_t index = 0; index < swapChain.images().size(); ++index) {
    newRenderFinishedSemaphores.emplace_back(device_.logicalDevice(),
                                             vk::SemaphoreCreateInfo{});
  }

  std::vector<vk::ImageLayout> newSwapChainImageLayouts(
      swapChain.images().size(), vk::ImageLayout::eUndefined);
  std::vector<vk::Fence> newImagesInFlight(swapChain.images().size(),
                                           vk::Fence{});
  SwapChain const *newSwapChain = &swapChain;
  DepthResources newDepthResource = createDepthResources(swapChain);
  auto newHdrOutput = std::make_unique<HdrOutput>(device_, swapChain.extent(), swapChain.imageFormat());

  using std::swap;
  swap(pipelineLayout_, newPipelineLayout);
  swap(graphicsPipeline_, newGraphicsPipeline);
  swap(doubleSidedGraphicsPipeline_, newDoubleSidedGraphicsPipeline);
  swap(transparentPipeline_, newTransparentPipeline);
  swap(doubleSidedTransparentPipeline_, newDoubleSidedTransparentPipeline);
  swap(debugLinePipeline_, newDebugLinePipeline);
  swap(environmentPipeline_, newEnvironmentPipeline);
  swap(shadowPipeline_, newShadowPipeline);
  swap(doubleSidedShadowPipeline_, newDoubleSidedShadowPipeline);
  swap(renderFinishedSemaphores_, newRenderFinishedSemaphores);
  swap(swapChainImageLayouts_, newSwapChainImageLayouts);
  swap(imagesInFlight_, newImagesInFlight);
  swap(swapChain_, newSwapChain);
  swap(depthResources_, newDepthResource);
  swap(hdrOutput_, newHdrOutput);
  currentFrame_ = 0;
  activeFrame_.reset();
  activePass_ = ActivePass::eNone;
  resourceStatistics_.pipelineBuilds += 9;
}

void Renderer::createPersistentResources() {
  createCommandPool();
  createFrameResources();
  UploadBatch debugUpload(device_);
  debugAabbLineResources_ = createGeometryResources(makeUnitAabbLineMesh(), debugUpload);
  debugUpload.finish();
  shadowResources_ = createShadowResources();
  HdrImage const environment =
      loadHdrImage("assets/environments/environment.hdr");
  environmentSh_ = projectEquirectangularToSh(environment);
  environmentTexture_ = TextureLoader(device_, device_.resourceLedger().scope(
      ResourceLedger::Domain::Persistent)).createFromHdrPixels(
      environment.rgba, environment.width, environment.height);
  ++resourceStatistics_.environmentUploads;
  createFrameDescriptorSetLayout();
  createFrameDescriptorPool();
  allocateAndWriteFrameDescriptorSets();
  createCommandBuffers();
  device_.nameObject(*shadowResources_.storage.image, "Directional shadow depth");
  device_.nameObject(environmentTexture_.image(), "HDR environment");
  for (auto const &command : commandBuffers_)
    device_.nameObject(*command, "Frame command buffer");
}

void Renderer::createFrameResources() {
  auto properties = device_.physicalDevice().getProperties();
  timestampPeriod_ = properties.limits.timestampPeriod;
  timestampBits_ =
      device_.physicalDevice()
          .getQueueFamilyProperties()[device_.graphicsQueueFamilyIndex()]
          .timestampValidBits;
  frames_.clear();
  frames_.reserve(kFramesInFlight);

  for (std::uint32_t index = 0; index < kFramesInFlight; ++index) {
    FrameContext frame{};
    if (timestampBits_) {
      frame.timestamps = vk::raii::QueryPool(
          device_.logicalDevice(),
          vk::QueryPoolCreateInfo{.queryType = vk::QueryType::eTimestamp,
                                  .queryCount = 10});
      device_.nameObject(*frame.timestamps, "Frame GPU timestamps");
    }
    frame.imageAvailableSemaphore =
        vk::raii::Semaphore(device_.logicalDevice(), vk::SemaphoreCreateInfo{});
    frame.inFlightFence = vk::raii::Fence(
        device_.logicalDevice(),
        vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});

    frame.uniform = device_.createBuffer(
        sizeof(FrameUniformBufferObject), vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible);
    frames_.push_back(std::move(frame));
  }
}

Renderer::MeshGpuResources Renderer::createGeometryResources(
    Mesh const &mesh, UploadBatch &uploads, ResourceLedger::Scope scope) {
  auto upload = [this, &uploads, &scope]<typename T>(std::vector<T> const &data,
                                                   vk::BufferUsageFlags usage) {
    auto bytes = std::as_bytes(std::span(data));
    auto resources = device_.createBuffer(bytes.size_bytes(),
        vk::BufferUsageFlagBits::eTransferDst | usage,
        vk::MemoryPropertyFlagBits::eDeviceLocal, scope);
    uploads.copyBuffer(bytes, *resources.buffer, usage);
    return resources;
  };
  MeshGpuResources resources;
  resources.vertex = upload(mesh.vertices, vk::BufferUsageFlagBits::eVertexBuffer);
  resources.index = upload(mesh.indices, vk::BufferUsageFlagBits::eIndexBuffer);
  resources.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
  return resources;
}

Renderer::DepthResources
Renderer::createDepthResources(SwapChain const &swapChain) const {
  vk::ImageCreateInfo imageCreateInfo{
      .imageType = vk::ImageType::e2D,
      .format = kDepthFormat,
      .extent =
          {
              .width = swapChain.extent().width,
              .height = swapChain.extent().height,
              .depth = 1,
          },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = vk::SampleCountFlagBits::e1,
      .tiling = vk::ImageTiling::eOptimal,
      .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
      .sharingMode = vk::SharingMode::eExclusive,
      .initialLayout = vk::ImageLayout::eUndefined,
  };

  DepthResources resources;
  resources.storage = device_.createImage(imageCreateInfo,
      std::uint64_t(swapChain.extent().width) * swapChain.extent().height * 4, vk::MemoryPropertyFlagBits::eDeviceLocal);

  vk::ImageViewCreateInfo imageViewCreateInfo{
      .image = *resources.storage.image,
      .viewType = vk::ImageViewType::e2D,
      .format = kDepthFormat,
      .subresourceRange =
          {
              .aspectMask = vk::ImageAspectFlagBits::eDepth,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };
  resources.imageView =
      vk::raii::ImageView(device_.logicalDevice(), imageViewCreateInfo);
  resources.accounting = device_.resourceLedger().scope(
      ResourceLedger::Domain::Persistent).track({.imageViews = 1});
  resources.layout = vk::ImageLayout::eUndefined;
  return resources;
}

Renderer::ShadowResources Renderer::createShadowResources() const {
  vk::ImageCreateInfo imageCreateInfo{
      .imageType = vk::ImageType::e2D,
      .format = kDepthFormat,
      .extent =
          {
              .width = kShadowMapSize,
              .height = kShadowMapSize,
              .depth = 1,
          },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = vk::SampleCountFlagBits::e1,
      .tiling = vk::ImageTiling::eOptimal,
      .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
               vk::ImageUsageFlagBits::eSampled,
      .sharingMode = vk::SharingMode::eExclusive,
      .initialLayout = vk::ImageLayout::eUndefined,
  };

  ShadowResources resources;
  resources.storage = device_.createImage(imageCreateInfo,
      std::uint64_t(kShadowMapSize) * kShadowMapSize * 4, vk::MemoryPropertyFlagBits::eDeviceLocal);

  vk::ImageViewCreateInfo imageViewCreateInfo{
      .image = *resources.storage.image,
      .viewType = vk::ImageViewType::e2D,
      .format = kDepthFormat,
      .subresourceRange =
          {
              .aspectMask = vk::ImageAspectFlagBits::eDepth,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };

  vk::SamplerCreateInfo samplerCreateInfo{
      .magFilter = vk::Filter::eLinear,
      .minFilter = vk::Filter::eLinear,
      .mipmapMode = vk::SamplerMipmapMode::eNearest,
      .addressModeU = vk::SamplerAddressMode::eClampToBorder,
      .addressModeV = vk::SamplerAddressMode::eClampToBorder,
      .addressModeW = vk::SamplerAddressMode::eClampToBorder,
      .mipLodBias = 0.0f,
      .anisotropyEnable = false,
      .compareEnable = true,
      .compareOp = vk::CompareOp::eLessOrEqual,
      .minLod = 0.0f,
      .maxLod = 0.0f,
      .borderColor = vk::BorderColor::eFloatOpaqueWhite,
      .unnormalizedCoordinates = false,
  };

  vk::SamplerCreateInfo debugSamplerCreateInfo = samplerCreateInfo;
  debugSamplerCreateInfo.compareEnable = false;

  resources.imageView =
      vk::raii::ImageView(device_.logicalDevice(), imageViewCreateInfo);
  resources.sampler =
      vk::raii::Sampler(device_.logicalDevice(), samplerCreateInfo);
  resources.debugSampler =
      vk::raii::Sampler(device_.logicalDevice(), debugSamplerCreateInfo);
  resources.accounting = device_.resourceLedger().scope(
      ResourceLedger::Domain::Persistent).track({.imageViews = 1, .samplers = 2});
  resources.layout = vk::ImageLayout::eUndefined;
  return resources;
}

Renderer::PreparedScene Renderer::prepareScene(AssetLibrary const &assets) {
  std::lock_guard lock(preparationMutex_);
  if (assets.meshes.empty() || assets.materials.empty())
    throw std::runtime_error("A GPU scene requires meshes and materials.");
  // Reject malformed geometry before issuing any upload or touching live state.
  for (auto const &mesh : assets.meshes) {
    if (mesh.vertices.empty() || mesh.indices.empty())
      throw std::runtime_error("Mesh has no vertices or indices.");
    if (mesh.indices.size() > std::numeric_limits<std::uint32_t>::max())
      throw std::runtime_error("Mesh index count exceeds the draw limit.");
    for (auto index : mesh.indices)
      if (index >= mesh.vertices.size())
        throw std::runtime_error("Mesh index is out of range.");
  }
  textureCache_.pruneExpired();
  PreparedScene candidate(new SceneAssets);
  candidate->uploads_ = std::make_unique<UploadBatch>(device_);
  auto &uploads = *candidate->uploads_;
  candidate->owner_ = this;
  candidate->resourceScope_ = device_.resourceLedger().scope(ResourceLedger::Domain::PreparedScene);
  candidate->meshes_.reserve(assets.meshes.size());
  for (auto const &mesh : assets.meshes)
    candidate->meshes_.push_back(createGeometryResources(mesh, uploads, candidate->resourceScope_));
  candidate->materials_ = std::make_unique<MaterialGpuStore>(
      device_, *materialDescriptorSetLayout_, assets.materials, textureCache_, uploads, candidate->resourceScope_);
  return candidate;
}

void Renderer::validateSceneCandidate(PreparedScene const &candidate) const {
  if (activeFrame_)
    throw std::runtime_error("Cannot submit or commit a scene while recording a frame.");
  if (!candidate || candidate->owner_ != this || candidate == sceneAssets_)
    throw std::runtime_error("Scene candidate is invalid or belongs to a different renderer.");
}

void Renderer::submitSceneUpload(PreparedScene const &candidate) {
  validateSceneCandidate(candidate);
  if (!candidate->uploads_ || candidate->uploads_->submitted())
    return;
  // Retain destinations before submission. Dropping/canceling the caller's
  // handle cannot destroy resources or wait for an upload still in flight.
  pendingSceneUploads_.push_back(candidate);
  try {
    candidate->uploads_->submit();
  } catch (...) {
    pendingSceneUploads_.pop_back();
    throw;
  }
  auto const &statistics = candidate->uploads_->statistics();
  resourceStatistics_.lastSceneUpload = statistics;
  resourceStatistics_.sceneUploadSubmissions += statistics.submissions;
  resourceStatistics_.sceneImageCopies += statistics.imageCopies;
  resourceStatistics_.pendingSceneUploads = pendingSceneUploads_.size();
}

void Renderer::waitSceneUpload(PreparedScene const &candidate) {
  submitSceneUpload(candidate);
  if (candidate->uploads_) {
    auto before = candidate->uploads_->statistics().fenceWaits;
    auto statistics = candidate->uploads_->finish();
    resourceStatistics_.sceneUploadFenceWaits += statistics.fenceWaits - before;
    resourceStatistics_.lastSceneUpload = statistics;
  }
}

bool Renderer::commitScene(PreparedScene &candidate,
                           std::function<void()> retireSceneUi) {
  validateSceneCandidate(candidate);
  if (candidate->uploads_ && !candidate->uploads_->submitted()) {
    submitSceneUpload(candidate);
    return false;
  }
  if (candidate->uploads_ && !candidate->uploads_->ready())
    return false;
  // Allocate the retirement entry before changing live state. The callback owns
  // a snapshot of old preview descriptors, not references to new UI containers.
  if (sceneAssets_ || retireSceneUi)
    retiredScenes_.push_back(RetiredScene{submittedFrameId_, sceneAssets_, std::move(retireSceneUi)});
  if (sceneAssets_)
    sceneAssets_->resourceScope_.setDomain(ResourceLedger::Domain::RetiredScene);
  candidate->resourceScope_.setDomain(ResourceLedger::Domain::LiveScene);
  sceneAssets_.swap(candidate);
  candidate.reset();
  ++resourceStatistics_.sceneCommits;
  resourceStatistics_.retiredScenes = retiredScenes_.size();
  return true;
}

MaterialGpuStore &Renderer::materials() const {
  if (!sceneAssets_)
    throw std::runtime_error("Renderer has no committed scene assets.");
  return *sceneAssets_->materials_;
}

void Renderer::setMaterialTint(MaterialId materialId, glm::vec4 const &tint) {
  materials().setMaterialTint(materialId, tint);
}

void Renderer::setMaterialSurfaceParams(MaterialId materialId,
                                        float normalScale,
                                        float parallaxScale) {
  materials().setMaterialSurfaceParams(materialId, normalScale, parallaxScale);
}

void Renderer::setMaterialAlphaParams(MaterialId materialId,
                                      AlphaMode alphaMode, float alphaCutoff) {
  materials().setMaterialAlphaParams(materialId, alphaMode, alphaCutoff);
}

void Renderer::setSurfaceDebugEnabled(bool normalMapsEnabled,
                                      bool parallaxEnabled) {
  normalMapsEnabled_ = normalMapsEnabled;
  parallaxEnabled_ = parallaxEnabled;
}

TextureResources const &
Renderer::materialAlbedoTexture(MaterialId materialId) const {
  return materials().material(materialId).albedoTexture;
}

TextureResources const *
Renderer::materialAlphaTexture(MaterialId materialId) const {
  auto const &material = materials().material(materialId);
  return material.alphaTexture.has_value() ? &*material.alphaTexture : nullptr;
}

void Renderer::setUiDrawCallback(
    std::function<void(vk::CommandBuffer)> callback) {
  uiDrawCallback_ = std::move(callback);
}

void Renderer::setRasterizerDebugSettings(RasterizerDebugSettings settings) {
  if (activeFrame_.has_value()) {
    throw std::runtime_error(
        "Cannot change rasterizer settings while a frame is in progress.");
  }

  rasterizerDebugSettings_ = settings;
  if (swapChain_ != nullptr) {
    recreateForSwapChain(*swapChain_);
  }
}

void Renderer::createFrameDescriptorSetLayout() {
  std::array bindings = {
      vk::DescriptorSetLayoutBinding{
          .binding = 0,
          .descriptorType = vk::DescriptorType::eUniformBuffer,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eVertex |
                        vk::ShaderStageFlagBits::eFragment,
      },
      vk::DescriptorSetLayoutBinding{
          .binding = 1,
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
          .binding = 2,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment,
      },
  };

  vk::DescriptorSetLayoutCreateInfo createInfo{
      .bindingCount = static_cast<std::uint32_t>(bindings.size()),
      .pBindings = bindings.data(),
  };
  frameDescriptorSetLayout_ =
      vk::raii::DescriptorSetLayout(device_.logicalDevice(), createInfo);
}

void Renderer::createFrameDescriptorPool() {
  std::array poolSizes = {
      vk::DescriptorPoolSize{
          .type = vk::DescriptorType::eUniformBuffer,
          .descriptorCount = kFramesInFlight,
      },
      vk::DescriptorPoolSize{
          .type = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = kFramesInFlight * 3,
      },
  };

  vk::DescriptorPoolCreateInfo createInfo{
      .maxSets = kFramesInFlight,
      .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
      .pPoolSizes = poolSizes.data(),
  };
  frameDescriptorPool_ =
      vk::raii::DescriptorPool(device_.logicalDevice(), createInfo);
}

void Renderer::allocateAndWriteFrameDescriptorSets() {
  std::vector<vk::DescriptorSetLayout> layouts(frames_.size(),
                                               *frameDescriptorSetLayout_);
  vk::DescriptorSetAllocateInfo allocateInfo{
      .descriptorPool = *frameDescriptorPool_,
      .descriptorSetCount = static_cast<std::uint32_t>(layouts.size()),
      .pSetLayouts = layouts.data(),
  };

  auto descriptorSets =
      (*device_.logicalDevice()).allocateDescriptorSets(allocateInfo);
  frameDescriptorAccounting_ = device_.resourceLedger().scope(
      ResourceLedger::Domain::Persistent).track(
          {.descriptorPools = 1, .descriptorSets = descriptorSets.size()});

  for (std::size_t index = 0; index < frames_.size(); ++index) {
    frames_[index].descriptorSet = descriptorSets[index];

    vk::DescriptorBufferInfo bufferInfo{
        .buffer = *frames_[index].uniform.buffer,
        .offset = 0,
        .range = sizeof(FrameUniformBufferObject),
    };

    vk::DescriptorImageInfo shadowImageInfo{
        .sampler = *shadowResources_.sampler,
        .imageView = *shadowResources_.imageView,
        .imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal,
    };

    vk::DescriptorImageInfo shadowDebugImageInfo{
        .sampler = *shadowResources_.debugSampler,
        .imageView = *shadowResources_.imageView,
        .imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal,
    };
    vk::DescriptorImageInfo environmentImageInfo{
        .sampler = environmentTexture_.sampler(),
        .imageView = environmentTexture_.imageView(),
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };

    std::array writes = {
        vk::WriteDescriptorSet{
            .dstSet = frames_[index].descriptorSet,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &bufferInfo,
        },
        vk::WriteDescriptorSet{
            .dstSet = frames_[index].descriptorSet,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &shadowImageInfo,
        },
        vk::WriteDescriptorSet{
            .dstSet = frames_[index].descriptorSet,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &shadowDebugImageInfo,
        },
        vk::WriteDescriptorSet{
            .dstSet = frames_[index].descriptorSet,
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &environmentImageInfo,
        },
    };

    device_.logicalDevice().updateDescriptorSets(writes, {});
  }
}

Renderer::FrameResult Renderer::beginFrame(glm::mat4 const &viewProjMatrix,
                                           glm::vec3 const &cameraPosition,
                                           LightingSettings const &lighting,
                                           bool shadowPassEnabled) {
  auto display = DisplaySettings{.exposureEv = lighting.exposureEv,
                                 .toneMap = lighting.toneMappingEnabled};
  validateDisplaySettings(display);
  bool dataDebug = (lighting.pbrDebugMode >= 1 && lighting.pbrDebugMode <= 5) ||
                   lighting.shadowDebugMode == 2 || lighting.shadowDebugMode == 3;
  if (dataDebug) { display.exposureEv = 0; display.toneMap = false; }
  validateSwapChainState();

  if (activeFrame_.has_value()) {
    throw std::runtime_error(
        "Cannot begin a new frame while another frame is in progress.");
  }

  displaySettings_ = display;
  std::uint32_t const frameIndex = currentFrame_;
  auto &frame = frames_[frameIndex];
  auto &commandBuffer = commandBuffers_[frameIndex];

  auto waitStart = std::chrono::steady_clock::now();
  (void)device_.logicalDevice().waitForFences(
      {*frame.inFlightFence}, true, std::numeric_limits<std::uint64_t>::max());
  cpuSyncTimes_.fenceMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - waitStart)
                              .count();
  collectFrameTimings(frame);
  collectCompletedWork();
  auto acquireStart = std::chrono::steady_clock::now();

  vk::Result acquireResult = vk::Result::eSuccess;
  std::uint32_t imageIndex = 0;

  try {
    auto acquire = swapChain_->handle().acquireNextImage(
        std::numeric_limits<std::uint64_t>::max(),
        *frame.imageAvailableSemaphore, nullptr);
    acquireResult = acquire.result;
    imageIndex = acquire.value;
  } catch (vk::OutOfDateKHRError const &) {
    return FrameResult::eSwapChainOutOfDate;
  }

  cpuSyncTimes_.acquireMs = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - acquireStart)
                                .count();

  if (acquireResult != vk::Result::eSuccess &&
      acquireResult != vk::Result::eSuboptimalKHR) {
    throw std::runtime_error("Failed to acquire swapchain image.");
  }

  if (imageIndex >= renderFinishedSemaphores_.size()) {
    throw std::runtime_error("Acquire swapchain image index is out of range");
  }

  if (imagesInFlight_[imageIndex]) {
    (void)device_.logicalDevice().waitForFences(
        {imagesInFlight_[imageIndex]}, true,
        std::numeric_limits<std::uint64_t>::max());
  }

  imagesInFlight_[imageIndex] = *frame.inFlightFence;

  device_.logicalDevice().resetFences({*frame.inFlightFence});

  commandBuffer.reset();
  updateFrameUniformBuffer(frame, viewProjMatrix, cameraPosition, lighting);

  commandBuffer.begin(vk::CommandBufferBeginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
  });

  frame.frameId = ++submittedFrameId_;
  frame.shadowEnabled = shadowPassEnabled;
  frame.uiEnabled = static_cast<bool>(uiDrawCallback_);
  if (*frame.timestamps)
    commandBuffer.resetQueryPool(*frame.timestamps, 0, 10);
  activeFrame_ = ActiveFrameState{
      .frameIndex = frameIndex,
      .imageIndex = imageIndex,
      .acquireResult = acquireResult,
  };
  device_.beginLabel(*commandBuffer, "Frame");
  timestamp(commandBuffer, 0);
  timestamp(commandBuffer, 1);
  if (shadowPassEnabled)
    device_.beginLabel(*commandBuffer, "Shadow");
  else
    timestamp(commandBuffer, 2);
  if (shadowPassEnabled) {
    beginShadowPass(commandBuffer, frame);
    activePass_ = ActivePass::eShadow;
  } else {
    beginMainPass(commandBuffer, frame, imageIndex, false);
    activePass_ = ActivePass::eMain;
  }

  return FrameResult::eSuccess;
}

void Renderer::drawObject(MeshId meshId, MaterialId materialId,
                          glm::mat4 const &modelMatrix) {
  if (!activeFrame_.has_value()) {
    throw std::runtime_error("Cannot draw without an active frame.");
  }

  if (!sceneAssets_ || meshId >= sceneAssets_->meshes_.size()) {
    throw std::runtime_error("Renderer mesh id is out of range.");
  }

  MeshGpuResources const &meshResources = sceneAssets_->meshes_[meshId];

  auto const &frameState = *activeFrame_;
  auto &commandBuffer = commandBuffers_[frameState.frameIndex];

  vk::Buffer vertexBuffer = *meshResources.vertex.buffer;
  vk::DeviceSize vertexOffset = 0;
  commandBuffer.bindVertexBuffers(0, {vertexBuffer}, {vertexOffset});
  commandBuffer.bindIndexBuffer(*meshResources.index.buffer, 0,
                                vk::IndexType::eUint32);

  auto const &materialResource = materials().material(materialId);
  PushConstants pushConstants{
      .transform = modelMatrix,
      .materialTint = materialResource.tint,
      .surfaceParams = materialResource.surfaceParams,
      .alphaParams = materialResource.alphaParams,
  };
  if (!normalMapsEnabled_) {
    pushConstants.surfaceParams.z = 0.0f;
  }
  if (!parallaxEnabled_) {
    pushConstants.surfaceParams.w = 0.0f;
  }

  if (activePass_ == ActivePass::eShadow) {
    if (materialResource.alphaMode == AlphaMode::Blend) {
      throw std::runtime_error(
          "Transparent materials are not supported in the shadow pass.");
    }
    MaterialPipelineVariant const variant = selectMaterialPipeline(
        materialResource.alphaMode, materialResource.doubleSided,
        RasterPass::Shadow);
    vk::raii::Pipeline const &pipeline =
        variant == MaterialPipelineVariant::ShadowDoubleSided
            ? doubleSidedShadowPipeline_
            : shadowPipeline_;
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                     *pipelineLayout_, 1,
                                     {materialResource.descriptorSet}, {});
    commandBuffer.pushConstants<PushConstants>(
        *pipelineLayout_,
        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        0, pushConstants);
    commandBuffer.drawIndexed(meshResources.indexCount, 1, 0, 0, 0);
    return;
  }

  if (activePass_ != ActivePass::eMain) {
    throw std::runtime_error("Renderer has no active draw pass.");
  }

  MaterialPipelineVariant const variant =
      selectMaterialPipeline(materialResource.alphaMode,
                             materialResource.doubleSided, RasterPass::Main);
  vk::raii::Pipeline const *pipeline = nullptr;
  switch (variant) {
  case MaterialPipelineVariant::OpaqueSingleSided:
    pipeline = &graphicsPipeline_;
    break;
  case MaterialPipelineVariant::OpaqueDoubleSided:
    pipeline = &doubleSidedGraphicsPipeline_;
    break;
  case MaterialPipelineVariant::TransparentSingleSided:
    pipeline = &transparentPipeline_;
    break;
  case MaterialPipelineVariant::TransparentDoubleSided:
    pipeline = &doubleSidedTransparentPipeline_;
    break;
  case MaterialPipelineVariant::ShadowSingleSided:
  case MaterialPipelineVariant::ShadowDoubleSided:
    throw std::runtime_error("Shadow pipeline selected during the main pass.");
  }
  commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, **pipeline);
  commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   *pipelineLayout_, 1,
                                   {materialResource.descriptorSet}, {});

  commandBuffer.pushConstants<PushConstants>(
      *pipelineLayout_,
      vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
      pushConstants);

  commandBuffer.drawIndexed(meshResources.indexCount, 1, 0, 0, 0);
}

void Renderer::drawEnvironment() {
  if (!activeFrame_.has_value() || activePass_ != ActivePass::eMain) {
    throw std::runtime_error(
        "Environment draw requires an active main rendering pass.");
  }

  ActiveFrameState const frameState = *activeFrame_;
  auto &commandBuffer = commandBuffers_[frameState.frameIndex];
  commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                             *environmentPipeline_);
  commandBuffer.bindDescriptorSets(
      vk::PipelineBindPoint::eGraphics, *pipelineLayout_, 0,
      {frames_[frameState.frameIndex].descriptorSet}, {});
  commandBuffer.draw(3, 1, 0, 0);
}

void Renderer::drawAabb(Aabb const &bounds, glm::vec4 const &color) {
  if (!bounds.valid) {
    return;
  }

  if (!activeFrame_.has_value()) {
    throw std::runtime_error("Cannot draw AABB without an active frame.");
  }

  if (activePass_ != ActivePass::eMain) {
    throw std::runtime_error("AABB debug draw is only valid in the main pass.");
  }

  auto const &frameState = *activeFrame_;
  auto &frame = frames_[frameState.frameIndex];
  auto &commandBuffer = commandBuffers_[frameState.frameIndex];

  glm::vec3 const size = bounds.max - bounds.min;
  glm::vec3 const center = (bounds.min + bounds.max) * 0.5f;
  glm::mat4 transform =
      glm::scale(glm::translate(glm::mat4{1.0f}, center), size);
  PushConstants pushConstants{
      .transform = transform,
      .materialTint = color,
  };

  vk::Buffer vertexBuffer = *debugAabbLineResources_.vertex.buffer;
  vk::DeviceSize vertexOffset = 0;
  commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                             *debugLinePipeline_);
  commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   *pipelineLayout_, 0, {frame.descriptorSet},
                                   {});
  commandBuffer.bindVertexBuffers(0, {vertexBuffer}, {vertexOffset});
  commandBuffer.bindIndexBuffer(*debugAabbLineResources_.index.buffer, 0,
                                vk::IndexType::eUint32);
  commandBuffer.pushConstants<PushConstants>(
      *pipelineLayout_,
      vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
      pushConstants);
  commandBuffer.drawIndexed(debugAabbLineResources_.indexCount, 1, 0, 0, 0);
}

Renderer::FrameResult Renderer::endFrame() {
  if (!activeFrame_.has_value()) {
    throw std::runtime_error(
        "Cannot end a frame when no frame is in progress.");
  }

  if (activePass_ != ActivePass::eMain) {
    throw std::runtime_error("Cannot end a frame before the main pass.");
  }

  ActiveFrameState const frameState = *activeFrame_;
  auto &frame = frames_[frameState.frameIndex];
  auto &commandBuffer = commandBuffers_[frameState.frameIndex];

  endCommandBuffer(commandBuffer, frameState.imageIndex);

  vk::Semaphore waitSemaphore = *frame.imageAvailableSemaphore;
  vk::PipelineStageFlags waitStage =
      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  vk::CommandBuffer rawCommandBuffer = *commandBuffer;
  vk::Semaphore signalSemaphore =
      *renderFinishedSemaphores_[frameState.imageIndex];

  vk::SubmitInfo submitInfo{
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &waitSemaphore,
      .pWaitDstStageMask = &waitStage,
      .commandBufferCount = 1,
      .pCommandBuffers = &rawCommandBuffer,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &signalSemaphore,
  };

  auto submitStart = std::chrono::steady_clock::now();
  device_.graphicsQueue().submit({submitInfo}, *frame.inFlightFence);
  frame.submitted = true;
  cpuSyncTimes_.submitMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - submitStart)
                               .count();

  vk::SwapchainKHR swapChainHandle = *swapChain_->handle();
  vk::PresentInfoKHR presentInfo{
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &signalSemaphore,
      .swapchainCount = 1,
      .pSwapchains = &swapChainHandle,
      .pImageIndices = &frameState.imageIndex,
  };

  auto advanceFrame = [this]() {
    currentFrame_ = (currentFrame_ + 1) % kFramesInFlight;
  };

  auto presentStart = std::chrono::steady_clock::now();
  vk::Result presentResult = vk::Result::eSuccess;
  try {
    presentResult = device_.presentQueue().presentKHR(presentInfo);
  } catch (vk::OutOfDateKHRError const &) {
    activeFrame_.reset();
    activePass_ = ActivePass::eNone;
    advanceFrame();
    return FrameResult::eSwapChainOutOfDate;
  }

  cpuSyncTimes_.presentMs = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - presentStart)
                                .count();
  if (presentResult != vk::Result::eSuccess &&
      presentResult != vk::Result::eSuboptimalKHR) {
    throw std::runtime_error("Failed to present swapchain image.");
  }

  activeFrame_.reset();
  activePass_ = ActivePass::eNone;

  if (frameState.acquireResult == vk::Result::eSuboptimalKHR ||
      presentResult == vk::Result::eSuboptimalKHR) {
    advanceFrame();
    return FrameResult::eSwapChainSuboptimal;
  }

  advanceFrame();
  return FrameResult::eSuccess;
}

Renderer::FrameResult Renderer::drawFrame(MeshId meshId, MaterialId materialId,
                                          glm::mat4 const &modelMatrix,
                                          glm::mat4 const &viewProjMatrix,
                                          glm::vec3 const &cameraPosition,
                                          LightingSettings const &lighting) {
  FrameResult beginResult =
      beginFrame(viewProjMatrix, cameraPosition, lighting, true);
  if (beginResult != FrameResult::eSuccess) {
    return beginResult;
  }

  drawObject(meshId, materialId, modelMatrix);
  beginMainPass();
  drawObject(meshId, materialId, modelMatrix);
  return endFrame();
}

std::vector<char> Renderer::readBinaryFile(char const *path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file) {
    throw std::runtime_error("Failed to open shader file: " +
                             std::string(path));
  }

  std::streamsize size = file.tellg();
  if (size <= 0) {
    throw std::runtime_error("Shader file is empty: " + std::string(path));
  }

  std::vector<char> buffer(static_cast<std::size_t>(size));
  file.seekg(0);
  file.read(buffer.data(), size);
  return buffer;
}

void Renderer::validateSwapChainCandidate(SwapChain const &swapChain) const {
  if (swapChain.images().empty()) {
    throw std::runtime_error("Swapchain candidate has no images.");
  }

  if (swapChain.imageViews().size() != swapChain.images().size()) {
    throw std::runtime_error(
        "Swapchain candidate image view count does not match image count.");
  }

  if (swapChain.imageFormat() == vk::Format::eUndefined) {
    throw std::runtime_error("Swapchain candidate has an undefined format.");
  }

  if (swapChain.extent().width == 0 || swapChain.extent().height == 0) {
    throw std::runtime_error("Swapchain candidate has an invalid extent.");
  }
}

void Renderer::validateSwapChainState() const {
  if (swapChain_ == nullptr) {
    throw std::runtime_error("Renderer is not initialized with a swapchain.");
  }

  if (frames_.size() != kFramesInFlight) {
    throw std::runtime_error("Renderer frame resource count is invalid.");
  }

  if (commandBuffers_.size() != kFramesInFlight) {
    throw std::runtime_error("Renderer command buffer count is invalid.");
  }

  if (currentFrame_ >= frames_.size()) {
    throw std::runtime_error("Renderer current frame index is out of range.");
  }

  auto const imageCount = swapChain_->images().size();
  if (imageCount == 0) {
    throw std::runtime_error("Renderer swapchain has no images.");
  }

  if (activeFrame_.has_value()) {
    if (activeFrame_->frameIndex >= frames_.size()) {
      throw std::runtime_error("Renderer active frame index is out of range.");
    }

    if (activeFrame_->imageIndex >= imageCount) {
      throw std::runtime_error(
          "Renderer active swapchain image index is out of range.");
    }
  }

  if (pipelineLayout_ == nullptr) {
    throw std::runtime_error("Renderer pipeline layout is not initialized.");
  }

  if (graphicsPipeline_ == nullptr || doubleSidedGraphicsPipeline_ == nullptr) {
    throw std::runtime_error("Renderer graphics pipeline is not initialized.");
  }

  if (transparentPipeline_ == nullptr ||
      doubleSidedTransparentPipeline_ == nullptr) {
    throw std::runtime_error(
        "Renderer transparent pipeline is not initialized.");
  }

  if (shadowPipeline_ == nullptr || doubleSidedShadowPipeline_ == nullptr) {
    throw std::runtime_error("Renderer shadow pipeline is not initialized.");
  }

  if (debugLinePipeline_ == nullptr || environmentPipeline_ == nullptr) {
    throw std::runtime_error(
        "Renderer debug line pipeline is not initialized.");
  }

  if (swapChain_->imageViews().size() != imageCount) {
    throw std::runtime_error(
        "Renderer swapchain image view count does not match image count.");
  }

  if (renderFinishedSemaphores_.size() != imageCount ||
      imagesInFlight_.size() != imageCount ||
      swapChainImageLayouts_.size() != imageCount) {
    throw std::runtime_error(
        "Renderer swapchain-dependent resource counts are inconsistent.");
  }

  for (auto const &frame : frames_) {
    if (frame.uniform.buffer == nullptr ||
        !frame.uniform.valid() ||
        frame.descriptorSet == nullptr) {
      throw std::runtime_error(
          "Renderer frame uniform resources are not initialized.");
    }
  }

  if (!depthResources_.storage.valid() ||
      depthResources_.imageView == nullptr) {
    throw std::runtime_error("Renderer depth resources are not initialized.");
  }

  if (!shadowResources_.storage.valid() ||
      shadowResources_.imageView == nullptr ||
      shadowResources_.sampler == nullptr ||
      shadowResources_.debugSampler == nullptr) {
    throw std::runtime_error("Renderer shadow resources are not initialized.");
  }

  if (sceneAssets_)
    for (auto const &meshResources : sceneAssets_->meshes_) {
      if (meshResources.vertex.buffer == nullptr ||
          !meshResources.vertex.valid() ||
          meshResources.index.buffer == nullptr ||
          !meshResources.index.valid() ||
          meshResources.indexCount == 0) {
        throw std::runtime_error(
            "Renderer mesh GPU resources are not initialized.");
      }
    }

  if (debugAabbLineResources_.vertex.buffer == nullptr ||
      !debugAabbLineResources_.vertex.valid() ||
      debugAabbLineResources_.index.buffer == nullptr ||
      !debugAabbLineResources_.index.valid() ||
      debugAabbLineResources_.indexCount == 0) {
    throw std::runtime_error(
        "Renderer debug AABB resources are not initialized.");
  }
}

vk::raii::Pipeline Renderer::createEnvironmentPipeline(
    SwapChain const &swapChain,
    vk::raii::PipelineLayout const &pipelineLayout) const {
  auto vertCode = readBinaryFile("shaders/environment.vert.spv");
  auto fragCode = readBinaryFile("shaders/environment.frag.spv");

  vk::raii::ShaderModule vertexShaderModule(
      device_.logicalDevice(),
      vk::ShaderModuleCreateInfo{
          .codeSize = vertCode.size(),
          .pCode = reinterpret_cast<std::uint32_t const *>(vertCode.data()),
      });
  vk::raii::ShaderModule fragmentShaderModule(
      device_.logicalDevice(),
      vk::ShaderModuleCreateInfo{
          .codeSize = fragCode.size(),
          .pCode = reinterpret_cast<std::uint32_t const *>(fragCode.data()),
      });
  std::array shaderStages = {
      vk::PipelineShaderStageCreateInfo{
          .stage = vk::ShaderStageFlagBits::eVertex,
          .module = *vertexShaderModule,
          .pName = "main",
      },
      vk::PipelineShaderStageCreateInfo{
          .stage = vk::ShaderStageFlagBits::eFragment,
          .module = *fragmentShaderModule,
          .pName = "main",
      },
  };

  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
      .topology = vk::PrimitiveTopology::eTriangleList,
  };
  vk::PipelineViewportStateCreateInfo viewportState{
      .viewportCount = 1,
      .scissorCount = 1,
  };
  vk::PipelineRasterizationStateCreateInfo rasterizer{
      .polygonMode = vk::PolygonMode::eFill,
      .cullMode = vk::CullModeFlagBits::eNone,
      .frontFace = vk::FrontFace::eClockwise,
      .lineWidth = 1.0f,
  };
  vk::PipelineMultisampleStateCreateInfo multisampling{
      .rasterizationSamples = vk::SampleCountFlagBits::e1,
  };
  vk::PipelineDepthStencilStateCreateInfo depthStencil{
      .depthTestEnable = true,
      .depthWriteEnable = false,
      .depthCompareOp = vk::CompareOp::eLessOrEqual,
  };
  vk::PipelineColorBlendAttachmentState colorBlendAttachment{
      .colorWriteMask =
          vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
  };
  vk::PipelineColorBlendStateCreateInfo colorBlending{
      .attachmentCount = 1,
      .pAttachments = &colorBlendAttachment,
  };
  std::array dynamicStates = {vk::DynamicState::eViewport,
                              vk::DynamicState::eScissor};
  vk::PipelineDynamicStateCreateInfo dynamicState{
      .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
      .pDynamicStates = dynamicStates.data(),
  };
  vk::Format colorAttachmentFormat = HdrOutput::sceneFormat;
  vk::PipelineRenderingCreateInfo renderingInfo{
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &colorAttachmentFormat,
      .depthAttachmentFormat = kDepthFormat,
  };
  vk::GraphicsPipelineCreateInfo pipelineInfo{
      .pNext = &renderingInfo,
      .stageCount = static_cast<std::uint32_t>(shaderStages.size()),
      .pStages = shaderStages.data(),
      .pVertexInputState = &vertexInput,
      .pInputAssemblyState = &inputAssembly,
      .pViewportState = &viewportState,
      .pRasterizationState = &rasterizer,
      .pMultisampleState = &multisampling,
      .pDepthStencilState = &depthStencil,
      .pColorBlendState = &colorBlending,
      .pDynamicState = &dynamicState,
      .layout = *pipelineLayout,
  };
  return vk::raii::Pipeline(device_.logicalDevice(), nullptr, pipelineInfo);
}

void Renderer::createCommandPool() {
  vk::CommandPoolCreateInfo createInfo{
      .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
      .queueFamilyIndex = device_.graphicsQueueFamilyIndex(),
  };
  commandPool_ = vk::raii::CommandPool(device_.logicalDevice(), createInfo);
}

vk::raii::Pipeline
Renderer::createGraphicsPipeline(SwapChain const &swapChain,
                                 vk::raii::PipelineLayout const &pipelineLayout,
                                 vk::CullModeFlagBits cullMode) const {
  auto vertCode = readBinaryFile("shaders/triangle.vert.spv");
  auto fragCode = readBinaryFile("shaders/triangle.frag.spv");

  vk::ShaderModuleCreateInfo vertexShaderCreateInfo{
      .codeSize = vertCode.size(),
      .pCode = reinterpret_cast<std::uint32_t const *>(vertCode.data()),
  };
  vk::ShaderModuleCreateInfo fragmentShaderCreateInfo{
      .codeSize = fragCode.size(),
      .pCode = reinterpret_cast<std::uint32_t const *>(fragCode.data()),
  };

  vk::raii::ShaderModule vertexShaderModule(device_.logicalDevice(),
                                            vertexShaderCreateInfo);
  vk::raii::ShaderModule fragmentShaderModule(device_.logicalDevice(),
                                              fragmentShaderCreateInfo);

  std::array shaderStages = {
      vk::PipelineShaderStageCreateInfo{
          .stage = vk::ShaderStageFlagBits::eVertex,
          .module = *vertexShaderModule,
          .pName = "main",
      },
      vk::PipelineShaderStageCreateInfo{
          .stage = vk::ShaderStageFlagBits::eFragment,
          .module = *fragmentShaderModule,
          .pName = "main",
      },
  };

  auto bindingDescription = Vertex::bindingDescription();
  auto attributeDescriptions = Vertex::attributeDescriptions();

  vk::PipelineVertexInputStateCreateInfo vertexInputInfo{
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &bindingDescription,
      .vertexAttributeDescriptionCount =
          static_cast<std::uint32_t>(attributeDescriptions.size()),
      .pVertexAttributeDescriptions = attributeDescriptions.data(),
  };
  vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
      .topology = vk::PrimitiveTopology::eTriangleList,
      .primitiveRestartEnable = false,
  };
  vk::PipelineViewportStateCreateInfo viewportState{
      .viewportCount = 1,
      .scissorCount = 1,
  };
  vk::PipelineRasterizationStateCreateInfo rasterizer{
      .depthClampEnable = false,
      .rasterizerDiscardEnable = false,
      .polygonMode = vk::PolygonMode::eFill,
      .cullMode = cullMode,
      .frontFace = rasterizerDebugSettings_.frontFace,
      .depthBiasEnable = false,
      .lineWidth = 1.0f,
  };
  vk::PipelineMultisampleStateCreateInfo multisampling{
      .rasterizationSamples = vk::SampleCountFlagBits::e1,
      .sampleShadingEnable = false,
  };

  vk::PipelineColorBlendAttachmentState colorBlendAttachment{
      .blendEnable = false,
      .colorWriteMask =
          vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
  };
  vk::PipelineColorBlendStateCreateInfo colorBlending{
      .logicOpEnable = false,
      .attachmentCount = 1,
      .pAttachments = &colorBlendAttachment,
  };

  vk::PipelineDepthStencilStateCreateInfo depthStencil{
      .depthTestEnable = true,
      .depthWriteEnable = true,
      .depthCompareOp = vk::CompareOp::eLess,
      .depthBoundsTestEnable = false,
      .stencilTestEnable = false,
  };

  std::array dynamicStates = {
      vk::DynamicState::eViewport,
      vk::DynamicState::eScissor,
  };
  vk::PipelineDynamicStateCreateInfo dynamicState{
      .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
      .pDynamicStates = dynamicStates.data(),
  };

  vk::Format colorAttachmentFormat = HdrOutput::sceneFormat;
  vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &colorAttachmentFormat,
      .depthAttachmentFormat = kDepthFormat,
  };

  vk::GraphicsPipelineCreateInfo pipelineCreateInfo{
      .pNext = &pipelineRenderingCreateInfo,
      .stageCount = static_cast<std::uint32_t>(shaderStages.size()),
      .pStages = shaderStages.data(),
      .pVertexInputState = &vertexInputInfo,
      .pInputAssemblyState = &inputAssembly,
      .pViewportState = &viewportState,
      .pRasterizationState = &rasterizer,
      .pMultisampleState = &multisampling,
      .pDepthStencilState = &depthStencil,
      .pColorBlendState = &colorBlending,
      .pDynamicState = &dynamicState,
      .layout = *pipelineLayout,
  };

  return vk::raii::Pipeline(device_.logicalDevice(), nullptr,
                            pipelineCreateInfo);
}

vk::raii::Pipeline Renderer::createTransparentPipeline(
    SwapChain const &swapChain, vk::raii::PipelineLayout const &pipelineLayout,
    vk::CullModeFlagBits cullMode) const {
  auto vertCode = readBinaryFile("shaders/triangle.vert.spv");
  auto fragCode = readBinaryFile("shaders/triangle.frag.spv");

  vk::ShaderModuleCreateInfo vertexShaderCreateInfo{
      .codeSize = vertCode.size(),
      .pCode = reinterpret_cast<std::uint32_t const *>(vertCode.data()),
  };
  vk::ShaderModuleCreateInfo fragmentShaderCreateInfo{
      .codeSize = fragCode.size(),
      .pCode = reinterpret_cast<std::uint32_t const *>(fragCode.data()),
  };

  vk::raii::ShaderModule vertexShaderModule(device_.logicalDevice(),
                                            vertexShaderCreateInfo);
  vk::raii::ShaderModule fragmentShaderModule(device_.logicalDevice(),
                                              fragmentShaderCreateInfo);

  std::array shaderStages = {
      vk::PipelineShaderStageCreateInfo{
          .stage = vk::ShaderStageFlagBits::eVertex,
          .module = *vertexShaderModule,
          .pName = "main",
      },
      vk::PipelineShaderStageCreateInfo{
          .stage = vk::ShaderStageFlagBits::eFragment,
          .module = *fragmentShaderModule,
          .pName = "main",
      },
  };

  auto bindingDescription = Vertex::bindingDescription();
  auto attributeDescriptions = Vertex::attributeDescriptions();

  vk::PipelineVertexInputStateCreateInfo vertexInputInfo{
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &bindingDescription,
      .vertexAttributeDescriptionCount =
          static_cast<std::uint32_t>(attributeDescriptions.size()),
      .pVertexAttributeDescriptions = attributeDescriptions.data(),
  };
  vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
      .topology = vk::PrimitiveTopology::eTriangleList,
      .primitiveRestartEnable = false,
  };
  vk::PipelineViewportStateCreateInfo viewportState{
      .viewportCount = 1,
      .scissorCount = 1,
  };
  vk::PipelineRasterizationStateCreateInfo rasterizer{
      .depthClampEnable = false,
      .rasterizerDiscardEnable = false,
      .polygonMode = vk::PolygonMode::eFill,
      .cullMode = cullMode,
      .frontFace = rasterizerDebugSettings_.frontFace,
      .depthBiasEnable = false,
      .lineWidth = 1.0f,
  };
  vk::PipelineMultisampleStateCreateInfo multisampling{
      .rasterizationSamples = vk::SampleCountFlagBits::e1,
      .sampleShadingEnable = false,
  };

  vk::PipelineColorBlendAttachmentState colorBlendAttachment{
      .blendEnable = true,
      .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
      .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
      .colorBlendOp = vk::BlendOp::eAdd,
      .srcAlphaBlendFactor = vk::BlendFactor::eOne,
      .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
      .alphaBlendOp = vk::BlendOp::eAdd,
      .colorWriteMask =
          vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
  };
  vk::PipelineColorBlendStateCreateInfo colorBlending{
      .logicOpEnable = false,
      .attachmentCount = 1,
      .pAttachments = &colorBlendAttachment,
  };

  vk::PipelineDepthStencilStateCreateInfo depthStencil{
      .depthTestEnable = true,
      .depthWriteEnable = false,
      .depthCompareOp = vk::CompareOp::eLess,
      .depthBoundsTestEnable = false,
      .stencilTestEnable = false,
  };

  std::array dynamicStates = {
      vk::DynamicState::eViewport,
      vk::DynamicState::eScissor,
  };
  vk::PipelineDynamicStateCreateInfo dynamicState{
      .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
      .pDynamicStates = dynamicStates.data(),
  };

  vk::Format colorAttachmentFormat = HdrOutput::sceneFormat;
  vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &colorAttachmentFormat,
      .depthAttachmentFormat = kDepthFormat,
  };

  vk::GraphicsPipelineCreateInfo pipelineCreateInfo{
      .pNext = &pipelineRenderingCreateInfo,
      .stageCount = static_cast<std::uint32_t>(shaderStages.size()),
      .pStages = shaderStages.data(),
      .pVertexInputState = &vertexInputInfo,
      .pInputAssemblyState = &inputAssembly,
      .pViewportState = &viewportState,
      .pRasterizationState = &rasterizer,
      .pMultisampleState = &multisampling,
      .pDepthStencilState = &depthStencil,
      .pColorBlendState = &colorBlending,
      .pDynamicState = &dynamicState,
      .layout = *pipelineLayout,
  };

  return vk::raii::Pipeline(device_.logicalDevice(), nullptr,
                            pipelineCreateInfo);
}

vk::raii::Pipeline Renderer::createDebugLinePipeline(
    SwapChain const &swapChain,
    vk::raii::PipelineLayout const &pipelineLayout) const {
  auto vertCode = readBinaryFile("shaders/debug_line.vert.spv");
  auto fragCode = readBinaryFile("shaders/debug_line.frag.spv");

  vk::ShaderModuleCreateInfo vertexShaderCreateInfo{
      .codeSize = vertCode.size(),
      .pCode = reinterpret_cast<std::uint32_t const *>(vertCode.data()),
  };
  vk::ShaderModuleCreateInfo fragmentShaderCreateInfo{
      .codeSize = fragCode.size(),
      .pCode = reinterpret_cast<std::uint32_t const *>(fragCode.data()),
  };

  vk::raii::ShaderModule vertexShaderModule(device_.logicalDevice(),
                                            vertexShaderCreateInfo);
  vk::raii::ShaderModule fragmentShaderModule(device_.logicalDevice(),
                                              fragmentShaderCreateInfo);

  std::array shaderStages = {
      vk::PipelineShaderStageCreateInfo{
          .stage = vk::ShaderStageFlagBits::eVertex,
          .module = *vertexShaderModule,
          .pName = "main",
      },
      vk::PipelineShaderStageCreateInfo{
          .stage = vk::ShaderStageFlagBits::eFragment,
          .module = *fragmentShaderModule,
          .pName = "main",
      },
  };

  auto bindingDescription = Vertex::bindingDescription();
  auto attributeDescriptions = Vertex::attributeDescriptions();

  vk::PipelineVertexInputStateCreateInfo vertexInputInfo{
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &bindingDescription,
      .vertexAttributeDescriptionCount =
          static_cast<std::uint32_t>(attributeDescriptions.size()),
      .pVertexAttributeDescriptions = attributeDescriptions.data(),
  };
  vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
      .topology = vk::PrimitiveTopology::eLineList,
      .primitiveRestartEnable = false,
  };
  vk::PipelineViewportStateCreateInfo viewportState{
      .viewportCount = 1,
      .scissorCount = 1,
  };
  vk::PipelineRasterizationStateCreateInfo rasterizer{
      .depthClampEnable = false,
      .rasterizerDiscardEnable = false,
      .polygonMode = vk::PolygonMode::eFill,
      .cullMode = vk::CullModeFlagBits::eNone,
      .frontFace = vk::FrontFace::eCounterClockwise,
      .depthBiasEnable = false,
      .lineWidth = 1.0f,
  };
  vk::PipelineMultisampleStateCreateInfo multisampling{
      .rasterizationSamples = vk::SampleCountFlagBits::e1,
      .sampleShadingEnable = false,
  };

  vk::PipelineColorBlendAttachmentState colorBlendAttachment{
      .blendEnable = true,
      .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
      .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
      .colorBlendOp = vk::BlendOp::eAdd,
      .srcAlphaBlendFactor = vk::BlendFactor::eOne,
      .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
      .alphaBlendOp = vk::BlendOp::eAdd,
      .colorWriteMask =
          vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
  };
  vk::PipelineColorBlendStateCreateInfo colorBlending{
      .logicOpEnable = false,
      .attachmentCount = 1,
      .pAttachments = &colorBlendAttachment,
  };

  vk::PipelineDepthStencilStateCreateInfo depthStencil{
      .depthTestEnable = false,
      .depthWriteEnable = false,
      .depthCompareOp = vk::CompareOp::eLess,
      .depthBoundsTestEnable = false,
      .stencilTestEnable = false,
  };

  std::array dynamicStates = {
      vk::DynamicState::eViewport,
      vk::DynamicState::eScissor,
  };
  vk::PipelineDynamicStateCreateInfo dynamicState{
      .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
      .pDynamicStates = dynamicStates.data(),
  };

  vk::Format colorAttachmentFormat = HdrOutput::sceneFormat;
  vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &colorAttachmentFormat,
      .depthAttachmentFormat = kDepthFormat,
  };

  vk::GraphicsPipelineCreateInfo pipelineCreateInfo{
      .pNext = &pipelineRenderingCreateInfo,
      .stageCount = static_cast<std::uint32_t>(shaderStages.size()),
      .pStages = shaderStages.data(),
      .pVertexInputState = &vertexInputInfo,
      .pInputAssemblyState = &inputAssembly,
      .pViewportState = &viewportState,
      .pRasterizationState = &rasterizer,
      .pMultisampleState = &multisampling,
      .pDepthStencilState = &depthStencil,
      .pColorBlendState = &colorBlending,
      .pDynamicState = &dynamicState,
      .layout = *pipelineLayout,
  };

  return vk::raii::Pipeline(device_.logicalDevice(), nullptr,
                            pipelineCreateInfo);
}

vk::raii::Pipeline
Renderer::createShadowPipeline(vk::raii::PipelineLayout const &pipelineLayout,
                               vk::CullModeFlagBits cullMode) const {
  auto vertCode = readBinaryFile("shaders/shadow.vert.spv");
  auto fragCode = readBinaryFile("shaders/shadow.frag.spv");

  vk::ShaderModuleCreateInfo vertexShaderCreateInfo{
      .codeSize = vertCode.size(),
      .pCode = reinterpret_cast<std::uint32_t const *>(vertCode.data()),
  };
  vk::ShaderModuleCreateInfo fragmentShaderCreateInfo{
      .codeSize = fragCode.size(),
      .pCode = reinterpret_cast<std::uint32_t const *>(fragCode.data()),
  };

  vk::raii::ShaderModule vertexShaderModule(device_.logicalDevice(),
                                            vertexShaderCreateInfo);
  vk::raii::ShaderModule fragmentShaderModule(device_.logicalDevice(),
                                              fragmentShaderCreateInfo);

  std::array shaderStages = {
      vk::PipelineShaderStageCreateInfo{
          .stage = vk::ShaderStageFlagBits::eVertex,
          .module = *vertexShaderModule,
          .pName = "main",
      },
      vk::PipelineShaderStageCreateInfo{
          .stage = vk::ShaderStageFlagBits::eFragment,
          .module = *fragmentShaderModule,
          .pName = "main",
      },
  };

  auto bindingDescription = Vertex::bindingDescription();
  auto attributeDescriptions = Vertex::attributeDescriptions();

  vk::PipelineVertexInputStateCreateInfo vertexInputInfo{
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &bindingDescription,
      .vertexAttributeDescriptionCount =
          static_cast<std::uint32_t>(attributeDescriptions.size()),
      .pVertexAttributeDescriptions = attributeDescriptions.data(),
  };
  vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
      .topology = vk::PrimitiveTopology::eTriangleList,
      .primitiveRestartEnable = false,
  };
  vk::PipelineViewportStateCreateInfo viewportState{
      .viewportCount = 1,
      .scissorCount = 1,
  };
  vk::PipelineRasterizationStateCreateInfo rasterizer{
      .depthClampEnable = false,
      .rasterizerDiscardEnable = false,
      .polygonMode = vk::PolygonMode::eFill,
      .cullMode = cullMode,
      .frontFace = rasterizerDebugSettings_.frontFace,
      .depthBiasEnable = true,
      .depthBiasConstantFactor = 1.25f,
      .depthBiasSlopeFactor = 1.75f,
      .lineWidth = 1.0f,
  };
  vk::PipelineMultisampleStateCreateInfo multisampling{
      .rasterizationSamples = vk::SampleCountFlagBits::e1,
      .sampleShadingEnable = false,
  };
  vk::PipelineColorBlendStateCreateInfo colorBlending{
      .logicOpEnable = false,
      .attachmentCount = 0,
  };
  vk::PipelineDepthStencilStateCreateInfo depthStencil{
      .depthTestEnable = true,
      .depthWriteEnable = true,
      .depthCompareOp = vk::CompareOp::eLessOrEqual,
      .depthBoundsTestEnable = false,
      .stencilTestEnable = false,
  };

  std::array dynamicStates = {
      vk::DynamicState::eViewport,
      vk::DynamicState::eScissor,
  };
  vk::PipelineDynamicStateCreateInfo dynamicState{
      .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
      .pDynamicStates = dynamicStates.data(),
  };

  vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
      .colorAttachmentCount = 0,
      .depthAttachmentFormat = kDepthFormat,
  };

  vk::GraphicsPipelineCreateInfo pipelineCreateInfo{
      .pNext = &pipelineRenderingCreateInfo,
      .stageCount = static_cast<std::uint32_t>(shaderStages.size()),
      .pStages = shaderStages.data(),
      .pVertexInputState = &vertexInputInfo,
      .pInputAssemblyState = &inputAssembly,
      .pViewportState = &viewportState,
      .pRasterizationState = &rasterizer,
      .pMultisampleState = &multisampling,
      .pDepthStencilState = &depthStencil,
      .pColorBlendState = &colorBlending,
      .pDynamicState = &dynamicState,
      .layout = *pipelineLayout,
  };

  return vk::raii::Pipeline(device_.logicalDevice(), nullptr,
                            pipelineCreateInfo);
}

void Renderer::createCommandBuffers() {
  vk::CommandBufferAllocateInfo allocateInfo{
      .commandPool = *commandPool_,
      .level = vk::CommandBufferLevel::ePrimary,
      .commandBufferCount = kFramesInFlight,
  };
  commandBuffers_ =
      vk::raii::CommandBuffers(device_.logicalDevice(), allocateInfo);
}

void Renderer::updateFrameUniformBuffer(
    FrameContext &frame, glm::mat4 const &viewProjMatrix,
    glm::vec3 const &cameraPosition, LightingSettings const &lighting) const {
  FrameUniformBufferObject ubo{};
  ubo.viewProj = viewProjMatrix;
  ubo.cameraPosition = glm::vec4(cameraPosition, 1.0f);
  glm::vec3 lightDirection = lighting.direction;
  if (glm::length(lightDirection) <= 0.0001f) {
    lightDirection = {0.0f, 1.0f, 0.0f};
  }
  ubo.lightDirection = glm::vec4(glm::normalize(lightDirection), 0.0f);
  glm::vec3 const lightColor = lighting.color * lighting.intensity;
  ubo.lightColor = glm::vec4(lightColor, 1.0f);
  ubo.ambientColor = glm::vec4(lightColor * lighting.ambientStrength, 1.0f);
  ubo.lightingParams =
      glm::vec4(lighting.diffuseStrength, lighting.specularStrength,
                lighting.shininess, static_cast<float>(lighting.pbrDebugMode));
  ubo.lightViewProj = computeLightViewProj(lighting.direction, lighting);
  ubo.shadowParams = glm::vec4(
      lighting.shadowBiasSlope, lighting.shadowBiasConstant,
      lighting.shadowPcfRadius, static_cast<float>(lighting.shadowDebugMode));
  ubo.inverseViewProj = glm::inverse(viewProjMatrix);
  ubo.environmentParams =
      glm::vec4(lighting.environmentIntensity, lighting.environmentRotation,
                lighting.environmentDiffuseStrength,
                lighting.environmentSpecularStrength);
  for (std::size_t coefficient = 0; coefficient < environmentSh_.size();
       ++coefficient) {
    ubo.environmentSh[coefficient] =
        glm::vec4(environmentSh_[coefficient], 0.0f);
  }

  frame.uniform.write(std::as_bytes(std::span{&ubo, 1}));
}

void Renderer::beginShadowPass(vk::raii::CommandBuffer const &commandBuffer,
                               FrameContext const &frame) {
  vk::ClearValue depthClearValue{
      .depthStencil =
          vk::ClearDepthStencilValue{
              .depth = 1.0f,
              .stencil = 0,
          },
  };

  transitionShadowImage(commandBuffer, vk::ImageLayout::eDepthAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderSampledRead,
                        vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests,
                        vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                            vk::AccessFlagBits2::eDepthStencilAttachmentWrite);

  vk::RenderingAttachmentInfo depthAttachment{
      .imageView = *shadowResources_.imageView,
      .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
      .loadOp = vk::AttachmentLoadOp::eClear,
      .storeOp = vk::AttachmentStoreOp::eStore,
      .clearValue = depthClearValue,
  };

  vk::RenderingInfo renderingInfo{
      .renderArea =
          {
              .offset = {0, 0},
              .extent = {kShadowMapSize, kShadowMapSize},
          },
      .layerCount = 1,
      .colorAttachmentCount = 0,
      .pDepthAttachment = &depthAttachment,
  };

  commandBuffer.beginRendering(renderingInfo);

  commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                             *shadowPipeline_);
  commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   *pipelineLayout_, 0, {frame.descriptorSet},
                                   {});

  vk::Viewport viewport{
      .x = 0.0f,
      .y = 0.0f,
      .width = static_cast<float>(kShadowMapSize),
      .height = static_cast<float>(kShadowMapSize),
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  vk::Rect2D scissor{
      .offset = {0, 0},
      .extent = {kShadowMapSize, kShadowMapSize},
  };
  commandBuffer.setViewport(0, {viewport});
  commandBuffer.setScissor(0, {scissor});
}

void Renderer::beginMainPass() {
  if (!activeFrame_.has_value()) {
    throw std::runtime_error("Cannot begin main pass without an active frame.");
  }

  if (activePass_ != ActivePass::eShadow) {
    throw std::runtime_error("Cannot begin main pass before shadow pass.");
  }

  ActiveFrameState const frameState = *activeFrame_;
  beginMainPass(commandBuffers_[frameState.frameIndex],
                frames_[frameState.frameIndex], frameState.imageIndex, true);
  activePass_ = ActivePass::eMain;
}

void Renderer::beginMainPass(vk::raii::CommandBuffer const &commandBuffer,
                             FrameContext const &frame,
                             std::uint32_t imageIndex, bool shadowPassEnabled) {
  if (shadowPassEnabled) {
    commandBuffer.endRendering();
    timestamp(commandBuffer, 2);
    device_.endLabel(*commandBuffer);

    transitionShadowImage(commandBuffer, vk::ImageLayout::eDepthReadOnlyOptimal,
                          vk::PipelineStageFlagBits2::eLateFragmentTests,
                          vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                          vk::PipelineStageFlagBits2::eFragmentShader,
                          vk::AccessFlagBits2::eShaderSampledRead);
  }

  hdrOutput_->prepareScene(*commandBuffer);

  vk::ClearValue clearValue{
      .color =
          vk::ClearColorValue(std::array<float, 4>{0.05f, 0.07f, 0.10f, 1.0f}),
  };

  vk::ClearValue depthClearValue{
      .depthStencil =
          vk::ClearDepthStencilValue{
              .depth = 1.0f,
              .stencil = 0,
          },
  };

  transitionDepthImage(commandBuffer, vk::ImageLayout::eDepthAttachmentOptimal,
                       vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests,
                       {},
                       vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests,
                       vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                           vk::AccessFlagBits2::eDepthStencilAttachmentWrite);

  vk::RenderingAttachmentInfo depthAttachment{
      .imageView = *depthResources_.imageView,
      .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
      .loadOp = vk::AttachmentLoadOp::eClear,
      .storeOp = vk::AttachmentStoreOp::eDontCare,
      .clearValue = depthClearValue,
  };

  vk::RenderingAttachmentInfo colorAttachment{
      .imageView = hdrOutput_->sceneView(),
      .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
      .loadOp = vk::AttachmentLoadOp::eClear,
      .storeOp = vk::AttachmentStoreOp::eStore,
      .clearValue = clearValue,
  };
  vk::RenderingInfo renderingInfo{
      .renderArea =
          {
              .offset = {0, 0},
              .extent = swapChain_->extent(),
          },
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &colorAttachment,
      .pDepthAttachment = &depthAttachment,
  };

  timestamp(commandBuffer, 3);
  device_.beginLabel(*commandBuffer,
                     "Main scene (sky + opaque + mask + blend + debug)");
  commandBuffer.beginRendering(renderingInfo);

  commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                             *graphicsPipeline_);
  commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   *pipelineLayout_, 0, {frame.descriptorSet},
                                   {});

  vk::Viewport viewport{
      .x = 0.0f,
      .y = 0.0f,
      .width = static_cast<float>(swapChain_->extent().width),
      .height = static_cast<float>(swapChain_->extent().height),
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  vk::Rect2D scissor{
      .offset = {0, 0},
      .extent = swapChain_->extent(),
  };
  commandBuffer.setViewport(0, {viewport});
  commandBuffer.setScissor(0, {scissor});
}

void Renderer::endCommandBuffer(vk::raii::CommandBuffer const &commandBuffer,
                                std::uint32_t imageIndex) {
  commandBuffer.endRendering();
  timestamp(commandBuffer, 4);
  device_.endLabel(*commandBuffer);
  device_.beginLabel(*commandBuffer, "Display output (exposure + filmic + sRGB)");
  timestamp(commandBuffer, 5);
  transitionSwapChainImage(commandBuffer, imageIndex,
                           vk::ImageLayout::eColorAttachmentOptimal,
                           vk::PipelineStageFlagBits2::eAllCommands,
                           vk::AccessFlagBits2::eMemoryRead,
                           vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                           vk::AccessFlagBits2::eColorAttachmentWrite);
  hdrOutput_->drawDisplay(*commandBuffer, *swapChain_->imageViews()[imageIndex], displaySettings_);
  timestamp(commandBuffer, 6);
  device_.endLabel(*commandBuffer);
  device_.beginLabel(*commandBuffer, "UI");
  timestamp(commandBuffer, 7);
  if (uiDrawCallback_) uiDrawCallback_(*commandBuffer);
  timestamp(commandBuffer, 8);
  device_.endLabel(*commandBuffer);
  commandBuffer.endRendering();

  transitionSwapChainImage(commandBuffer, imageIndex,
                           vk::ImageLayout::ePresentSrcKHR,
                           vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                           vk::AccessFlagBits2::eColorAttachmentWrite,
                           vk::PipelineStageFlagBits2::eAllCommands,
                           vk::AccessFlagBits2::eMemoryRead);

  timestamp(commandBuffer, 9);
  device_.endLabel(*commandBuffer);
  commandBuffer.end();
}

void Renderer::transitionSwapChainImage(
    vk::raii::CommandBuffer const &commandBuffer, std::uint32_t imageIndex,
    vk::ImageLayout newLayout, vk::PipelineStageFlags2 srcStageMask,
    vk::AccessFlags2 srcAccessMask, vk::PipelineStageFlags2 dstStageMask,
    vk::AccessFlags2 dstAccessMask) {
  vk::ImageMemoryBarrier2 barrier{
      .srcStageMask = srcStageMask,
      .srcAccessMask = srcAccessMask,
      .dstStageMask = dstStageMask,
      .dstAccessMask = dstAccessMask,
      .oldLayout = swapChainImageLayouts_[imageIndex],
      .newLayout = newLayout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = swapChain_->images()[imageIndex],
      .subresourceRange =
          {
              .aspectMask = vk::ImageAspectFlagBits::eColor,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };
  vk::DependencyInfo dependencyInfo{
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
  };

  commandBuffer.pipelineBarrier2(dependencyInfo);
  swapChainImageLayouts_[imageIndex] = newLayout;
}

void Renderer::transitionDepthImage(
    vk::raii::CommandBuffer const &commanderBuffer, vk::ImageLayout newLayout,
    vk::PipelineStageFlags2 srcStageMask, vk::AccessFlags2 srcAccessMask,
    vk::PipelineStageFlags2 dstStageMask, vk::AccessFlags2 dstAccessMask) {
  vk::ImageMemoryBarrier2 barrier{
      .srcStageMask = srcStageMask,
      .srcAccessMask = srcAccessMask,
      .dstStageMask = dstStageMask,
      .dstAccessMask = dstAccessMask,
      .oldLayout = depthResources_.layout,
      .newLayout = newLayout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = *depthResources_.storage.image,
      .subresourceRange =
          {
              .aspectMask = vk::ImageAspectFlagBits::eDepth,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };
  vk::DependencyInfo dependencyInfo{
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
  };

  commanderBuffer.pipelineBarrier2(dependencyInfo);
  depthResources_.layout = newLayout;
}

void Renderer::transitionShadowImage(
    vk::raii::CommandBuffer const &commandBuffer, vk::ImageLayout newLayout,
    vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
    vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess) {
  if (shadowResources_.layout == vk::ImageLayout::eUndefined) {
    srcStage = vk::PipelineStageFlagBits2::eNone;
    srcAccess = {};
  }

  vk::ImageMemoryBarrier2 barrier{
      .srcStageMask = srcStage,
      .srcAccessMask = srcAccess,
      .dstStageMask = dstStage,
      .dstAccessMask = dstAccess,
      .oldLayout = shadowResources_.layout,
      .newLayout = newLayout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = *shadowResources_.storage.image,
      .subresourceRange =
          {
              .aspectMask = vk::ImageAspectFlagBits::eDepth,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };
  vk::DependencyInfo dependencyInfo{
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
  };

  commandBuffer.pipelineBarrier2(dependencyInfo);
  shadowResources_.layout = newLayout;
}

void Renderer::timestamp(vk::raii::CommandBuffer const &command,
                         std::uint32_t query) {
  if (!timestampBits_ || !activeFrame_)
    return;
  command.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe,
                          *frames_[activeFrame_->frameIndex].timestamps, query);
}
void Renderer::collectFrameTimings(FrameContext &frame) {
  if (!frame.submitted)
    return;
  resourceStatistics_.completedFrameId =
      std::max(resourceStatistics_.completedFrameId, frame.frameId);
  GpuTimings result{.frameId = frame.frameId};
  if (*frame.timestamps) {
    std::array<std::uint64_t, 20> values{};
    auto status = vkGetQueryPoolResults(
        static_cast<VkDevice>(device_.deviceHandle()),
        static_cast<VkQueryPool>(*frame.timestamps), 0, 10, sizeof(values),
        values.data(), 2 * sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (status != VK_SUCCESS && status != VK_NOT_READY)
      throw std::runtime_error("GPU timestamp readback failed");
    bool ready = true;
    for (unsigned i = 0; i < 10; ++i)
      ready = ready && values[2 * i + 1] != 0;
    if (!ready)
      return;
    auto elapsed = [&](unsigned a, unsigned b) {
      return timestampMilliseconds(values[2 * a], values[2 * b], timestampBits_,
                                   timestampPeriod_);
    };
    result.valid = true;
    result.totalMs = elapsed(0, 9);
    result.shadowMs = frame.shadowEnabled ? elapsed(1, 2) : 0;
    result.mainMs = elapsed(3, 4);
    result.outputMs = elapsed(5, 6);
    result.uiMs = frame.uiEnabled ? elapsed(7, 8) : 0;
  }
  gpuTimings_ = result;
  frame.submitted = false;
}
void Renderer::collectCompletedWork() {
  for (auto &frame : frames_) {
    if (frame.submitted &&
        frame.inFlightFence.getStatus() == vk::Result::eSuccess)
      collectFrameTimings(frame);
  }
  std::erase_if(pendingSceneUploads_, [](auto const &candidate) {
    if (!candidate->uploads_ || candidate->uploads_->ready()) {
      candidate->uploads_.reset();
      return true;
    }
    return false;
  });
  std::erase_if(retiredScenes_, [this](auto &retired) {
    if (retired.lastFrame > resourceStatistics_.completedFrameId)
      return false;
    if (retired.releaseUi)
      retired.releaseUi();
    return true;
  });
  resourceStatistics_.pendingSceneUploads = pendingSceneUploads_.size();
  resourceStatistics_.retiredScenes = retiredScenes_.size();
}
