#include "frame_context_regression.hpp"
#include "renderer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/packing.hpp>
#include <iostream>
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
    require(*a.uniform.buffer != *b.uniform.buffer && a.descriptorSet != b.descriptorSet &&
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
  static void submit(Renderer &r, unsigned slot, float debugMode, GpuBuffer const &readback) {
    auto &frame = r.frames_[slot]; auto &command = r.commandBuffers_[slot];
    require(!frame.submitted && frame.inFlightFence.getStatus() == vk::Result::eSuccess,
            "Test attempted pending slot reuse");
    command.reset();
    LightingSettings lighting;
    lighting.pbrDebugMode = debugMode;
    lighting.shadowDebugMode = 0;
    r.updateFrameUniformBuffer(frame, glm::mat4(1), {0, 0, 2}, lighting);
    command.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    if (*frame.timestamps) command.resetQueryPool(*frame.timestamps, 0, 10);
    r.activeFrame_ = Renderer::ActiveFrameState{slot, 0, vk::Result::eSuccess};
    frame.shadowEnabled = frame.uiEnabled = false;
    using G = RenderGraph;
    G graph;
    auto depthUsage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                      vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc;
    auto shadow = graph.importImage({"Shared shadow", *r.shadowResources_.storage.image,
        *r.shadowResources_.imageView, Renderer::kDepthFormat,
        {Renderer::kShadowMapSize, Renderer::kShadowMapSize}, vk::ImageAspectFlagBits::eDepth,
        depthUsage, false, r.imageStates_.shadow});
    auto depth = graph.importImage({"Shared depth", *r.depthResources_.storage.image,
        *r.depthResources_.imageView, Renderer::kDepthFormat, r.swapChain_->extent(),
        vk::ImageAspectFlagBits::eDepth, depthUsage, false, r.imageStates_.depth});
    auto hdr = graph.importImage({"Shared HDR", r.hdrOutput_->sceneImage(), r.hdrOutput_->sceneView(),
        HdrOutput::sceneFormat, r.swapChain_->extent(), vk::ImageAspectFlagBits::eColor,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferSrc, false, r.imageStates_.hdr});
    vk::ClearValue farDepth{.depthStencil = {1, 0}};
    vk::ClearValue black{.color = vk::ClearColorValue{std::array<float, 4>{0, 0, 0, 1}}};
    if (!r.imageStates_.shadow.defined)
      graph.addPass("Initialize shadow", {{shadow, G::Usage::DepthAttachment,
          vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, false, farDepth}});
    auto main = graph.addPass("Uniform race probe", {
        {shadow, G::Usage::SampledDepth}, {hdr, G::Usage::ColorAttachment,
         vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, false, black},
        {depth, G::Usage::DepthAttachment, vk::AttachmentLoadOp::eClear,
         vk::AttachmentStoreOp::eStore, false, farDepth}});
    auto copy = graph.addPass("Readback", {{hdr, G::Usage::TransferSource}});
    graph.exportImage(hdr, G::Usage::SampledColor);
    graph.exportImage(depth, G::Usage::SampledDepth);
    auto plan = graph.compile();
    for (unsigned i = 0; i < 3; ++i) r.timestamp(command, i);
    plan.record(*command, [&](G::Pass const &pass, G::Event event) {
      if (pass.id == main) {
        if (event == G::Event::Begin) {
          r.timestamp(command, 3); r.activePass_ = Renderer::ActivePass::eMain;
          command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *r.pipelineLayout_, 0,
                                     {frame.descriptorSet}, {});
        } else if (event == G::Event::Draw) r.recordObject(0, 0, glm::mat4(1));
        else { r.timestamp(command, 4); r.activePass_ = Renderer::ActivePass::eNone; }
      } else if (pass.id == copy) {
        if (event == G::Event::Begin) r.timestamp(command, 5);
        else if (event == G::Event::End) r.timestamp(command, 6);
        else {
          auto extent = r.swapChain_->extent();
          command.copyImageToBuffer(r.hdrOutput_->sceneImage(), vk::ImageLayout::eTransferSrcOptimal,
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
      std::cout << "PASS two blocked frame slots: distinct uniforms -> HDR .25/.75, pending scene/UI retained, "
                   "all queries delivered once, reuse/reverse collection preserves latest ID, zero stale fences\n";
    } catch (...) { release(); throw; }
  }
  require(device.resourceLedger().snapshot().current == ResourceLedger::Footprint{},
          "Frame context regression leaked GPU storage");
}
