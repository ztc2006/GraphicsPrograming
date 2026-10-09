#include "frame_context_regression.hpp"
#include "renderer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool condition, char const *message) {
  if (!condition) throw std::runtime_error(message);
}
}
// Use production slot resources, uniforms, scene shaders, graph barriers and
// submission bookkeeping. Only WSI output is omitted, because present may block
// the host while the test's future timeline signal is still pending.
struct RendererFrameTestAccess {
  static std::weak_ptr<Renderer::SceneAssets> live(Renderer const &r) { return r.sceneAssets_; }
  static void verifyDistinct(Renderer const &r) {
    auto const &a = r.frames_[0]; auto const &b = r.frames_[1];
    require(*a.motionBuffer.buffer != *b.motionBuffer.buffer &&
                *a.uniform.buffer != *b.uniform.buffer &&
                *a.punctualLights.buffer != *b.punctualLights.buffer &&
                *a.clusterConfig.buffer != *b.clusterConfig.buffer &&
                *a.indoorBuffer.buffer != *b.indoorBuffer.buffer &&
                *a.clusterIndices.buffer != *b.clusterIndices.buffer &&
                a.descriptorSet != b.descriptorSet &&
                *a.inFlightFence != *b.inFlightFence &&
                *a.imageAvailableSemaphore != *b.imageAvailableSemaphore &&
                *r.commandBuffers_[0] != *r.commandBuffers_[1],
            "Mutable frame resources were shared between slots");
    if (r.gpuTimingSupported()) require(*a.timestamps != *b.timestamps, "Query pools alias");
    std::cout << "Frame uniform payload per slot: " << a.uniform.memoryInfo().payloadBytes << " bytes\n";
  }
  static void verifyDrained(Renderer &r) {
    r.collectCompletedWork();
    for (auto const &frame : r.frames_) require(!frame.submitted, "Completed slot was not drained");
    require(std::all_of(r.imagesInFlight_.begin(), r.imagesInFlight_.end(),
                       [](auto fence) { return !fence; }), "Completed image retained a recyclable slot fence");
    require(r.gpuTimings().frameId == r.submittedFrameId(), "Latest timing regressed to an older slot");
  }
  static void verifyLightSnapshots(Renderer const &r) {
    std::array<std::uint32_t, 4> a{}, b{};
    r.frames_[0].punctualLights.read(std::as_writable_bytes(std::span{a}));
    r.frames_[1].punctualLights.read(std::as_writable_bytes(std::span{b}));
    require(
        a[0] == 65 && b[0] == 1 && r.frames_[0].punctualCapacity == 128 &&
            r.frames_[1].punctualCapacity == 64,
        "Pending slot descriptor growth/count mutated another light snapshot");
    ClusterGrid ga, gb;
    r.frames_[0].clusterConfig.read(std::as_writable_bytes(std::span{&ga, 1}));
    r.frames_[1].clusterConfig.read(std::as_writable_bytes(std::span{&gb, 1}));
    if (r.clusterSupported())
      require(ga.screen.z == 1 && gb.screen.z == 1 && ga.cameraNear.z == 3 &&
                  gb.cameraNear.z == 4 && r.frames_[0].clusterCapacityBytes > 4 &&
                  r.frames_[1].clusterCapacityBytes > 4 &&
                  *r.frames_[0].clusterIndices.buffer != *r.frames_[1].clusterIndices.buffer,
              "Pending slots shared cluster config/indices or failed safe growth");
  }
  static void verifyMotionSnapshots(Renderer const &r) {
    auto const &a=r.frames_[0]; auto const &b=r.frames_[1];
    MotionObjectGpu ga,gb;
    a.motionBuffer.read(std::as_writable_bytes(std::span{&ga,1}),sizeof(MotionObjectGpu));
    b.motionBuffer.read(std::as_writable_bytes(std::span{&gb,1}),sizeof(MotionObjectGpu));
    require(*a.motionBuffer.buffer!=*b.motionBuffer.buffer && a.motionCapacityBytes==320 &&
        b.motionCapacityBytes==160 && ga.flags.x==1 && gb.flags.x==0 &&
        std::abs(ga.previousModel[3].x-.2f)<1e-6f,"Pending motion storage/identity overwritten");
  }
  static void submit(Renderer &r, unsigned slot, float debugMode,
                     GpuBuffer const &readback, unsigned lights = 0, bool clustered = false, bool cascades = false, bool motionCase = false, bool aoCase = false) {
    auto &frame = r.frames_[slot]; auto &command = r.commandBuffers_[slot];
    require(!frame.submitted && frame.inFlightFence.getStatus() == vk::Result::eSuccess,
            "Test attempted pending slot reuse");
    command.reset();
    LightingSettings lighting;
    lighting.pbrDebugMode = debugMode;
    lighting.shadowDebugMode = 0;
    glm::mat4 view(1);
    glm::vec3 camera{0, 0, 2};
    auto extent = r.swapChain_->extent();
    if (lights) {
      view[1][1] = -1;
      lighting.sunEnabled = false;
      lighting.environmentIntensity = 0;
      lighting.diffuseStrength = slot == 0 ? .25f : .75f;
      lighting.specularStrength = 0;
      PunctualLight point;
      point.position = {1.f / extent.width, -1.f / extent.height, 1.5f};
      if (clustered) {
        camera = {0, 0, slot == 0 ? 3.f : 4.f};
        auto projection = glm::perspective(glm::radians(75.f),
            float(extent.width) / extent.height, .1f, 50.f);
        projection[1][1] *= -1;
        view = projection * glm::lookAt(camera, glm::vec3{0}, glm::vec3{0, 1, 0});
        auto nearPoint = glm::inverse(view) * glm::vec4{
            1.f / extent.width, 1.f / extent.height, 0, 1};
        auto ray = glm::vec3(nearPoint) / nearPoint.w - camera;
        point.position = camera + ray * ((.5f - camera.z) / ray.z) + glm::vec3{0, 0, 1};
        point.range = 1e6f; // Finite sphere, unit irradiance at the readback pixel.
      }
      point.intensity = std::numbers::pi_v<float> / lights;
      lighting.punctualLights.assign(lights, point);
    }
    if (cascades || aoCase) {
      camera = {0,0,3};
      Camera c; c.position=camera; c.target={0,0,0}; c.farPlane=50;
      view=c.viewProj(float(extent.width)/extent.height);
      lighting.sunCascades.count=slot==0?4:2;
      lighting.sunCascades.distance=slot==0?10:40;
    }
    auto snapshot = packPunctualLights(lighting.punctualLights,
                                       r.device_.physicalDevice()
                                           .getProperties()
                                           .limits.maxStorageBufferRange);
    r.updateFrameLights(frame, snapshot);
    auto limits = r.device_.physicalDevice().getProperties().limits;
    r.updateFrameClusters(frame, makeClusterGrid(view, camera, extent.width, extent.height,
        limits.maxStorageBufferRange, limits.maxComputeWorkGroupCount[0],
        clustered && lights && r.clusterSupported()));
    frame.indoor=r.makeIndoorLighting(lighting,snapshot,false);
    if (cascades) frame.indoor.sun=buildSunCascades(view,camera,{0,1,0},lighting.sunCascades,1);
    frame.indoorBuffer.write(std::as_bytes(std::span{&frame.indoor,1}));
    glm::mat4 model(1);
    if (motionCase) {
      model[3].x = slot == 1 ? .2f : .4f;
      std::vector<MotionObject> objects{{7,0,0,model}};
      if (slot == 0) {objects.push_back({8,0,0,glm::mat4(1)});objects.push_back({9,0,0,glm::mat4(1)});}
      r.updateFrameMotion(frame,r.temporalHistory_.prepare(view,camera,extent.width,extent.height,false,objects));
    }
    r.updateFrameUniformBuffer(frame, view, camera, lighting);
    command.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    if (*frame.timestamps) command.resetQueryPool(*frame.timestamps, 0, 12);
    frame.aoEnabled = aoCase;
    if (aoCase && *frame.aoTimestamps)
      command.resetQueryPool(*frame.aoTimestamps, 0, 6);
    GtaoPush aoPush{
        .inverseRaster = glm::inverse(view),
        .camera = glm::vec4(camera, 1),
        .settings = {.5f, 1, slot == 0 ? 1.f : 0.f, 128},
        .screen = {float(extent.width), float(extent.height), 0, 0}};
    r.activeFrame_ = Renderer::ActiveFrameState{slot, 0, vk::Result::eSuccess};
    frame.shadowEnabled = frame.uiEnabled = false;
    using G = RenderGraph;
    G graph;
    auto depthUsage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                      vk::ImageUsageFlagBits::eSampled |
                      vk::ImageUsageFlagBits::eTransferSrc;
    auto shadow = graph.importImage(
        {"Shared shadow",
         *r.shadowResources_.storage.image,
         *r.shadowResources_.imageView,
         Renderer::kDepthFormat,
         {Renderer::kShadowAtlasWidth, Renderer::kShadowMapSize},
         vk::ImageAspectFlagBits::eDepth,
         depthUsage,
         false,
         r.imageStates_.shadow});
    auto depth = graph.importImage(
        {"Shared depth", *r.depthResources_.storage.image,
         *r.depthResources_.imageView, Renderer::kDepthFormat,
         r.swapChain_->extent(), vk::ImageAspectFlagBits::eDepth, depthUsage,
         false, r.imageStates_.depth});
    auto hdr = graph.importImage({"Shared HDR", r.hdrOutput_->sceneImage(), r.hdrOutput_->sceneView(),
        HdrOutput::sceneFormat, r.swapChain_->extent(), vk::ImageAspectFlagBits::eColor,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferSrc, false, r.imageStates_.hdr});
    auto diffuse = r.gtao_->importDiffuse(graph);
    auto motion = graph.importImage({"Shared motion", *r.motionResources_.storage.image,
        *r.motionResources_.imageView, Renderer::kMotionFormat, r.swapChain_->extent(),
        vk::ImageAspectFlagBits::eColor, vk::ImageUsageFlagBits::eColorAttachment |
        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
        false, r.imageStates_.motion});
    G::BufferState host{vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite, true};
    auto lightBuffer = graph.importBuffer({"Slot lights", *frame.punctualLights.buffer, 0,
        punctualHeaderBytes + frame.punctualCapacity * sizeof(GpuPunctualLight),
        vk::BufferUsageFlagBits::eStorageBuffer, host});
    auto config = graph.importBuffer({"Slot cluster config", *frame.clusterConfig.buffer, 0,
        sizeof(ClusterGrid), vk::BufferUsageFlagBits::eStorageBuffer, host});
    auto indoor=graph.importBuffer({"Slot shadow/probe",*frame.indoorBuffer.buffer,0,
        sizeof(IndoorLightingGpu),vk::BufferUsageFlagBits::eStorageBuffer,host});
    auto transforms=graph.importBuffer({"Slot previous transforms",*frame.motionBuffer.buffer,0,
      frame.motionCapacityBytes,vk::BufferUsageFlagBits::eStorageBuffer,host});
    std::vector<G::BufferUse> reads{{transforms,G::BufferUsage::VertexRead},{indoor,G::BufferUsage::FragmentRead},{lightBuffer, G::BufferUsage::FragmentRead},
                                   {config, G::BufferUsage::FragmentRead}};
    std::optional<G::PassId> cull;
    G::BufferId indices;
    if (frame.clusterEnabled) {
      indices = graph.importBuffer({"Slot cluster indices", *frame.clusterIndices.buffer, 0,
          clusterListBytes(frame.clusterGrid), vk::BufferUsageFlagBits::eStorageBuffer,
          frame.clusterState});
      cull = graph.addPass("Slot cluster assignment", {}, {
          {lightBuffer, G::BufferUsage::ComputeRead}, {config, G::BufferUsage::ComputeRead},
          {indices, G::BufferUsage::ComputeWrite, true}});
      reads.push_back({indices, G::BufferUsage::FragmentRead});
    }
    vk::ClearValue farDepth{.depthStencil = {1, 0}};
    vk::ClearValue black{.color = vk::ClearColorValue{std::array<float, 4>{0, 0, 0, 1}}};
    if (!r.imageStates_.shadow.defined)
      graph.addPass(
          "Initialize shadow",
          {{shadow, G::Usage::DepthAttachment, vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore, false, farDepth}});
    auto main = graph.addPass(
        "Uniform race probe",
        {{shadow, G::Usage::SampledDepth},
         {hdr, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
          vk::AttachmentStoreOp::eStore, false, black},
         {motion, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
          vk::AttachmentStoreOp::eStore, false, black},
         {diffuse, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
          vk::AttachmentStoreOp::eStore, false, black},
         {depth, G::Usage::DepthAttachment, vk::AttachmentLoadOp::eClear,
          vk::AttachmentStoreOp::eStore, false, farDepth}},
        std::move(reads));
    std::optional<Gtao::Frame> ao;
    if (aoCase)
      ao = r.gtao_->addPasses(graph, hdr, depth, diffuse);
    auto copy = graph.addPass("Readback", {{ao ? ao->images[Gtao::Composite]
                                            : motionCase ? motion
                                                         : hdr,
                                            G::Usage::TransferSource}});
    if (ao)
      graph.exportImage(ao->images[Gtao::Composite], G::Usage::SampledColor);
    graph.exportImage(hdr, G::Usage::SampledColor);
    graph.exportImage(depth, G::Usage::SampledDepth);
    auto plan = graph.compile();
    for (unsigned i = 0; i < 3; ++i)
      r.timestamp(command, i);
    plan.record(*command, [&](G::Pass const &pass, G::Event event) {
      int aoPass = !ao                        ? -1
                   : pass.id == ao->horizon   ? 0
                   : pass.id == ao->filter    ? 1
                   : pass.id == ao->composite ? 2
                                              : -1;
      if (aoPass >= 0) {
        if (event == G::Event::Begin && *frame.aoTimestamps)
          command.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe,
                                  *frame.aoTimestamps, unsigned(aoPass) * 2);
        else if (event == G::Event::End && *frame.aoTimestamps)
          command.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe,
                                  *frame.aoTimestamps,
                                  unsigned(aoPass) * 2 + 1);
        else if (event == G::Event::Draw)
          r.gtao_->draw(*command, unsigned(aoPass), aoPush);
      } else if (cull && pass.id == *cull) {
        if (event == G::Event::Begin) {
          r.timestamp(command, 10);
          command.bindPipeline(vk::PipelineBindPoint::eCompute,
                               *r.clusterPipeline_);
          command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                     *r.clusterPipelineLayout_, 0,
                                     {frame.descriptorSet}, {});
        } else if (event == G::Event::Draw)
          command.dispatch((frame.clusterGrid.grid.w + 63) / 64, 1, 1);
        else
          r.timestamp(command, 11);
      } else if (pass.id == main) {
        if (event == G::Event::Begin) {
          r.timestamp(command, 3);
          r.activePass_ = Renderer::ActivePass::eMain;
          command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                     *r.pipelineLayout_, 0,
                                     {frame.descriptorSet}, {});
        } else if (event == G::Event::Draw)
          r.recordObject(0, lights ? 1 : 0, model, motionCase ? 1 : 0);
        else {
          r.timestamp(command, 4);
          r.activePass_ = Renderer::ActivePass::eNone;
        }
      } else if (pass.id == copy) {
        if (event == G::Event::Begin)
          r.timestamp(command, 5);
        else if (event == G::Event::End)
          r.timestamp(command, 6);
        else {
          auto extent = r.swapChain_->extent();
          command.copyImageToBuffer(ao?r.gtao_->image(Gtao::Composite):motionCase ? *r.motionResources_.storage.image : r.hdrOutput_->sceneImage(), vk::ImageLayout::eTransferSrcOptimal,
              *readback.buffer, {vk::BufferImageCopy{
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageOffset = {int(extent.width / 2), int(extent.height / 2), 0},
                .imageExtent = {1, 1, 1}}});
          vk::BufferMemoryBarrier2 host{
              .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
              .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
              .dstStageMask = vk::PipelineStageFlagBits2::eHost,
              .dstAccessMask = vk::AccessFlagBits2::eHostRead,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .buffer = *readback.buffer, .size = 8};
          command.pipelineBarrier2(vk::DependencyInfo{
              .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host});
        }
      }
    });
    for (unsigned i = 7; i < 10; ++i) r.timestamp(command, i);
    command.end();
    vk::CommandBuffer raw = *command;
    r.device_.logicalDevice().resetFences({*frame.inFlightFence});
    r.device_.graphicsQueue().submit({vk::SubmitInfo{.commandBufferCount = 1,
                                                    .pCommandBuffers = &raw}}, *frame.inFlightFence);
    r.onFrameSubmitted(frame);
    r.imageStates_.shadow = plan.finalState(shadow);
    r.imageStates_.depth = plan.finalState(depth);
    r.imageStates_.hdr = plan.finalState(hdr);
    r.imageStates_.motion = plan.finalState(motion);
    r.gtao_->submitted(plan,diffuse,ao?&*ao:nullptr);
    if (frame.clusterEnabled) frame.clusterState = plan.finalBufferState(indices);
    r.activeFrame_.reset();
  }
};
void verifyDrainedFrameContexts(Renderer &r) { RendererFrameTestAccess::verifyDrained(r); }
void exerciseFrameContexts(Device const &device, SwapChain const &swapchain) {
  for (unsigned count : {0u, 3u}) {
    bool rejected = false;
    try { Renderer invalid(device, count); } catch (std::invalid_argument const &) { rejected = true; }
    require(rejected, "Invalid renderer frame count accepted");
    require(device.resourceLedger().snapshot().current == ResourceLedger::Footprint{},
            "Invalid renderer allocated GPU storage");
  }
  if (!device.timelineSemaphoreSupported()) {
    std::cout << "SKIP subcase: frame pending gate requires timeline semaphore\n"; return;
  }
  {
    Renderer renderer(device, 2); renderer.recreateForSwapChain(swapchain);
    RendererFrameTestAccess::verifyDistinct(renderer);
    AssetLibrary assets;
    Mesh triangle;
    for (auto p : {glm::vec3{-1, -1, .5f}, glm::vec3{3, -1, .5f}, glm::vec3{-1, 3, .5f}})
      triangle.vertices.push_back(Vertex{.position = p, .color = {1, 1, 1}, .normal = {0, 0, 1}});
    triangle.indices = {0, 1, 2}; assets.meshes.push_back(triangle);
    Material material; material.doubleSided = true; material.metallicFactor = .25f;
    material.roughnessFactor = .75f; assets.materials.push_back(material);
    auto diffuse = material;
    diffuse.metallicFactor = 0;
    diffuse.specularFactor = 0;
    assets.materials.push_back(diffuse);
    auto initial = renderer.prepareScene(assets); renderer.waitSceneUpload(initial);
    require(renderer.commitScene(initial), "Initial frame probe scene did not commit");
    auto old = RendererFrameTestAccess::live(renderer);
    auto replacement = renderer.prepareScene(assets); renderer.waitSceneUpload(replacement);
    auto a = device.createBuffer(8, vk::BufferUsageFlagBits::eTransferDst, vk::MemoryPropertyFlagBits::eHostVisible);
    auto b = device.createBuffer(8, vk::BufferUsageFlagBits::eTransferDst, vk::MemoryPropertyFlagBits::eHostVisible);
    std::vector<GpuTimings> samples;
    renderer.setGpuTimingCallback([&](auto const &sample) { samples.push_back(sample); });
    vk::SemaphoreTypeCreateInfo type{.semaphoreType = vk::SemaphoreType::eTimeline};
    vk::raii::Semaphore gate(device.logicalDevice(), vk::SemaphoreCreateInfo{.pNext = &type});
    std::uint64_t value = 1; vk::Semaphore raw = *gate;
    vk::TimelineSemaphoreSubmitInfo timeline{.waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &value};
    vk::PipelineStageFlags stages = vk::PipelineStageFlagBits::eAllCommands;
    device.graphicsQueue().submit({vk::SubmitInfo{.pNext = &timeline, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &raw, .pWaitDstStageMask = &stages}}, nullptr);
    bool released = false;
    auto release = [&] {
      if (!released) { device.logicalDevice().signalSemaphore({.semaphore = raw, .value = value}); released = true; }
      device.logicalDevice().waitIdle();
    };
    try {
      RendererFrameTestAccess::submit(renderer, 0, 2, a);
      RendererFrameTestAccess::submit(renderer, 1, 3, b);
      unsigned uiReleases = 0;
      require(renderer.commitScene(replacement, [&] { ++uiReleases; }), "Ready replacement failed to commit");
      renderer.collectCompletedWork();
      require(samples.empty() && renderer.resourceStatistics().completedFrameId == 0 &&
                  renderer.resourceStatistics().retiredScenes == 1 && !old.expired() && uiReleases == 0,
              "Pending frames released old scene/UI or returned premature timing");
      bool rejected = false;
      try { renderer.recreateForSwapChain(swapchain); } catch (std::runtime_error const &) { rejected = true; }
      require(rejected, "Replacement destroyed shared targets while frames were pending");
      release(); renderer.collectCompletedWork();
      require(samples.size() == 2 && samples[0].frameId == 1 && samples[1].frameId == 2 &&
                  renderer.gpuTimings().frameId == 2 && renderer.resourceStatistics().completedFrameId == 2 &&
                  renderer.resourceStatistics().retiredScenes == 0 && old.expired() && uiReleases == 1,
              "Completed frames lost queries or failed scene/UI retirement");
      if (renderer.gpuTimingSupported()) require(samples[0].valid && samples[1].valid, "Frame queries invalid");
      auto checkPixel = [](GpuBuffer const &buffer, float expected) {
        std::array<std::uint16_t, 4> half{}; buffer.read(std::as_writable_bytes(std::span{half}));
        for (unsigned c = 0; c < 3; ++c)
          require(std::abs(glm::unpackHalf1x16(half[c]) - expected) < .001f,
                  "Pending frame uniform overwritten or shared HDR access raced");
        require(glm::unpackHalf1x16(half[3]) == 1, "Frame probe alpha changed");
      };
      checkPixel(a, .25f); checkPixel(b, .75f);
      renderer.collectCompletedWork(); require(samples.size() == 2, "Duplicate completion callback");
      // Reverse slot submission order; collecting slot 0 first now yields newer
      // frame 4 before frame 3. Latest UI data must never regress to frame 3.
      RendererFrameTestAccess::submit(renderer, 1, 3, b);
      RendererFrameTestAccess::submit(renderer, 0, 2, a);
      device.logicalDevice().waitIdle(); renderer.collectCompletedWork();
      require(samples.size() == 4 && samples[2].frameId == 4 && samples[3].frameId == 3 &&
                  renderer.gpuTimings().frameId == 4 && renderer.resourceStatistics().completedFrameId == 4,
              "Slot reuse lost samples or latest completion regressed");
      checkPixel(a, .25f); checkPixel(b, .75f);
      RendererFrameTestAccess::verifyDrained(renderer);
      // Block both slots again, then grow idle slot 0 while slot 1 is pending.
      // Only the descriptor of the fence-completed slot may be updated.
      value = 2;
      released = false;
      device.graphicsQueue().submit(
          {vk::SubmitInfo{.pNext = &timeline,
                          .waitSemaphoreCount = 1,
                          .pWaitSemaphores = &raw,
                          .pWaitDstStageMask = &stages}},
          nullptr);
      RendererFrameTestAccess::submit(renderer, 1, 14, b, 1, true);
      RendererFrameTestAccess::submit(renderer, 0, 14, a, 65, true);
      RendererFrameTestAccess::verifyLightSnapshots(renderer);
      renderer.collectCompletedWork();
      require(samples.size() == 4, "Blocked light frame returned early");
      release();
      renderer.collectCompletedWork();
      checkPixel(a, .25f);
      checkPixel(b, .75f);
      require(samples.size() == 6 && renderer.gpuTimings().frameId == 6 &&
                  renderer.resourceStatistics().completedFrameId == 6,
              "Light slot completion lost samples or regressed IDs");
      if (renderer.clusterSupported() && renderer.gpuTimingSupported())
        require(samples[4].clustered && samples[5].clustered &&
                    samples[4].cullingMs > 0 && samples[5].cullingMs > 0,
                "Pending cluster slot timestamps missing or conflated");
      RendererFrameTestAccess::verifyDrained(renderer);
      std::cout << "PASS two blocked light/cluster slots: 65/1 counts, slot-local "
                   "64->128 light growth, independent cluster growth/cameras, HDR .25/.75\n";

      value=3; released=false;
      device.graphicsQueue().submit({vk::SubmitInfo{.pNext=&timeline,.waitSemaphoreCount=1,
          .pWaitSemaphores=&raw,.pWaitDstStageMask=&stages}},nullptr);
      RendererFrameTestAccess::submit(renderer,0,17,a,0,false,true);
      RendererFrameTestAccess::submit(renderer,1,17,b,0,false,true);
      renderer.collectCompletedWork();
      require(samples.size()==6,"Blocked CSM snapshot returned early");
      release();renderer.collectCompletedWork();
      auto color=[](GpuBuffer const &buffer) {
        std::array<std::uint16_t,4> h{};buffer.read(std::as_writable_bytes(std::span{h}));
        return glm::vec3{glm::unpackHalf1x16(h[0]),glm::unpackHalf1x16(h[1]),glm::unpackHalf1x16(h[2])};
      };
      require(glm::length(color(a)-glm::vec3{.15f,.35f,1})<.001f &&
          glm::length(color(b)-glm::vec3{1,.15f,.15f})<.001f,
          "Pending CSM SSBO overwritten: 4-level blue/2-level red readback differs");
      require(samples.size()==8 && renderer.gpuTimings().frameId==8,
          "CSM pending queries lost or IDs regressed");
      RendererFrameTestAccess::verifyDrained(renderer);
      std::cout<<"PASS two blocked CSM snapshots: 4/2 counts, 10/40 distance, blue/red HDR, independent SSBO and queries\n";

      renderer.invalidateTemporalHistory();
      value=4;released=false;
      device.graphicsQueue().submit({vk::SubmitInfo{.pNext=&timeline,.waitSemaphoreCount=1,
          .pWaitSemaphores=&raw,.pWaitDstStageMask=&stages}},nullptr);
      RendererFrameTestAccess::submit(renderer,1,2,b,0,false,false,true);
      RendererFrameTestAccess::submit(renderer,0,2,a,0,false,false,true);
      renderer.collectCompletedWork();
      require(samples.size()==8,"Blocked motion returned early");
      RendererFrameTestAccess::verifyMotionSnapshots(renderer);
      release();renderer.collectCompletedWork();
      std::array<std::uint16_t,4> ma{},mb{};a.read(std::as_writable_bytes(std::span{ma}));b.read(std::as_writable_bytes(std::span{mb}));
      require(glm::unpackHalf1x16(mb[2])==-1 && glm::unpackHalf1x16(ma[2])==1 &&
          std::abs(glm::unpackHalf1x16(ma[0])-.1f)<.001f,
          "Pending motion snapshots did not follow successful submission order");
      require(samples.size()==10 && renderer.gpuTimings().frameId==10,"Motion query IDs regressed");
      RendererFrameTestAccess::verifyDrained(renderer);
      std::cout<<"PASS two blocked motion slots: reversed slot order, first invalid then +.1 UV, independent 160/320B previous transforms, ten exact query callbacks\n";

      value=5;released=false;
      device.graphicsQueue().submit({vk::SubmitInfo{.pNext=&timeline,.waitSemaphoreCount=1,
          .pWaitSemaphores=&raw,.pWaitDstStageMask=&stages}},nullptr);
      RendererFrameTestAccess::submit(renderer,0,2,a,0,false,false,false,true);
      RendererFrameTestAccess::submit(renderer,1,2,b,0,false,false,false,true);
      renderer.collectCompletedWork();require(samples.size()==10,"AO query completed before timeline release");
      release();renderer.collectCompletedWork();
      require(glm::length(color(a)-glm::vec3(1))<.04f && glm::length(color(b)-glm::vec3(.25f))<.001f,"Pending AO push/debug mode overwritten or shared targets raced");
      require(samples.size()==12 && samples[10].aoMs>0 && samples[11].aoMs>0,"Pending AO query lost/duplicated");
      RendererFrameTestAccess::verifyDrained(renderer);
      std::cout<<"PASS two timeline-blocked AO slots: raw white / HDR .25, immutable pushes, shared graph targets, independent six-query pools\n";

      std::cout << "PASS two blocked frame slots: distinct uniforms -> HDR .25/.75, pending scene/UI retained, "
                   "all queries delivered once, reuse/reverse collection preserves latest ID, zero stale fences\n";
    } catch (...) { release(); throw; }
  }
  require(device.resourceLedger().snapshot().current == ResourceLedger::Footprint{},
          "Frame context regression leaked GPU storage");
}
