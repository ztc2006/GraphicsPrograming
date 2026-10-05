#include "renderer.hpp"
#include <cmath>
#include <stdexcept>

void Renderer::validateDrawItem(DrawItem const &item, bool caster) const {
  if (!sceneAssets_ || item.meshId >= sceneAssets_->meshes_.size())
    throw std::runtime_error("Renderer mesh id is out of range");
  auto const &material = materials().material(item.materialId);
  if (caster && material.alphaMode == AlphaMode::Blend)
    throw std::runtime_error("Transparent materials cannot be shadow casters");
  for (unsigned col = 0; col < 4; ++col)
    for (unsigned row = 0; row < 4; ++row)
      if (!std::isfinite(item.modelMatrix[col][row]))
        throw std::runtime_error("Draw transform must be finite");
}
Renderer::FrameResult Renderer::renderFrame(SceneDrawList const &scene,
                                           glm::mat4 const &viewProj,
                                           glm::vec3 const &camera,
                                           LightingSettings const &lighting,
                                           bool shadows) {
  for (auto list : {scene.opaque, scene.mask, scene.transparent})
    for (auto const &item : list) validateDrawItem(item, false);
  if (shadows)
    for (auto list : {scene.allOpaque, scene.allMask})
      for (auto const &item : list) validateDrawItem(item, true);
  auto result = beginFrame(viewProj, camera, lighting, shadows);
  if (result != FrameResult::eSuccess) return result;
  return finishFrame(scene);
}
void Renderer::drawObject(MeshId mesh, MaterialId material, glm::mat4 const &model) {
  if (!activeFrame_) throw std::runtime_error("Cannot queue draw without an active frame");
  DrawItem item{.meshId = mesh, .materialId = material, .modelMatrix = model};
  validateDrawItem(item, false);
  queuedObjects_.push_back(item);
  if (requestedShadows_ && materials().material(material).alphaMode != AlphaMode::Blend)
    queuedCasters_.push_back(item);
}
void Renderer::drawEnvironment() {
  if (!activeFrame_) throw std::runtime_error("Cannot queue sky without an active frame");
  queuedSky_ = true;
}
void Renderer::drawAabb(Aabb const &bounds, glm::vec4 const &color) {
  if (!bounds.valid) return;
  if (!activeFrame_) throw std::runtime_error("Cannot queue debug bounds without an active frame");
  queuedBoxes_.push_back({bounds, color});
}
Renderer::FrameResult Renderer::endFrame() {
  return finishFrame(SceneDrawList{.opaque = queuedObjects_, .allOpaque = queuedCasters_,
                                   .sky = queuedSky_});
}
RenderGraph::State const &Renderer::hdrState() const {
  return activeGraph_ ? activeGraph_->plan.recordedState(activeGraph_->hdr) : imageStates_.hdr;
}
Renderer::FrameGraph Renderer::buildFrameGraph(std::uint32_t imageIndex, bool shadows) const {
  using G = RenderGraph;
  G graph;
  FrameGraph frame;
  auto depthUsage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc;
  frame.shadow = graph.importImage({"Shadow depth", *shadowResources_.storage.image,
      *shadowResources_.imageView, kDepthFormat, {kShadowMapSize, kShadowMapSize},
      vk::ImageAspectFlagBits::eDepth, depthUsage, false, imageStates_.shadow});
  frame.depth = graph.importImage({"Scene depth", *depthResources_.storage.image,
      *depthResources_.imageView, kDepthFormat, swapChain_->extent(),
      vk::ImageAspectFlagBits::eDepth, depthUsage, false, imageStates_.depth});
  frame.hdr = graph.importImage({"Scene HDR", hdrOutput_->sceneImage(), hdrOutput_->sceneView(),
      HdrOutput::sceneFormat, swapChain_->extent(), vk::ImageAspectFlagBits::eColor,
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
          vk::ImageUsageFlagBits::eTransferSrc, false, imageStates_.hdr});
  frame.output = graph.importImage({"Swapchain output", swapChain_->images()[imageIndex],
      *swapChain_->imageViews()[imageIndex], swapChain_->imageFormat(), swapChain_->extent(),
      vk::ImageAspectFlagBits::eColor, vk::ImageUsageFlagBits::eColorAttachment,
      true, imageStates_.output[imageIndex]});
  vk::ClearValue farDepth{.depthStencil = {1, 0}};
  vk::ClearValue background{.color = vk::ClearColorValue{std::array<float, 4>{.05f, .07f, .1f, 1}}};
  if (shadows || !imageStates_.shadow.defined)
    frame.shadowPass = graph.addPass(shadows ? "Shadow" : "Shadow initialization", {
        {frame.shadow, G::Usage::DepthAttachment, vk::AttachmentLoadOp::eClear,
         vk::AttachmentStoreOp::eStore, false, farDepth}});
  frame.mainPass = graph.addPass("Main scene (sky + opaque + mask + blend + debug)", {
      {frame.shadow, G::Usage::SampledDepth},
      {frame.hdr, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
       vk::AttachmentStoreOp::eStore, false, background},
      {frame.depth, G::Usage::DepthAttachment, vk::AttachmentLoadOp::eClear,
       vk::AttachmentStoreOp::eStore, false, farDepth}});
  frame.outputPass = graph.addPass("Display output (exposure + filmic + sRGB)", {
      {frame.hdr, G::Usage::SampledColor},
      {frame.output, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eDontCare,
       vk::AttachmentStoreOp::eStore, true}});
  if (uiDrawCallback_)
    frame.uiPass = graph.addPass("UI", {{frame.output, G::Usage::ColorAttachment,
        vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore}});
  graph.exportImage(frame.depth, G::Usage::SampledDepth);
  graph.exportImage(frame.output, G::Usage::Present);
  frame.plan = graph.compile();
  return frame;
}
void Renderer::recordGraph(SceneDrawList const &scene) {
  if (!activeFrame_ || !activeGraph_) throw std::runtime_error("No active frame graph");
  auto const &state = *activeFrame_;
  auto &command = commandBuffers_[state.frameIndex];
  auto &graph = *activeGraph_;
  device_.beginLabel(*command, "Frame");
  timestamp(command, 0);
  if (!graph.shadowPass) { timestamp(command, 1); timestamp(command, 2); }
  graph.plan.record(*command, [&](RenderGraph::Pass const &pass, RenderGraph::Event event) {
    bool shadow = graph.shadowPass && pass.id == *graph.shadowPass;
    bool main = pass.id == graph.mainPass;
    bool output = pass.id == graph.outputPass;
    unsigned firstQuery = shadow ? 1 : main ? 3 : output ? 5 : 7;
    if (event == RenderGraph::Event::Begin) {
      device_.beginLabel(*command, pass.name.c_str());
      timestamp(command, firstQuery);
      activePass_ = shadow ? ActivePass::eShadow : main ? ActivePass::eMain : ActivePass::eNone;
      if (shadow || main)
        command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipelineLayout_, 0,
                                   {frames_[state.frameIndex].descriptorSet}, {});
    } else if (event == RenderGraph::Event::Draw) {
      if (shadow) {
        if (!requestedShadows_) return; // The graph still records the clear and store.
        for (auto list : {scene.allOpaque, scene.allMask})
          for (auto const &item : list) {
            recordObject(item.meshId, item.materialId, item.modelMatrix);
            ++drawStatistics_.shadow;
          }
      } else if (main) {
        if (scene.sky) recordEnvironment();
        for (auto list : {scene.opaque, scene.mask, scene.transparent})
          for (auto const &item : list) {
            recordObject(item.meshId, item.materialId, item.modelMatrix);
            ++drawStatistics_.main;
          }
        if (scene.bounds)
          for (auto list : {scene.opaque, scene.mask, scene.transparent})
            for (auto const &item : list) if (item.worldBounds.valid) {
              recordAabb(item.worldBounds, {.1f, .95f, .65f, .95f});
              ++drawStatistics_.debug;
            }
        for (auto const &box : queuedBoxes_) {
          recordAabb(box.bounds, box.color);
          ++drawStatistics_.debug;
        }
      } else if (output) {
        hdrOutput_->drawDisplay(*command, displaySettings_);
      } else if (graph.uiPass && pass.id == *graph.uiPass && uiDrawCallback_)
        uiDrawCallback_(*command);
    } else {
      timestamp(command, firstQuery + 1);
      device_.endLabel(*command);
      activePass_ = ActivePass::eNone;
      if (output && !graph.uiPass) { timestamp(command, 7); timestamp(command, 8); }
    }
  });
  timestamp(command, 9);
  device_.endLabel(*command);
  command.end();
}
