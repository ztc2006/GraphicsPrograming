#include "renderer.hpp"
#include "frustum.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/packing.hpp>
#include <numbers>
#include <stdexcept>

void Renderer::setShadowCasterCullingEnabled(bool enabled) {
  if (activeFrame_) throw std::runtime_error("Cannot change culling during a frame");
  shadowCasterCullingEnabled_ = enabled;
}

IndoorLightingGpu Renderer::makeIndoorLighting(LightingSettings const &lighting,
                                               PackedPunctualLights &lights,
                                               bool shadows) const {
  IndoorLightingGpu g;
  assignSpotShadows(lighting.punctualLights, lights, g, shadows);
  g.counts.z = lighting.localShadowDebugIndex;
  if (lighting.localProbe.enabled && probeValid_) {
    g.counts.w = 1;
    g.probeMin = glm::vec4(capturedProbe_.minimum, 0);
    g.probeMax = glm::vec4(capturedProbe_.maximum, 0);
    g.probePosition = glm::vec4(capturedProbe_.position, 0);
    for (unsigned i = 0; i < 9; ++i)
      g.probeSh[i] = glm::vec4(probeSh_[i], 0);
  }
  if (lighting.localProbe.enabled && lighting.detailReflectionProbe.enabled &&
      probeValid_ && detailProbeValid_) {
    g.counts.w |= 4u;
    g.detailMin = glm::vec4(capturedDetailProbe_.minimum, 0);
    g.detailMax = glm::vec4(capturedDetailProbe_.maximum, 0);
    g.detailPosition = glm::vec4(capturedDetailProbe_.position, 0);
  }
  return g;
}
void Renderer::recordShadowTiles(SceneDrawList const &scene) {
  auto &command = commandBuffers_[activeFrame_->frameIndex];
  auto const &g = frames_[activeFrame_->frameIndex].indoor;
  auto tile = [&](int index, vk::Rect2D rectangle) {
    auto const &frame=frames_[activeFrame_->frameIndex];
    if(shadowCasterCullingEnabled_ && index<=-2 && frame.shadowReceiverCullingAllowed &&
        (g.counts.w & 2u)==0u) {
      unsigned cascade=unsigned(-2-index);
      float lower=cascade ? g.sun.splits[cascade-1] : g.sun.forwardNear.w;
      if(cascade) {
        float previousStart=cascade>1 ? g.sun.splits[cascade-2] : g.sun.forwardNear.w;
        lower-=(g.sun.splits[cascade-1]-previousStart)*g.sun.params.y;
      }
      bool receiver=false;
      for(auto list : {scene.opaque,scene.mask,scene.transparent})
        for(auto const &item:list)
          receiver |=
              item.primaryVisible &&
              intersectsDepthRange(item.worldBounds, frame.cameraPosition,
                                   glm::vec3{g.sun.forwardNear}, lower,
                                   g.sun.splits[cascade]);
      if(!receiver) {
        auto count=std::uint32_t(scene.allOpaque.size()+scene.allMask.size());
        drawStatistics_.shadowCandidates+=count;drawStatistics_.shadowCulled+=count;
        return; // The graph has already cleared the complete atlas to far depth.
      }
    }
    activeShadowIndex_ = index;
    // CSM uses world-space receiver bias/plane correction. Preserve the old
    // raster bias for the legacy sun map and perspective spot tiles.
    command.setDepthBias(index <= -2 ? 0.f : 1.25f, 0.f,
                         index <= -2 ? 0.f : 1.75f);
    command.setViewport(
        0, {vk::Viewport{float(rectangle.offset.x), float(rectangle.offset.y),
                         float(rectangle.extent.width),
                         float(rectangle.extent.height), 0, 1}});
    command.setScissor(0, {rectangle});
    std::optional<Frustum> frustum;
    if (shadowCasterCullingEnabled_ && index != -1) {
      auto const &vp = index >= 0 ? g.spotViewProj[index] : g.sun.viewProj[-2-index];
      frustum = extractFrustum(vp, {2.f / rectangle.extent.width,
                                    2.f / rectangle.extent.height});
    }
    for (auto list : {scene.allOpaque, scene.allMask})
      for (auto const &item : list) {
        if (!item.shadowCaster)
          continue;
        ++drawStatistics_.shadowCandidates;
        if (frustum && !intersectsFrustum(*frustum, item.worldBounds)) {
          ++drawStatistics_.shadowCulled;
          continue;
        }
        recordObject(item.meshId, item.materialId, item.modelMatrix);
        ++drawStatistics_.shadow;
      }
  };
  if (activeSunShadow_) {
    unsigned count = unsigned(g.sun.params.x);
    if (!count) tile(-1, {{0, 0}, {kShadowMapSize, kShadowMapSize}});
    for (unsigned i = 0; i < count; ++i)
      tile(-2 - int(i), {{int(1024 * (i % 2)), int(1024 * (i / 2))}, {1024, 1024}});
  }
  for (unsigned i = 0; i < g.counts.x; ++i)
    tile(int(i),
         {{int(2048 + 1024 * (i % 2)), int(1024 * (i / 2))}, {1024, 1024}});
  activeShadowIndex_ = -1;
}
std::vector<std::byte>
Renderer::probeLightingKey(LightingSettings const &l) const {
  std::vector<std::byte> key;
  auto add = [&]<typename T>(T const &v) {
    auto b = std::as_bytes(std::span{&v, 1});
    key.insert(key.end(), b.begin(), b.end());
  };
  for (auto const &light : l.punctualLights) {
    add(light.type);
    add(light.enabled);
    add(light.position);
    add(light.direction);
    add(light.color);
    add(light.intensity);
    add(light.range);
    add(light.innerCone);
    add(light.outerCone);
    add(light.castsShadow);
    add(light.shadowNear);
    add(light.shadowDistance);
    add(light.shadowBiasSlope);
    add(light.shadowBiasConstant);
    add(light.shadowPcfRadius);
  }
  add(l.sunEnabled);
  add(l.direction);
  add(l.color);
  add(l.intensity);
  add(l.diffuseStrength);
  add(l.specularStrength);
  add(l.shadowTarget);
  add(l.shadowOrthoExtent);
  add(l.shadowNearPlane);
  add(l.shadowFarPlane);
  add(l.shadowLightDistance);
  add(l.shadowBiasSlope);
  add(l.shadowBiasConstant);
  add(l.shadowPcfRadius);
  add(l.environmentIntensity);
  add(l.environmentRotation);
  add(l.localProbe.minimum);
  add(l.localProbe.maximum);
  add(l.localProbe.position);
  add(l.detailReflectionProbe.enabled);
  add(l.detailReflectionProbe.minimum);
  add(l.detailReflectionProbe.maximum);
  add(l.detailReflectionProbe.position);
  return key;
}
bool Renderer::localProbeMatches(LightingSettings const &l) const {
  return probeValid_ && !probeGeometryDirty_ && !probeMaterialsDirty_ &&
         capturedLightingKey_ == probeLightingKey(l);
}

std::vector<std::byte>
Renderer::probeGeometryKey(SceneDrawList const &scene) const {
  std::vector<std::byte> result;
  auto transparent =
      scene.allTransparent.empty() ? scene.transparent : scene.allTransparent;
  for (auto list : {scene.allOpaque, scene.allMask, transparent}) {
    auto count = list.size();
    auto b = std::as_bytes(std::span{&count, 1});
    result.insert(result.end(), b.begin(), b.end());
    for (auto const &item : list) {
      auto add = [&]<typename T>(T const &v) {
        auto bytes = std::as_bytes(std::span{&v, 1});
        result.insert(result.end(), bytes.begin(), bytes.end());
      };
      add(item.meshId);
      add(item.materialId);
      add(item.modelMatrix);
      add(item.shadowCaster);
    }
  }
  return result;
}

void Renderer::captureLocalProbe(SceneDrawList const &scene,
                                 LightingSettings const &lighting) {
  if (activeFrame_ || !sceneAssets_ || !swapChain_)
    throw std::runtime_error(
        "Probe capture requires an idle renderer with a committed scene");
  validateLocalProbe(lighting.localProbe);
  if (lighting.detailReflectionProbe.enabled)
    validateLocalProbe(lighting.detailReflectionProbe);
  for (auto list : {scene.allOpaque, scene.allMask,
                    scene.allTransparent.empty() ? scene.transparent
                                                 : scene.allTransparent})
    for (auto const &item : list)
      validateDrawItem(item, false);
  auto lights = packPunctualLights(
      lighting.punctualLights,
      device_.physicalDevice().getProperties().limits.maxStorageBufferRange);
  auto config = makeIndoorLighting(lighting, lights, true);
  config.counts.w =
      2; // Capture geometry has no indirect/environment illumination.
  auto key = probeLightingKey(lighting);
  auto geometryKey = probeGeometryKey(scene);
  for (auto &frame : frames_) {
    if (device_.logicalDevice().waitForFences(
            {*frame.inFlightFence}, true, UINT64_MAX) != vk::Result::eSuccess)
      throw std::runtime_error("Probe could not drain frame users");
    collectFrameTimings(frame);
  }
  collectCompletedWork();
  auto &slot = frames_[0];
  updateFrameLights(slot, lights);
  ClusterGrid disabled{};
  updateFrameClusters(slot, disabled);
  slot.indoor = config;
  slot.indoorBuffer.write(std::as_bytes(std::span{&slot.indoor, 1}));
  activeSunShadow_ = lighting.sunEnabled;
  unsigned const size = globalEnvironment_.levels.front().size;
  std::uint64_t const faceBytes = std::uint64_t(size) * size * 8;
  auto scope = device_.resourceLedger().scope(ResourceLedger::Domain::Staging);
  auto target = [&](vk::Format format, vk::ImageAspectFlags aspect,
                    vk::ImageUsageFlags usage, unsigned bytes) {
    DepthResources r;
    r.storage = device_.createImage(
        vk::ImageCreateInfo{.imageType = vk::ImageType::e2D,
                            .format = format,
                            .extent = {size, size, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = vk::SampleCountFlagBits::e1,
                            .tiling = vk::ImageTiling::eOptimal,
                            .usage = usage,
                            .sharingMode = vk::SharingMode::eExclusive},
        std::uint64_t(size) * size * bytes,
        vk::MemoryPropertyFlagBits::eDeviceLocal, scope);
    r.imageView = vk::raii::ImageView(
        device_.logicalDevice(),
        vk::ImageViewCreateInfo{.image = *r.storage.image,
                                .viewType = vk::ImageViewType::e2D,
                                .format = format,
                                .subresourceRange = {aspect, 0, 1, 0, 1}});
    r.accounting = scope.track({.imageViews = 1});
    return r;
  };
  auto colorUsage = vk::ImageUsageFlagBits::eColorAttachment |
                    vk::ImageUsageFlagBits::eTransferSrc;
  auto depthUsage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
  auto diffuse = target(HdrOutput::sceneFormat, vk::ImageAspectFlagBits::eColor,
                        colorUsage, 8);
  auto color = target(HdrOutput::sceneFormat, vk::ImageAspectFlagBits::eColor,
                      colorUsage, 8);
  auto motion = target(kMotionFormat, vk::ImageAspectFlagBits::eColor, colorUsage, 8);
  slot.temporal.reset();
  auto depth =
      target(kDepthFormat, vk::ImageAspectFlagBits::eDepth, depthUsage, 4);
  auto readback =
      device_.createBuffer(faceBytes * 6, vk::BufferUsageFlagBits::eTransferDst,
                           vk::MemoryPropertyFlagBits::eHostVisible, scope);
  vk::raii::Fence fence(device_.logicalDevice(), vk::FenceCreateInfo{});
  auto &command = commandBuffers_[0];
  RenderGraph::State colorState, depthState, motionState, diffuseState;
  auto savedDraws = drawStatistics_;
  try {
    std::vector<LocalProbeSettings> regions{lighting.localProbe};
    if (lighting.detailReflectionProbe.enabled)
      regions.push_back(lighting.detailReflectionProbe);
    std::vector<BakedEnvironment> bakes;
    bakes.reserve(regions.size());
    for (auto const &region : regions) {
      for (unsigned face = 0; face < 6; ++face) {
        auto direction = environmentCubeDirection(face, 0, 0);
        auto up = glm::normalize(
            -environmentCubeDirection(face, 0, 1) +
            direction *
                glm::dot(direction, environmentCubeDirection(face, 0, 1)));
        auto p = region.position;
        auto projection =
            glm::perspective(glm::half_pi<float>(), 1.f, .02f,
                             std::max(glm::length(lighting.localProbe.maximum -
                                                  lighting.localProbe.minimum) *
                                          2,
                                      1.f));
        projection[1][1] *= -1;
        auto captureLighting = lighting;
        captureLighting.environmentIntensity =
            1; // Apply user IBL gain once at runtime.
        captureLighting.pbrDebugMode = 0;
        captureLighting.shadowDebugMode = 1;
        updateFrameUniformBuffer(slot,
                                 projection * glm::lookAt(p, p + direction, up),
                                 p, captureLighting);
        using G = RenderGraph;
        G graph;
        auto shadow =
            graph.importImage({"Probe shadow atlas",
                               *shadowResources_.storage.image,
                               *shadowResources_.imageView,
                               kDepthFormat,
                               {kShadowAtlasWidth, kShadowMapSize},
                               vk::ImageAspectFlagBits::eDepth,
                               vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                   vk::ImageUsageFlagBits::eSampled,
                               false,
                               imageStates_.shadow});
        auto hdr = graph.importImage({"Probe face HDR",
                                      *color.storage.image,
                                      *color.imageView,
                                      HdrOutput::sceneFormat,
                                      {size, size},
                                      vk::ImageAspectFlagBits::eColor,
                                      colorUsage,
                                      false,
                                      colorState});
        auto velocity = graph.importImage({"Probe motion (history disabled)",
                                           *motion.storage.image,
                                           *motion.imageView,
                                           kMotionFormat,
                                           {size, size},
                                           vk::ImageAspectFlagBits::eColor,
                                           colorUsage,
                                           false,
                                           motionState});
        auto ambient = graph.importImage({"Probe unused diffuse",
                                          *diffuse.storage.image,
                                          *diffuse.imageView,
                                          HdrOutput::sceneFormat,
                                          {size, size},
                                          vk::ImageAspectFlagBits::eColor,
                                          colorUsage,
                                          false,
                                          diffuseState});
        auto z = graph.importImage({"Probe face depth",
                                    *depth.storage.image,
                                    *depth.imageView,
                                    kDepthFormat,
                                    {size, size},
                                    vk::ImageAspectFlagBits::eDepth,
                                    depthUsage,
                                    false,
                                    depthState});
        G::BufferState host{vk::PipelineStageFlagBits2::eHost,
                            vk::AccessFlagBits2::eHostWrite, true};
        auto cfg =
            graph.importBuffer({"Probe config", *slot.indoorBuffer.buffer, 0,
                                sizeof(IndoorLightingGpu),
                                vk::BufferUsageFlagBits::eStorageBuffer, host});
        auto lightBuffer = graph.importBuffer(
            {"Probe direct lights", *slot.punctualLights.buffer, 0,
             punctualHeaderBytes +
                 slot.punctualCapacity * sizeof(GpuPunctualLight),
             vk::BufferUsageFlagBits::eStorageBuffer, host});
        auto cluster = graph.importBuffer(
            {"Probe full light list", *slot.clusterConfig.buffer, 0,
             sizeof(ClusterGrid), vk::BufferUsageFlagBits::eStorageBuffer,
             host});
        std::optional<G::PassId> shadowPass;
        vk::ClearValue far{.depthStencil = {1, 0}};
        if (face == 0)
          shadowPass = graph.addPass(
              "Probe shadow capture",
              {{shadow, G::Usage::DepthAttachment, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, false, far}},
              {{cfg, G::BufferUsage::VertexRead}});
        auto main = graph.addPass(
            "Probe scene capture",
            {{shadow, G::Usage::SampledDepth},
             {hdr, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
              vk::AttachmentStoreOp::eStore, false,
              vk::ClearValue{.color = vk::ClearColorValue{std::array<float, 4>{
                                 0, 0, 0, 1}}}},
             {velocity, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
              vk::AttachmentStoreOp::eStore, false,
              vk::ClearValue{.color = vk::ClearColorValue{std::array<float, 4>{
                                 0, 0, 0, 0}}}},
             {ambient, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
              vk::AttachmentStoreOp::eStore, false,
              vk::ClearValue{.color = vk::ClearColorValue{std::array<float, 4>{
                                 0, 0, 0, 0}}}},
             {z, G::Usage::DepthAttachment, vk::AttachmentLoadOp::eClear,
              vk::AttachmentStoreOp::eStore, false, far}},
            {{cfg, G::BufferUsage::FragmentRead},
             {lightBuffer, G::BufferUsage::FragmentRead},
             {cluster, G::BufferUsage::FragmentRead}});
        graph.exportImage(hdr, G::Usage::TransferSource);
        auto plan = graph.compile();
        command.reset();
        command.begin(vk::CommandBufferBeginInfo{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        activeFrame_ = ActiveFrameState{0, 0, vk::Result::eSuccess};
        plan.record(*command, [&](G::Pass const &pass, G::Event event) {
          if (event == G::Event::Begin) {
            activePass_ =
                pass.id == main ? ActivePass::eMain : ActivePass::eShadow;
            command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                       *pipelineLayout_, 0,
                                       {slot.descriptorSet}, {});
          } else if (event == G::Event::Draw) {
            if (shadowPass && pass.id == *shadowPass)
              recordShadowTiles(scene);
            else {
              if (scene.sky)
                recordEnvironment();
              for (auto list : {scene.allOpaque, scene.allMask})
                for (auto const &item : list)
                  recordObject(item.meshId, item.materialId, item.modelMatrix);
              auto complete = scene.allTransparent.empty()
                                  ? scene.transparent
                                  : scene.allTransparent;
              auto transparent =
                  std::vector<DrawItem>(complete.begin(), complete.end());
              std::sort(
                  transparent.begin(), transparent.end(),
                  [&](auto const &a, auto const &b) {
                    auto center =
                        [](auto const &item) {
                          return item.worldBounds.valid
                                     ? (item.worldBounds.min +
                                        item.worldBounds.max) *
                                           .5f
                                     : glm::vec3(item.modelMatrix[3]);
                        };
                    return glm::dot(center(a) - p, center(a) - p) >
                           glm::dot(center(b) - p, center(b) - p);
                  });
              for (auto const &item : transparent)
                recordObject(item.meshId, item.materialId, item.modelMatrix);
            }
          } else
            activePass_ = ActivePass::eNone;
        });
        command.copyImageToBuffer(
            *color.storage.image, vk::ImageLayout::eTransferSrcOptimal,
            *readback.buffer,
            {vk::BufferImageCopy{
                .bufferOffset = faceBytes * face,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {size, size, 1}}});
        vk::BufferMemoryBarrier2 hostRead{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *readback.buffer,
            .size = faceBytes * 6};
        command.pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &hostRead});
        command.end();
        vk::CommandBuffer raw = *command;
        device_.graphicsQueue().submit(
            {vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &raw}},
            *fence);
        if (device_.logicalDevice().waitForFences({*fence}, true, UINT64_MAX) !=
            vk::Result::eSuccess)
          throw std::runtime_error("Probe capture submission failed");
        device_.logicalDevice().resetFences({*fence});
        colorState = plan.finalState(hdr);
        motionState = plan.finalState(velocity);
        diffuseState = plan.finalState(ambient);
        depthState = plan.finalState(z);
        imageStates_.shadow = plan.finalState(shadow);
        activeFrame_.reset();
      }
      std::vector<std::uint16_t> half(faceBytes * 6 / 2);
      readback.read(std::as_writable_bytes(std::span{half}));
      std::vector<float> pixels(half.size());
      // Normal right-handed cameras with a Vulkan Y flip have the opposite
      // screen-right basis to samplerCube's (s,t) on every face. Reverse each
      // row during readback; raster winding/normals retain production
      // semantics.
      for (unsigned face = 0; face < 6; ++face)
        for (unsigned y = 0; y < size; ++y)
          for (unsigned x = 0; x < size; ++x)
            for (unsigned c = 0; c < 4; ++c) {
              auto destination =
                  ((std::size_t(face) * size + y) * size + x) * 4 + c;
              auto source =
                  ((std::size_t(face) * size + y) * size + (size - 1 - x)) * 4 +
                  c;
              pixels[destination] = glm::unpackHalf1x16(half[source]);
            }
      auto equirect = environmentCubeToEquirectangular(pixels, size);
      auto bake = bakeEnvironment(equirect, {.faceSize = size,
                                             .prefilterSamples = 256,
                                             .lutSize = 2,
                                             .lutSamples = 1});
      bakes.push_back(std::move(bake));
    }
    std::vector<float> array;
    array.reserve(globalEnvironment_.cubeRgba.size() * (1 + bakes.size()));
    for (std::size_t mip = 0; mip < globalEnvironment_.levels.size(); ++mip) {
      auto count = std::size_t(globalEnvironment_.levels[mip].size) *
                   globalEnvironment_.levels[mip].size * 6 * 4;
      auto append = [&](BakedEnvironment const &source) {
        auto start = source.cubeRgba.begin() + source.levels[mip].offset;
        array.insert(array.end(), start, start + count);
      };
      append(globalEnvironment_);
      for (auto const &bake : bakes)
        append(bake);
    }
    TextureLoader loader(device_, device_.resourceLedger().scope(
                                      ResourceLedger::Domain::Persistent));
    UploadBatch upload(device_);
    auto next = loader.createFromHdrCube(array, size, upload,
                                         unsigned(1 + bakes.size()));
    upload.finish();
    vk::DescriptorImageInfo info{.sampler = next.sampler(),
                                 .imageView = next.imageView(),
                                 .imageLayout =
                                     vk::ImageLayout::eShaderReadOnlyOptimal};
    for (auto const &frame : frames_)
      device_.logicalDevice().updateDescriptorSets(
          {vk::WriteDescriptorSet{.dstSet = frame.descriptorSet,
                                  .dstBinding = 4,
                                  .descriptorCount = 1,
                                  .descriptorType =
                                      vk::DescriptorType::eCombinedImageSampler,
                                  .pImageInfo = &info}},
          {});
    environmentPrefilter_ = std::move(next);
    probeSh_ = bakes.front().sh;
    capturedProbe_ = lighting.localProbe;
    capturedDetailProbe_ = lighting.detailReflectionProbe;
    detailProbeValid_ = lighting.detailReflectionProbe.enabled;
    capturedLightingKey_ = std::move(key);
    capturedGeometryKey_ = std::move(geometryKey);
    probeGeometryDirty_ = probeMaterialsDirty_ = false;
    probeValid_ = true;
    ++resourceStatistics_.environmentUploads;
    lastIndoor_ = makeIndoorLighting(lighting, lights, true);
    drawStatistics_ = savedDraws;
  } catch (...) {
    activeFrame_.reset();
    activePass_ = ActivePass::eNone;
    activeShadowIndex_ = -1;
    drawStatistics_ = savedDraws;
    throw;
  }
}
