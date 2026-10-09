#include "pch.hpp"

#include "renderer.hpp"

#include "hdr_image.hpp"
#include "hdr_ibl_cache.hpp"
#include "material_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <bit>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
struct PushConstants {
  glm::mat4 transform{1.0f};
  glm::vec4 materialTint{1.0f};
  glm::vec4 surfaceParams{1.0f, 0.04f, 0.0f, 0.0f};
  glm::vec4 alphaParams{0.0f, 0.5f, 0.0f, 0.0f};
  glm::ivec4 shadowPass{-1, 0, 0, 0};
};

static_assert(sizeof(PushConstants) == 128);

struct FrameUniformBufferObject {
  glm::mat4 viewProj{1.0f};
  glm::vec4 cameraPosition{0.0f};
  glm::vec4 lightDirection{0.0f, 1.0f, 0.3f, 0.0f};
  glm::vec4 lightColor{1.0f, 0.98f, 0.92f, 1.0f};
  glm::vec4 ambientColor{0.08f, 0.08f, 0.1f, 1.0f};
  glm::vec4 lightingParams{1.0f, 0.35f, 1.0f, 0.0f};
  glm::mat4 lightViewProj{1.0f};
  glm::vec4 shadowParams{0.0025f, 0.0007f, 1.0f, 1.0f};
  glm::mat4 inverseViewProj{1.0f};
  glm::vec4 environmentParams{1.0f, 0.0f, 0.0f, 0.0f};
  std::array<glm::vec4, 9> environmentSh{};
  glm::mat4 currentViewProj{1}, previousViewProj{1};
  glm::vec4 previousCamera{}, jitterUv{};
};
static_assert(sizeof(FrameUniformBufferObject) == 608);
static_assert(offsetof(FrameUniformBufferObject, currentViewProj) == 448);

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

Renderer::Renderer(Device const &device, unsigned framesInFlight)
    : framesInFlight_([&] {
        if (framesInFlight != 1 && framesInFlight != 2)
          throw std::invalid_argument("Frames in flight must be 1 or 2");
        return framesInFlight;
      }()),
      device_(device), materialDescriptorSetLayout_(
                           MaterialGpuStore::createDescriptorSetLayout(device)),
      textureCache_(device) {
  createPersistentResources();
}

void Renderer::recreateForSwapChain(SwapChain const &swapChain) {
  if (activeFrame_.has_value()) {
    throw std::runtime_error("Cannot recreate renderer swapchain resources "
                             "while a frame is in progress.");
  }

  collectCompletedWork();
  for (auto const &frame : frames_)
    if (frame.submitted)
      throw std::runtime_error(
          "Drain submitted frames before replacing swapchain resources");
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
  auto mirroredCull =
      rasterizerDebugSettings_.cullMode == vk::CullModeFlagBits::eBack
          ? vk::CullModeFlagBits::eFront
      : rasterizerDebugSettings_.cullMode == vk::CullModeFlagBits::eFront
          ? vk::CullModeFlagBits::eBack
          : rasterizerDebugSettings_.cullMode;
  auto newMirroredGraphicsPipeline =
      createGraphicsPipeline(swapChain, newPipelineLayout, mirroredCull);
  auto newMirroredTransparentPipeline =
      createTransparentPipeline(swapChain, newPipelineLayout, mirroredCull);
  auto newMirroredShadowPipeline =
      createShadowPipeline(newPipelineLayout, mirroredCull);

  std::vector<RenderGraph::State> newOutputStates(swapChain.images().size());
  std::vector<vk::Fence> newImagesInFlight(swapChain.images().size(),
                                           vk::Fence{});
  SwapChain const *newSwapChain = &swapChain;
  DepthResources newDepthResource = createDepthResources(swapChain);
  auto newMotionResource = createMotionResources(swapChain.extent());
  auto newHdrOutput = std::make_unique<HdrOutput>(device_, swapChain.extent(),
                                                  swapChain.imageFormat());

  auto newAo = std::make_unique<Gtao>(device_, swapChain.extent(),
                                      newHdrOutput->sceneView(),
                                      *newDepthResource.imageView);
  auto newTaa = std::make_unique<TaaResolve>(
      device_, swapChain.extent(), newHdrOutput->sceneView(),
      *newDepthResource.imageView, *newMotionResource.imageView,
      newAo->view(Gtao::Composite));
  std::unique_ptr<RayTracingRenderer> newRt;
  std::string rtReason = "RT Pipeline/BDA/indexing features not enabled or available";
  if (device_.rayTracingSupported()) {
    try {
      newRt = std::make_unique<RayTracingRenderer>(device_, swapChain.extent(), framesInFlight_);
      newHdrOutput->configureRayTracingView(newRt->view());
      rtReason.clear();
    } catch (std::exception const &error) {
      rtReason = error.what();
      std::cerr << "RT unavailable; retaining raster: " << rtReason << '\n';
    }
  }
  newHdrOutput->configureTemporalViews({newTaa->colorView(0),
                                        newTaa->colorView(1),
                                        newAo->view(Gtao::Composite)});
  using std::swap;
  swap(rayTracing_, newRt);
  rtUnavailableReason_ = std::move(rtReason);
  if (!rayTracing_) renderMethod_ = RenderMethod::Raster;
  swap(gtao_, newAo);
  lastAoActive_ = false;
  swap(taa_,newTaa);
  lastTaaActive_=false;
  swap(pipelineLayout_, newPipelineLayout);
  swap(graphicsPipeline_, newGraphicsPipeline);
  swap(doubleSidedGraphicsPipeline_, newDoubleSidedGraphicsPipeline);
  swap(transparentPipeline_, newTransparentPipeline);
  swap(doubleSidedTransparentPipeline_, newDoubleSidedTransparentPipeline);
  swap(debugLinePipeline_, newDebugLinePipeline);
  swap(environmentPipeline_, newEnvironmentPipeline);
  swap(shadowPipeline_, newShadowPipeline);
  swap(doubleSidedShadowPipeline_, newDoubleSidedShadowPipeline);
  swap(mirroredGraphicsPipeline_, newMirroredGraphicsPipeline);
  swap(mirroredTransparentPipeline_, newMirroredTransparentPipeline);
  swap(mirroredShadowPipeline_, newMirroredShadowPipeline);
  swap(imageStates_.output, newOutputStates);
  swap(imagesInFlight_, newImagesInFlight);
  swap(swapChain_, newSwapChain);
  swap(depthResources_, newDepthResource);
  swap(motionResources_, newMotionResource);
  temporalHistory_.reset();
  lastTemporalCamera_ = {};
  imageStates_.motion = {};
  swap(hdrOutput_, newHdrOutput);
  imageStates_.depth = {};
  imageStates_.hdr = {};
  activeGraph_.reset();
  currentFrame_ = 0;
  activeFrame_.reset();
  activePass_ = ActivePass::eNone;
  resourceStatistics_.pipelineBuilds += 16;
}

void Renderer::createPersistentResources() {
  createCommandPool();
  createFrameResources();
  UploadBatch debugUpload(device_);
  debugAabbLineResources_ =
      createGeometryResources(makeUnitAabbLineMesh(), debugUpload);
  debugUpload.finish();
  shadowResources_ = createShadowResources();
  HdrImage const environment =
      loadHdrImage("assets/environments/environment.hdr");
  auto bake = loadOrBakeEnvironment(environment, {}, ".cache/ibl");
  environmentSh_ = bake.data.sh;
  globalEnvironment_ = bake.data;
  TextureLoader environmentLoader(
      device_,
      device_.resourceLedger().scope(ResourceLedger::Domain::Persistent));
  UploadBatch environmentUpload(device_);
  environmentTexture_ = environmentLoader.createFromHdrPixels(
      environment.rgba, environment.width, environment.height,
      environmentUpload);
  environmentPrefilter_ = environmentLoader.createFromHdrCube(
      bake.data.cubeRgba, bake.data.levels.front().size, environmentUpload);
  environmentBrdfLut_ = environmentLoader.createFromHdrPixels(
      bake.data.brdfLut.rgba, bake.data.brdfLut.width, bake.data.brdfLut.height,
      environmentUpload, TextureWrap::ClampToEdge);
  auto uploadStats = environmentUpload.finish(); // Cold startup only.
  std::clog << "IBL cache="
            << (bake.cacheHit
                    ? "hit"
                    : (bake.cacheStored ? "baked/stored" : "baked/uncached"))
            << " cube=" << bake.data.levels.front().size
            << " mips=" << bake.data.levels.size()
            << " LUT=" << bake.data.brdfLut.width
            << " image copies=" << uploadStats.imageCopies
            << " submissions=" << uploadStats.submissions
            << " cold fence waits=" << uploadStats.fenceWaits << '\n';
  ++resourceStatistics_.environmentUploads;
  createFrameDescriptorSetLayout();
  createFrameDescriptorPool();
  allocateAndWriteFrameDescriptorSets();
  createCommandBuffers();
  createClusterPipeline();
  device_.nameObject(*shadowResources_.storage.image,
                     "Directional shadow depth");
  device_.nameObject(environmentTexture_.image(), "HDR environment");
  device_.nameObject(environmentPrefilter_.image(),
                     "GGX environment prefilter");
  device_.nameObject(environmentBrdfLut_.image(), "Environment BRDF A/B LUT");
  for (auto const &command : commandBuffers_)
    device_.nameObject(*command, "Frame command buffer");
}

void Renderer::createFrameResources() {
  auto properties = device_.physicalDevice().getProperties();
  auto const &limits = properties.limits;
  if (limits.maxPerStageDescriptorStorageBuffers < 4 ||
      limits.maxDescriptorSetStorageBuffers < 4 ||
      limits.maxPerStageResources < 22)
    throw std::runtime_error("Punctual lights require fragment storage buffer "
                             "and 22 stage resources");
  auto capacity = punctualCapacity(0, 0, limits.maxStorageBufferRange);
  timestampPeriod_ = properties.limits.timestampPeriod;
  timestampBits_ =
      device_.physicalDevice()
          .getQueueFamilyProperties()[device_.graphicsQueueFamilyIndex()]
          .timestampValidBits;
  frames_.clear();
  frames_.reserve(framesInFlight_);

  for (std::uint32_t index = 0; index < framesInFlight_; ++index) {
    FrameContext frame{};
    if (timestampBits_) {
      frame.aoTimestamps = vk::raii::QueryPool(
          device_.logicalDevice(),
          vk::QueryPoolCreateInfo{.queryType = vk::QueryType::eTimestamp,
                                  .queryCount = 6});
      frame.taaTimestamps=vk::raii::QueryPool(device_.logicalDevice(),vk::QueryPoolCreateInfo{.queryType=vk::QueryType::eTimestamp,.queryCount=2});
      frame.timestamps = vk::raii::QueryPool(
          device_.logicalDevice(),
          vk::QueryPoolCreateInfo{.queryType = vk::QueryType::eTimestamp,
                                  .queryCount = 12});
      device_.nameObject(*frame.timestamps, "Frame GPU timestamps");
    }
    frame.imageAvailableSemaphore =
        vk::raii::Semaphore(device_.logicalDevice(), vk::SemaphoreCreateInfo{});
    frame.inFlightFence = vk::raii::Fence(
        device_.logicalDevice(),
        vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});

    frame.uniform =
        device_.createBuffer(sizeof(FrameUniformBufferObject),
                             vk::BufferUsageFlagBits::eUniformBuffer,
                             vk::MemoryPropertyFlagBits::eHostVisible);
    frame.motionBuffer = device_.createBuffer(sizeof(MotionObjectGpu), vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible);
    MotionObjectGpu noHistory;
    frame.motionBuffer.write(std::as_bytes(std::span{&noHistory, 1}));
    frame.indoorBuffer = device_.createBuffer(sizeof(IndoorLightingGpu), vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible);
    frame.indoorBuffer.write(std::as_bytes(std::span{&frame.indoor, 1}));
    frame.clusterConfig = device_.createBuffer(sizeof(ClusterGrid), vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible);
    frame.clusterConfig.write(std::as_bytes(std::span{&frame.clusterGrid, 1}));
    frame.clusterIndices = device_.createBuffer(4, vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eDeviceLocal);
    frame.punctualCapacity = capacity;
    frame.punctualLights = device_.createBuffer(
        punctualHeaderBytes + capacity * sizeof(GpuPunctualLight),
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible);
    std::array<std::uint32_t, 4> empty{};
    frame.punctualLights.write(std::as_bytes(std::span{empty}));
    device_.nameObject(*frame.punctualLights.buffer, "Frame punctual lights");
    frames_.push_back(std::move(frame));
  }
}

Renderer::MeshGpuResources
Renderer::createGeometryResources(Mesh const &mesh, UploadBatch &uploads,
                                  ResourceLedger::Scope scope) {
  auto upload = [this, &uploads, &scope]<typename T>(
                    std::vector<T> const &data, vk::BufferUsageFlags usage) {
    auto bytes = std::as_bytes(std::span(data));
    if (device_.rayTracingSupported())
      usage |= vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
    auto resources = device_.createBuffer(
        bytes.size_bytes(), vk::BufferUsageFlagBits::eTransferDst | usage,
        vk::MemoryPropertyFlagBits::eDeviceLocal, scope);
    uploads.copyBuffer(bytes, *resources.buffer, usage);
    return resources;
  };
  MeshGpuResources resources;
  resources.vertex =
      upload(mesh.vertices, vk::BufferUsageFlagBits::eVertexBuffer);
  resources.index = upload(mesh.indices, vk::BufferUsageFlagBits::eIndexBuffer);
  resources.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
  resources.vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
  resources.unitVertexAlpha = std::ranges::all_of(
      mesh.vertices, [](Vertex const &v) { return v.alpha == 1.0f; });
  return resources;
}

Renderer::DepthResources
Renderer::createDepthResources(SwapChain const &swapChain) const {
  auto required = vk::FormatFeatureFlagBits::eDepthStencilAttachment |
                  vk::FormatFeatureFlagBits::eSampledImage |
                  vk::FormatFeatureFlagBits::eTransferSrc;
  if ((device_.physicalDevice()
           .getFormatProperties(kDepthFormat)
           .optimalTilingFeatures &
       required) != required)
    throw std::runtime_error(
        "D32 depth attachment/sampling/readback is unsupported");

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
      .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
               vk::ImageUsageFlagBits::eSampled |
               vk::ImageUsageFlagBits::eTransferSrc,
      .sharingMode = vk::SharingMode::eExclusive,
      .initialLayout = vk::ImageLayout::eUndefined,
  };

  DepthResources resources;
  resources.storage = device_.createImage(
      imageCreateInfo,
      std::uint64_t(swapChain.extent().width) * swapChain.extent().height * 4,
      vk::MemoryPropertyFlagBits::eDeviceLocal);

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
  resources.accounting = device_.resourceLedger()
                             .scope(ResourceLedger::Domain::Persistent)
                             .track({.imageViews = 1});
  return resources;
}

Renderer::ShadowResources Renderer::createShadowResources() const {
  vk::ImageCreateInfo imageCreateInfo{
      .imageType = vk::ImageType::e2D,
      .format = kDepthFormat,
      .extent =
          {
              .width = kShadowAtlasWidth,
              .height = kShadowMapSize,
              .depth = 1,
          },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = vk::SampleCountFlagBits::e1,
      .tiling = vk::ImageTiling::eOptimal,
      .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
               vk::ImageUsageFlagBits::eSampled |
               vk::ImageUsageFlagBits::eTransferSrc,
      .sharingMode = vk::SharingMode::eExclusive,
      .initialLayout = vk::ImageLayout::eUndefined,
  };

  ShadowResources resources;
  resources.storage = device_.createImage(
      imageCreateInfo, std::uint64_t(kShadowAtlasWidth) * kShadowMapSize * 4,
      vk::MemoryPropertyFlagBits::eDeviceLocal);

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
  resources.accounting = device_.resourceLedger()
                             .scope(ResourceLedger::Domain::Persistent)
                             .track({.imageViews = 1, .samplers = 2});
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
    if (mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max())
      throw std::runtime_error("Mesh vertex count exceeds the RT address limit.");
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
  candidate->resourceScope_ =
      device_.resourceLedger().scope(ResourceLedger::Domain::PreparedScene);
  candidate->meshes_.reserve(assets.meshes.size());
  bool opticalScene=std::ranges::any_of(assets.materials,[](auto const &m){return m.optical.enabled && m.optical.solid;});
  for (auto const &mesh : assets.meshes) {
    candidate->meshes_.push_back(createGeometryResources(mesh,uploads,candidate->resourceScope_));
    if(opticalScene)candidate->meshes_.back().dielectric=classifyDielectricMesh(mesh);
  }
  candidate->materials_ = std::make_unique<MaterialGpuStore>(
      device_, *materialDescriptorSetLayout_, assets.materials, textureCache_,
      uploads, candidate->resourceScope_);
  candidate->materialCount_ = assets.materials.size();
  candidate->rtTextureCount_ = rtTextures(nullptr,nullptr,nullptr,candidate->materials_.get(),candidate->materialCount_).size();
  if (device_.rayTracingSupported()) {
    std::vector<RtMeshInput> meshes;
    for (std::size_t i=0;i<candidate->meshes_.size();++i) {
      auto const &mesh=candidate->meshes_[i];
      meshes.push_back({*mesh.vertex.buffer,*mesh.index.buffer,mesh.vertexCount,mesh.indexCount,
          assets.meshes[i].vertices,assets.meshes[i].indices});
    }
    candidate->rtGeometry_ = std::make_unique<RayTracingGeometry>(device_, meshes, uploads, candidate->resourceScope_);
  }
  return candidate;
}

void Renderer::validateSceneCandidate(PreparedScene const &candidate) const {
  if (activeFrame_)
    throw std::runtime_error(
        "Cannot submit or commit a scene while recording a frame.");
  if (!candidate || candidate->owner_ != this || candidate == sceneAssets_)
    throw std::runtime_error(
        "Scene candidate is invalid or belongs to a different renderer.");
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
  if (candidate->rtGeometry_) candidate->rtGeometry_->uploadsCompleted();
  // Allocate the retirement entry before changing live state. The callback owns
  // a snapshot of old preview descriptors, not references to new UI containers.
  if (sceneAssets_ || retireSceneUi)
    retiredScenes_.push_back(RetiredScene{submittedFrameId_, sceneAssets_,
                                          std::move(retireSceneUi)});
  if (sceneAssets_)
    sceneAssets_->resourceScope_.setDomain(
        ResourceLedger::Domain::RetiredScene);
  candidate->resourceScope_.setDomain(ResourceLedger::Domain::LiveScene);
  sceneAssets_.swap(candidate);
  if (renderMethod_ == RenderMethod::RayTracing && !rayTracingAvailable())
    renderMethod_ = RenderMethod::Raster;
  probeValid_ = false;
  detailProbeValid_ = false;
  invalidateTemporalHistory();
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
  bool changed=materials().material(materialId).tint!=tint;
  materials().setMaterialTint(materialId, tint);
  if(changed && taa_)taa_->reset();
  probeMaterialsDirty_=true;
}

void Renderer::setMaterialSurfaceParams(MaterialId materialId,
                                        float normalScale,
                                        float parallaxScale) {
  auto before=materials().material(materialId).surfaceParams;
  materials().setMaterialSurfaceParams(materialId, normalScale, parallaxScale);
  if(before!=materials().material(materialId).surfaceParams && taa_)taa_->reset();
  probeMaterialsDirty_=true;
}

void Renderer::setMaterialAlphaParams(MaterialId materialId,
                                      AlphaMode alphaMode, float alphaCutoff) {
  auto before=materials().material(materialId).alphaParams;
  materials().setMaterialAlphaParams(materialId, alphaMode, alphaCutoff);
  if(before!=materials().material(materialId).alphaParams && taa_)taa_->reset();
  probeMaterialsDirty_=true;
}

void Renderer::setSurfaceDebugEnabled(bool normalMapsEnabled,
                                      bool parallaxEnabled) {
  if(normalMapsEnabled_!=normalMapsEnabled || parallaxEnabled_!=parallaxEnabled){probeMaterialsDirty_=true;if(taa_)taa_->reset();}
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
  if (activeFrame_)
    throw std::runtime_error("Cannot change UI callback during a frame");
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
      vk::DescriptorSetLayoutBinding{
          .binding = 4,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{
          .binding = 5,
          .descriptorType = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{
          .binding = 6,
          .descriptorType = vk::DescriptorType::eStorageBuffer,
          .descriptorCount = 1,
          .stageFlags = vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 7, .descriptorType = vk::DescriptorType::eStorageBuffer,
          .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 8, .descriptorType = vk::DescriptorType::eStorageBuffer,
          .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding=9, .descriptorType=vk::DescriptorType::eStorageBuffer,
          .descriptorCount=1, .stageFlags=vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding=10, .descriptorType=vk::DescriptorType::eStorageBuffer,
          .descriptorCount=1, .stageFlags=vk::ShaderStageFlagBits::eVertex},
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
          .descriptorCount = framesInFlight_,
      },
      vk::DescriptorPoolSize{
          .type = vk::DescriptorType::eCombinedImageSampler,
          .descriptorCount = framesInFlight_ * 5,
      },
      vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageBuffer,
                             .descriptorCount = framesInFlight_ * 5},
  };

  vk::DescriptorPoolCreateInfo createInfo{
      .maxSets = framesInFlight_,
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
  frameDescriptorAccounting_ =
      device_.resourceLedger()
          .scope(ResourceLedger::Domain::Persistent)
          .track(
              {.descriptorPools = 1, .descriptorSets = descriptorSets.size()});

  for (std::size_t index = 0; index < frames_.size(); ++index) {
    frames_[index].descriptorSet = descriptorSets[index];

    vk::DescriptorBufferInfo bufferInfo{
        .buffer = *frames_[index].uniform.buffer,
        .offset = 0,
        .range = sizeof(FrameUniformBufferObject),
    };

    vk::DescriptorBufferInfo motionInfo{.buffer=*frames_[index].motionBuffer.buffer, .range=frames_[index].motionCapacityBytes};
    vk::DescriptorBufferInfo indoorInfo{.buffer=*frames_[index].indoorBuffer.buffer, .range=sizeof(IndoorLightingGpu)};
    vk::DescriptorBufferInfo lightInfo{
        .buffer = *frames_[index].punctualLights.buffer,
        .offset = 0,
        .range = punctualHeaderBytes +
                 frames_[index].punctualCapacity * sizeof(GpuPunctualLight)};
    vk::DescriptorBufferInfo clusterConfigInfo{.buffer = *frames_[index].clusterConfig.buffer, .range = sizeof(ClusterGrid)};
    vk::DescriptorBufferInfo clusterIndicesInfo{.buffer = *frames_[index].clusterIndices.buffer, .range = frames_[index].clusterCapacityBytes};
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

    vk::DescriptorImageInfo prefilterImageInfo{
        .sampler = environmentPrefilter_.sampler(),
        .imageView = environmentPrefilter_.imageView(),
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    vk::DescriptorImageInfo brdfImageInfo{
        .sampler = environmentBrdfLut_.sampler(),
        .imageView = environmentBrdfLut_.imageView(),
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};

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
        vk::WriteDescriptorSet{.dstSet = frames_[index].descriptorSet,
                               .dstBinding = 4,
                               .descriptorCount = 1,
                               .descriptorType =
                                   vk::DescriptorType::eCombinedImageSampler,
                               .pImageInfo = &prefilterImageInfo},
        vk::WriteDescriptorSet{.dstSet = frames_[index].descriptorSet,
                               .dstBinding = 5,
                               .descriptorCount = 1,
                               .descriptorType =
                                   vk::DescriptorType::eCombinedImageSampler,
                               .pImageInfo = &brdfImageInfo},
        vk::WriteDescriptorSet{.dstSet = frames_[index].descriptorSet,
                               .dstBinding = 6,
                               .descriptorCount = 1,
                               .descriptorType =
                                   vk::DescriptorType::eStorageBuffer,
                               .pBufferInfo = &lightInfo},
        vk::WriteDescriptorSet{.dstSet = frames_[index].descriptorSet, .dstBinding = 7,
            .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &clusterConfigInfo},
        vk::WriteDescriptorSet{.dstSet = frames_[index].descriptorSet, .dstBinding = 8,
            .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &clusterIndicesInfo},
        vk::WriteDescriptorSet{.dstSet=frames_[index].descriptorSet,.dstBinding=10,.descriptorCount=1,
            .descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&motionInfo},
        vk::WriteDescriptorSet{.dstSet=frames_[index].descriptorSet, .dstBinding=9,
            .descriptorCount=1, .descriptorType=vk::DescriptorType::eStorageBuffer, .pBufferInfo=&indoorInfo},
    };

    device_.logicalDevice().updateDescriptorSets(writes, {});
  }
}

Renderer::FrameResult Renderer::beginFrame(glm::mat4 const &viewProjMatrix,
                                           glm::vec3 const &cameraPosition,
                                           LightingSettings const &lighting,
                                           bool shadowPassEnabled) {
  return beginFrameImpl(viewProjMatrix, cameraPosition, lighting, shadowPassEnabled, {});
}
Renderer::FrameResult Renderer::beginFrameImpl(glm::mat4 const &viewProjMatrix,
    glm::vec3 const &cameraPosition, LightingSettings const &lighting,
    bool shadowPassEnabled, std::span<MotionObject const> objects) {
  auto display = DisplaySettings{.exposureEv = lighting.exposureEv,
                                 .toneMap = lighting.toneMappingEnabled};
  validateDisplaySettings(display);
  bool dataDebug = (lighting.pbrDebugMode >= 1 && lighting.pbrDebugMode <= 5) ||
                   (lighting.pbrDebugMode >= 9 && lighting.pbrDebugMode <= 13) || lighting.pbrDebugMode == 15 || lighting.pbrDebugMode == 16 || lighting.pbrDebugMode == 17 || lighting.pbrDebugMode == 18 || lighting.pbrDebugMode == 19 ||
                   (shadowPassEnabled && (lighting.shadowDebugMode == 2 ||
                                          lighting.shadowDebugMode == 3));
  if (dataDebug) {
    display.exposureEv = 0;
    display.toneMap = false;
  }
  validateSwapChainState();

  if (activeFrame_.has_value()) {
    throw std::runtime_error(
        "Cannot begin a new frame while another frame is in progress.");
  }

  // Reject the complete invalid/oversized snapshot before acquiring an image or
  // resetting a fence. The retry therefore retains a usable frame slot.
  auto eye=viewProjMatrix*glm::vec4(cameraPosition,1);
  bool perspective=std::abs(eye.w)<1e-4f && glm::length(glm::vec3(viewProjMatrix[0][3],viewProjMatrix[1][3],viewProjMatrix[2][3]))>.5f;
  bool activeAo = renderMethod_ == RenderMethod::Raster && aoSettings_.enabled && perspective &&
                  lighting.pbrDebugMode == 0 && lighting.shadowDebugMode < 2;
  bool activeTaa =
      renderMethod_ == RenderMethod::Raster && taaEnabled_ && (!activeAo || aoSettings_.debug == AoDebug::None) &&
      perspective && lighting.pbrDebugMode == 0 && lighting.shadowDebugMode < 2;
  if(activeTaa!=lastTaaActive_){temporalHistory_.reset();taa_->reset();}
  lastTaaActive_=activeTaa;
  if (activeAo != lastAoActive_)
    taa_->reset();
  lastAoActive_ = activeAo;
  if (activeAo && aoSettings_.debug != AoDebug::None) {
    display.exposureEv = 0;
    display.toneMap = false;
  }
  auto temporal = temporalHistory_.prepare(viewProjMatrix, cameraPosition,
      swapChain_->extent().width, swapChain_->extent().height, activeTaa || (renderMethod_ == RenderMethod::Raster && temporalJitterEnabled_ && !taaEnabled_), objects);
  if (temporal.gpu.size() > device_.physicalDevice().getProperties().limits.maxStorageBufferRange / sizeof(MotionObjectGpu))
    throw std::runtime_error("Motion snapshot exceeds storage buffer range");
  if (renderMethod_ == RenderMethod::RayTracing) {
    rtLighting_ = lighting;
    rtShadows_ = shadowPassEnabled;
    shadowPassEnabled = false;
    rtPush_.inverseViewProjection = glm::inverse(viewProjMatrix);
    rtPush_.camera = glm::vec4(cameraPosition, 1);
    rtPush_.options.y = unsigned(rasterizerDebugSettings_.cullMode);
    rtPush_.options.z = rasterizerDebugSettings_.frontFace == vk::FrontFace::eClockwise;
  }
  auto lights = packPunctualLights(
      lighting.punctualLights,
      device_.physicalDevice().getProperties().limits.maxStorageBufferRange);
  auto indoor = makeIndoorLighting(lighting, lights, shadowPassEnabled);
  validateSunCascades(lighting.sunCascades);
  if (shadowPassEnabled && lighting.sunEnabled)
    indoor.sun = buildSunCascades(viewProjMatrix, cameraPosition, lighting.direction,
                                  lighting.sunCascades, lighting.shadowPcfRadius);
  auto const &limits = device_.physicalDevice().getProperties().limits;
  auto grid = makeClusterGrid(viewProjMatrix, cameraPosition, swapChain_->extent().width,
      swapChain_->extent().height, limits.maxStorageBufferRange, limits.maxComputeWorkGroupCount[0],
      renderMethod_ == RenderMethod::Raster && lighting.clusteredLights && clusterSupported_ && !lights.lights.empty());
  if (grid.screen.z) grid.inverseViewProj = glm::inverse(temporal.camera.rasterViewProj);
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
  frame.rayTracing = renderMethod_ == RenderMethod::RayTracing;
  frame.taaEnabled=activeTaa;
  frame.aoEnabled = activeAo;
  aoPush_ = {.inverseRaster = glm::inverse(temporal.camera.rasterViewProj),
             .camera = glm::vec4(cameraPosition, 1),
             .settings = {aoSettings_.radius, aoSettings_.strength,
                          float(aoSettings_.debug), 128},
             .screen = {float(swapChain_->extent().width),
                        float(swapChain_->extent().height), 0, 0}};
  frame.cameraPosition=cameraPosition;
  frame.shadowReceiverCullingAllowed=lighting.shadowDebugMode<2;
  taaPush_={.inverseRaster=glm::inverse(temporal.camera.rasterViewProj),
    .depthRow={viewProjMatrix[0][3],viewProjMatrix[1][3],viewProjMatrix[2][3],viewProjMatrix[3][3]},
    .jitterWeight={temporal.camera.jitterUv.x,temporal.camera.jitterUv.y,.1f,float(taaHistoryFilter_)},
    .options={temporal.camera.previousCamera.w,.002f,8,1.5f}};
  updateFrameMotion(frame, std::move(temporal));
  updateFrameLights(frame, lights);
  frame.indoor = indoor;
  lastIndoor_ = indoor;
  activeSunShadow_ = lighting.sunEnabled;
  frame.indoorBuffer.write(std::as_bytes(std::span{&frame.indoor, 1}));
  updateFrameClusters(frame, grid);
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

  if (imageIndex >= swapChain_->images().size()) {
    throw std::runtime_error("Acquire swapchain image index is out of range");
  }

  if (imagesInFlight_[imageIndex]) {
    (void)device_.logicalDevice().waitForFences(
        {imagesInFlight_[imageIndex]}, true,
        std::numeric_limits<std::uint64_t>::max());
  }

  commandBuffer.reset();
  auto effectiveLighting = lighting;
  if (!shadowPassEnabled)
    effectiveLighting.shadowDebugMode = 0;
  updateFrameUniformBuffer(frame, frame.temporal->camera.rasterViewProj, cameraPosition,
                           effectiveLighting);

  commandBuffer.begin(vk::CommandBufferBeginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
  });

  activeGraph_ = buildFrameGraph(imageIndex, shadowPassEnabled);
  lastGraphDump_ = activeGraph_->plan.dump();
  queuedObjects_.clear();
  queuedCasters_.clear();
  queuedBoxes_.clear();
  queuedSky_ = false;
  requestedShadows_ = shadowPassEnabled;
  drawStatistics_ = {};
  frame.shadowEnabled = activeGraph_->shadowPass.has_value();
  frame.uiEnabled = activeGraph_->uiPass.has_value();
  if (*frame.timestamps)
    commandBuffer.resetQueryPool(*frame.timestamps, 0, 12);
  if (activeAo && *frame.aoTimestamps)
    commandBuffer.resetQueryPool(*frame.aoTimestamps, 0, 6);
  if(activeTaa && *frame.taaTimestamps) commandBuffer.resetQueryPool(*frame.taaTimestamps,0,2);
  activeFrame_ = ActiveFrameState{frameIndex, imageIndex, acquireResult};
  activePass_ = ActivePass::eNone;

  return FrameResult::eSuccess;
}

void Renderer::recordObject(MeshId meshId, MaterialId materialId,
                            glm::mat4 const &modelMatrix, std::uint32_t motionIndex) {
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
  bool mirrored = glm::determinant(glm::mat3(modelMatrix)) < 0;
  PushConstants pushConstants{
      .transform = modelMatrix,
      .materialTint = materialResource.tint,
      .surfaceParams = materialResource.surfaceParams,
      .alphaParams = materialResource.alphaParams,
      .shadowPass = {activeShadowIndex_, int(motionIndex), 0, 0},
  };
  if (materialResource.optical.enabled) {
    bool solid=materialResource.optical.solid && meshResources.dielectric.solid;
    float local=solid ? (materialResource.optical.thickness>0 ? materialResource.optical.thickness : meshResources.dielectric.boxThickness) : materialResource.optical.thickness;
    float scale=std::max({glm::length(glm::vec3(modelMatrix[0])),glm::length(glm::vec3(modelMatrix[1])),glm::length(glm::vec3(modelMatrix[2]))});
    pushConstants.shadowPass.z=std::bit_cast<std::int32_t>(local*scale);
    pushConstants.shadowPass.w=int(solid);
  }
  if (!normalMapsEnabled_) {
    pushConstants.surfaceParams.z = 0.0f;
  }
  if (!parallaxEnabled_) {
    pushConstants.surfaceParams.w = 0.0f;
  }
  pushConstants.alphaParams.w =
      meshResources.unitVertexAlpha &&
              !(pushConstants.surfaceParams.w > 0.5f &&
                pushConstants.surfaceParams.y > 0.0001f)
          ? 1.0f
          : 0.0f;

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
        : mirrored ? mirroredShadowPipeline_
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

  // Probe refraction already includes the background. Full optical coverage
  // needs the nearest surface, independent of triangle order. Capture still
  // uses alpha blending for its direct-only opacity approximation.
  bool opticalDepth = materialResource.optical.enabled &&
                      materialResource.optical.coverage != 2 &&
                      (frames_[frameState.frameIndex].indoor.counts.w & 2u) == 0;
  MaterialPipelineVariant const variant =
      selectMaterialPipeline(opticalDepth ? AlphaMode::Opaque : materialResource.alphaMode,
                             materialResource.doubleSided, RasterPass::Main);
  vk::raii::Pipeline const *pipeline = nullptr;
  switch (variant) {
  case MaterialPipelineVariant::OpaqueSingleSided:
    pipeline = mirrored ? &mirroredGraphicsPipeline_ : &graphicsPipeline_;
    break;
  case MaterialPipelineVariant::OpaqueDoubleSided:
    pipeline = &doubleSidedGraphicsPipeline_;
    break;
  case MaterialPipelineVariant::TransparentSingleSided:
    pipeline = mirrored ? &mirroredTransparentPipeline_ : &transparentPipeline_;
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

void Renderer::recordEnvironment() {
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

void Renderer::recordAabb(Aabb const &bounds, glm::vec4 const &color) {
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

Renderer::FrameResult Renderer::finishFrame(SceneDrawList const &scene) {
  if (!activeFrame_.has_value()) {
    throw std::runtime_error(
        "Cannot end a frame when no frame is in progress.");
  }

  ActiveFrameState const frameState = *activeFrame_;
  auto &frame = frames_[frameState.frameIndex];
  auto &commandBuffer = commandBuffers_[frameState.frameIndex];

  if (renderMethod_ == RenderMethod::RayTracing) prepareRayTracing(scene);
  recordGraph(scene);

  vk::Semaphore waitSemaphore = *frame.imageAvailableSemaphore;
  vk::PipelineStageFlags waitStage =
      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  vk::CommandBuffer rawCommandBuffer = *commandBuffer;
  vk::Semaphore signalSemaphore =
      swapChain_->renderFinishedSemaphore(frameState.imageIndex);

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
  device_.logicalDevice().resetFences({*frame.inFlightFence});
  device_.graphicsQueue().submit({submitInfo}, *frame.inFlightFence);
  auto const &graph = *activeGraph_;
  if (graph.clusterPass) frame.clusterState = graph.plan.finalBufferState(graph.clusterIndices);
  if (graph.rayTracing) {
    rayTracing_->submitted(graph.plan, graph.hdr);
  } else {
  imageStates_.shadow = graph.plan.finalState(graph.shadow);
  imageStates_.depth = graph.plan.finalState(graph.depth);
  imageStates_.hdr = graph.plan.finalState(graph.hdr);
  imageStates_.motion = graph.plan.finalState(graph.motion);
  gtao_->submitted(graph.plan, graph.diffuse, graph.ao ? &*graph.ao : nullptr);
  if(graph.taa)taa_->submitted(graph.plan,*graph.taa);
  }
  imageStates_.output[frameState.imageIndex] =
      graph.plan.finalState(graph.output);
  onFrameSubmitted(frame);
  imagesInFlight_[frameState.imageIndex] = *frame.inFlightFence;
  cpuSyncTimes_.submitMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - submitStart)
                               .count();

  auto advanceFrame = [this]() {
    currentFrame_ = (currentFrame_ + 1) % framesInFlight_;
  };
  auto presentStart = std::chrono::steady_clock::now();
  auto presentResult = swapChain_->present(frameState.imageIndex);
  activeFrame_.reset();
  activeGraph_.reset();
  activePass_ = ActivePass::eNone;
  if (presentResult == vk::Result::eErrorOutOfDateKHR) {
    advanceFrame();
    return FrameResult::eSwapChainOutOfDate;
  }

  cpuSyncTimes_.presentMs = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - presentStart)
                                .count();
  if (presentResult != vk::Result::eSuccess &&
      presentResult != vk::Result::eSuboptimalKHR) {
    throw std::runtime_error("Failed to present swapchain image: " +
                             vk::to_string(presentResult));
  }

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

  if (frames_.size() != framesInFlight_) {
    throw std::runtime_error("Renderer frame resource count is invalid.");
  }

  if (commandBuffers_.size() != framesInFlight_) {
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

  if (graphicsPipeline_ == nullptr || doubleSidedGraphicsPipeline_ == nullptr ||
      mirroredGraphicsPipeline_ == nullptr) {
    throw std::runtime_error("Renderer graphics pipeline is not initialized.");
  }

  if (transparentPipeline_ == nullptr ||
      doubleSidedTransparentPipeline_ == nullptr || mirroredTransparentPipeline_ == nullptr) {
    throw std::runtime_error(
        "Renderer transparent pipeline is not initialized.");
  }

  if (shadowPipeline_ == nullptr || doubleSidedShadowPipeline_ == nullptr ||
      mirroredShadowPipeline_ == nullptr) {
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

  if (imagesInFlight_.size() != imageCount ||
      imageStates_.output.size() != imageCount) {
    throw std::runtime_error(
        "Renderer swapchain-dependent resource counts are inconsistent.");
  }

  for (auto const &frame : frames_) {
    if (frame.uniform.buffer == nullptr || !frame.uniform.valid() ||
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
          !meshResources.index.valid() || meshResources.indexCount == 0) {
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
  std::array colorBlendAttachments{colorBlendAttachment, colorBlendAttachment,
                                   colorBlendAttachment};
  vk::PipelineColorBlendStateCreateInfo colorBlending{
      .attachmentCount = 3,
      .pAttachments = colorBlendAttachments.data(),
  };
  std::array dynamicStates = {vk::DynamicState::eViewport,
                              vk::DynamicState::eScissor};
  vk::PipelineDynamicStateCreateInfo dynamicState{
      .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
      .pDynamicStates = dynamicStates.data(),
  };
  std::array colorAttachmentFormats{HdrOutput::sceneFormat, kMotionFormat,
                                    HdrOutput::sceneFormat};
  vk::PipelineRenderingCreateInfo renderingInfo{
      .colorAttachmentCount = 3,
      .pColorAttachmentFormats = colorAttachmentFormats.data(),
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
  std::array colorBlendAttachments{colorBlendAttachment, colorBlendAttachment,
                                   colorBlendAttachment};
  vk::PipelineColorBlendStateCreateInfo colorBlending{
      .logicOpEnable = false,
      .attachmentCount = 3,
      .pAttachments = colorBlendAttachments.data(),
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

  std::array colorAttachmentFormats{HdrOutput::sceneFormat, kMotionFormat,
                                    HdrOutput::sceneFormat};
  vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
      .colorAttachmentCount = 3,
      .pColorAttachmentFormats = colorAttachmentFormats.data(),
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
  std::array colorBlendAttachments{colorBlendAttachment, colorBlendAttachment,
                                   colorBlendAttachment};
  vk::PipelineColorBlendStateCreateInfo colorBlending{
      .logicOpEnable = false,
      .attachmentCount = 3,
      .pAttachments = colorBlendAttachments.data(),
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

  std::array colorAttachmentFormats{HdrOutput::sceneFormat, kMotionFormat,
                                    HdrOutput::sceneFormat};
  vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
      .colorAttachmentCount = 3,
      .pColorAttachmentFormats = colorAttachmentFormats.data(),
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
  std::array colorBlendAttachments{colorBlendAttachment, colorBlendAttachment,
                                   colorBlendAttachment};
  vk::PipelineColorBlendStateCreateInfo colorBlending{
      .logicOpEnable = false,
      .attachmentCount = 3,
      .pAttachments = colorBlendAttachments.data(),
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

  std::array colorAttachmentFormats{HdrOutput::sceneFormat, kMotionFormat,
                                    HdrOutput::sceneFormat};
  vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
      .colorAttachmentCount = 3,
      .pColorAttachmentFormats = colorAttachmentFormats.data(),
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
      vk::DynamicState::eDepthBias,
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
      .commandBufferCount = framesInFlight_,
  };
  commandBuffers_ =
      vk::raii::CommandBuffers(device_.logicalDevice(), allocateInfo);
}

void Renderer::updateFrameLights(FrameContext &frame,
                                 PackedPunctualLights const &lights) {
  auto capacity = punctualCapacity(
      frame.punctualCapacity, lights.lights.size(),
      device_.physicalDevice().getProperties().limits.maxStorageBufferRange);
  auto bytes = punctualHeaderBytes + capacity * sizeof(GpuPunctualLight);
  if (capacity != frame.punctualCapacity) {
    auto replacement =
        device_.createBuffer(bytes, vk::BufferUsageFlagBits::eStorageBuffer,
                             vk::MemoryPropertyFlagBits::eHostVisible);
    replacement.write(std::as_bytes(std::span{lights.counts}));
    if (!lights.lights.empty())
      replacement.write(std::as_bytes(std::span{lights.lights}),
                        punctualHeaderBytes);
    vk::DescriptorBufferInfo info{
        .buffer = *replacement.buffer, .offset = 0, .range = bytes};
    device_.logicalDevice().updateDescriptorSets(
        {vk::WriteDescriptorSet{.dstSet = frame.descriptorSet,
                                .dstBinding = 6,
                                .descriptorCount = 1,
                                .descriptorType =
                                    vk::DescriptorType::eStorageBuffer,
                                .pBufferInfo = &info}},
        {});
    frame.punctualLights = std::move(replacement);
    frame.punctualCapacity = capacity;
    device_.nameObject(*frame.punctualLights.buffer, "Frame punctual lights");
  } else {
    frame.punctualLights.write(std::as_bytes(std::span{lights.counts}));
    if (!lights.lights.empty())
      frame.punctualLights.write(std::as_bytes(std::span{lights.lights}),
                                 punctualHeaderBytes);
  }
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
  glm::vec3 const lightColor =
      lighting.color * (lighting.sunEnabled ? lighting.intensity : 0.0f);
  ubo.lightColor = glm::vec4(lightColor, 1.0f);
  ubo.ambientColor = glm::vec4(lightColor * lighting.ambientStrength, 1.0f);
  ubo.lightingParams =
      glm::vec4(lighting.diffuseStrength, lighting.specularStrength,
                lighting.specularAaEnabled ? 1.0f : 0.0f,
                static_cast<float>(lighting.pbrDebugMode));
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

  ubo.currentViewProj = viewProjMatrix;
  ubo.previousViewProj = viewProjMatrix;
  if (frame.temporal) {
    auto const &t = frame.temporal->camera;
    ubo.currentViewProj = t.currentViewProj;
    ubo.previousViewProj = t.previousViewProj;
    ubo.previousCamera = t.previousCamera;
    ubo.jitterUv = t.jitterUv;
  }
  frame.uniform.write(std::as_bytes(std::span{&ubo, 1}));
}

void Renderer::timestamp(vk::raii::CommandBuffer const &command,
                         std::uint32_t query) {
  if (!timestampBits_ || !activeFrame_)
    return;
  command.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe,
                          *frames_[activeFrame_->frameIndex].timestamps, query);
}
void Renderer::setGpuTimingCallback(
    std::function<void(GpuTimings const &)> callback) {
  if (activeFrame_)
    throw std::runtime_error(
        "Cannot change timing callback during frame recording");
  gpuTimingCallback_ = std::move(callback);
}
void Renderer::onFrameSubmitted(FrameContext &frame) {
  if (frame.temporal) {
    temporalHistory_.commit(std::move(*frame.temporal));
    frame.temporal.reset();
  }
  frame.frameId = ++submittedFrameId_;
  frame.submitted = true;
}
void Renderer::collectFrameTimings(FrameContext &frame) {
  if (!frame.submitted)
    return;
  GpuTimings result{.frameId = frame.frameId, .clustered = frame.clusterEnabled, .rayTracing = frame.rayTracing};
  if (*frame.timestamps) {
    std::array<std::uint64_t, 24> values{};
    unsigned count = frame.clusterEnabled ? 12 : 10;
    auto status = vkGetQueryPoolResults(
        static_cast<VkDevice>(device_.deviceHandle()),
        static_cast<VkQueryPool>(*frame.timestamps), 0, count, sizeof(values),
        values.data(), 2 * sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (status != VK_SUCCESS && status != VK_NOT_READY)
      throw std::runtime_error("GPU timestamp readback failed");
    bool ready = true;
    for (unsigned i = 0; i < count; ++i)
      ready = ready && values[2 * i + 1] != 0;
    if (!ready)
      throw std::runtime_error(
          "Completed frame has unavailable timestamps; refusing slot reuse");
    auto elapsed = [&](unsigned a, unsigned b) {
      return timestampMilliseconds(values[2 * a], values[2 * b], timestampBits_,
                                   timestampPeriod_);
    };
    result.valid = true;
    result.totalMs = elapsed(0, 9);
    result.shadowMs = frame.shadowEnabled ? elapsed(1, 2) : 0;
    result.mainMs = elapsed(3, 4);
    result.cullingMs = frame.clusterEnabled ? elapsed(10, 11) : 0;
    result.outputMs = elapsed(5, 6);
    result.uiMs = frame.uiEnabled ? elapsed(7, 8) : 0;
    if (frame.aoEnabled) {
      std::array<std::uint64_t, 12> v{};
      auto status = vkGetQueryPoolResults(
          static_cast<VkDevice>(device_.deviceHandle()),
          static_cast<VkQueryPool>(*frame.aoTimestamps), 0, 6, sizeof(v),
          v.data(), 16,
          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      if (status != VK_SUCCESS)
        throw std::runtime_error("Completed AO query unavailable");
      for (unsigned i = 0; i < 6; ++i)
        if (!v[i * 2 + 1])
          throw std::runtime_error("Completed AO query unavailable");
      result.aoMs =
          timestampMilliseconds(v[0], v[10], timestampBits_, timestampPeriod_);
      result.aoHorizonMs =
          timestampMilliseconds(v[0], v[2], timestampBits_, timestampPeriod_);
      result.aoFilterMs =
          timestampMilliseconds(v[4], v[6], timestampBits_, timestampPeriod_);
      result.aoCompositeMs =
          timestampMilliseconds(v[8], v[10], timestampBits_, timestampPeriod_);
    }
    if(frame.taaEnabled){std::array<std::uint64_t,4> v{};
      auto status=vkGetQueryPoolResults(static_cast<VkDevice>(device_.deviceHandle()),static_cast<VkQueryPool>(*frame.taaTimestamps),0,2,sizeof(v),v.data(),16,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      if(status!=VK_SUCCESS||!v[1]||!v[3])throw std::runtime_error("Completed TAA query unavailable");
      result.taaMs=timestampMilliseconds(v[0],v[2],timestampBits_,timestampPeriod_);
    }
  }
  resourceStatistics_.completedFrameId =
      std::max(resourceStatistics_.completedFrameId, frame.frameId);
  // A fence handle belongs to a slot, not permanently to an acquired image.
  for (auto &fence : imagesInFlight_)
    if (fence == *frame.inFlightFence)
      fence = nullptr;
  if (result.frameId > gpuTimings_.frameId)
    gpuTimings_ = result;
  frame.submitted = false;
  if (gpuTimingCallback_)
    gpuTimingCallback_(result);
}
void Renderer::collectCompletedWork() {
  if (swapChain_)
    swapChain_->collectPresentationCompletions();
  for (auto &frame : frames_) {
    if (frame.submitted &&
        frame.inFlightFence.getStatus() == vk::Result::eSuccess)
      collectFrameTimings(frame);
  }
  std::erase_if(pendingSceneUploads_, [](auto const &candidate) {
    if (!candidate->uploads_ || candidate->uploads_->ready()) {
      if (candidate->rtGeometry_) candidate->rtGeometry_->uploadsCompleted();
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
