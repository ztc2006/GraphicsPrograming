#include "renderer.hpp"
#include <cmath>
#include <stdexcept>
#include <unordered_set>

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
  if (probeValid_) probeGeometryDirty_ = capturedGeometryKey_ != probeGeometryKey(scene);
  std::vector<MotionObject> objects;
  for (auto list : {scene.allOpaque.empty() ? scene.opaque : scene.allOpaque,
                    scene.allMask.empty() ? scene.mask : scene.allMask,
                    scene.allTransparent.empty() ? scene.transparent : scene.allTransparent})
    for (auto const &item : list) {
      validateDrawItem(item, false);
      objects.push_back({item.objectIndex, item.meshId, item.materialId, item.modelMatrix});
    }
  std::unordered_map<std::size_t, MotionObject const *> complete;
  for (auto const &object : objects) if (object.identity != invalidMotionIdentity)
    if (!complete.emplace(object.identity, &object).second)
      throw std::runtime_error("Duplicate temporal object identity");
  std::unordered_set<std::size_t> visible;
  for (auto list : {scene.opaque, scene.mask, scene.transparent})
    for (auto const &item : list) if (item.objectIndex != invalidMotionIdentity) {
      auto found = complete.find(item.objectIndex);
      if (!visible.insert(item.objectIndex).second || found == complete.end() ||
          found->second->mesh != item.meshId || found->second->material != item.materialId ||
          found->second->model != item.modelMatrix)
        throw std::runtime_error("Visible temporal instance differs from complete scene");
    }
  auto result = beginFrameImpl(viewProj, camera, lighting, shadows, objects);
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
      *shadowResources_.imageView, kDepthFormat, {kShadowAtlasWidth, kShadowMapSize},
      vk::ImageAspectFlagBits::eDepth, depthUsage, false, imageStates_.shadow});
  frame.depth = graph.importImage({"Scene depth", *depthResources_.storage.image,
      *depthResources_.imageView, kDepthFormat, swapChain_->extent(),
      vk::ImageAspectFlagBits::eDepth, depthUsage, false, imageStates_.depth});
  frame.hdr = graph.importImage({"Scene HDR", hdrOutput_->sceneImage(), hdrOutput_->sceneView(),
      HdrOutput::sceneFormat, swapChain_->extent(), vk::ImageAspectFlagBits::eColor,
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
          vk::ImageUsageFlagBits::eTransferSrc, false, imageStates_.hdr});
  frame.motion = graph.importImage({"Scene motion", *motionResources_.storage.image, *motionResources_.imageView,
      kMotionFormat, swapChain_->extent(), vk::ImageAspectFlagBits::eColor,
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
      vk::ImageUsageFlagBits::eTransferSrc, false, imageStates_.motion});
  frame.output = graph.importImage({"Swapchain output", swapChain_->images()[imageIndex],
      *swapChain_->imageViews()[imageIndex], swapChain_->imageFormat(), swapChain_->extent(),
      vk::ImageAspectFlagBits::eColor, vk::ImageUsageFlagBits::eColorAttachment,
      true, imageStates_.output[imageIndex]});
  auto const &slot = frames_[currentFrame_];
  G::BufferState host{vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite, true};
  auto lights = graph.importBuffer({"Punctual lights", *slot.punctualLights.buffer, 0,
      punctualHeaderBytes + slot.punctualCapacity * sizeof(GpuPunctualLight),
      vk::BufferUsageFlagBits::eStorageBuffer, host});
  auto config = graph.importBuffer({"Cluster config", *slot.clusterConfig.buffer, 0,
      sizeof(ClusterGrid), vk::BufferUsageFlagBits::eStorageBuffer, host});
  auto indoor = graph.importBuffer({"Shadow/probe config", *slot.indoorBuffer.buffer, 0,
      sizeof(IndoorLightingGpu), vk::BufferUsageFlagBits::eStorageBuffer, host});
  auto motion = graph.importBuffer({"Previous instance transforms", *slot.motionBuffer.buffer, 0,
      slot.motionCapacityBytes, vk::BufferUsageFlagBits::eStorageBuffer, host});
  std::vector<G::BufferUse> mainBuffers{{motion, G::BufferUsage::VertexRead}, {lights, G::BufferUsage::FragmentRead},
                                       {config, G::BufferUsage::FragmentRead}, {indoor, G::BufferUsage::FragmentRead}};
  if (slot.clusterEnabled) {
    frame.clusterIndices = graph.importBuffer({"Cluster indices", *slot.clusterIndices.buffer,
        0, clusterListBytes(slot.clusterGrid), vk::BufferUsageFlagBits::eStorageBuffer,
        slot.clusterState});
    frame.clusterPass = graph.addPass("Cluster light assignment", {}, {
        {lights, G::BufferUsage::ComputeRead}, {config, G::BufferUsage::ComputeRead},
        {frame.clusterIndices, G::BufferUsage::ComputeWrite, true}});
    mainBuffers.push_back({frame.clusterIndices, G::BufferUsage::FragmentRead});
  }
  vk::ClearValue farDepth{.depthStencil = {1, 0}};
  vk::ClearValue background{.color = vk::ClearColorValue{std::array<float, 4>{.05f, .07f, .1f, 1}}};
  if (shadows || !imageStates_.shadow.defined)
    frame.shadowPass = graph.addPass(shadows ? "Shadow" : "Shadow initialization", {
        {frame.shadow, G::Usage::DepthAttachment, vk::AttachmentLoadOp::eClear,
         vk::AttachmentStoreOp::eStore, false, farDepth}}, {{indoor, G::BufferUsage::VertexRead}});
  frame.mainPass = graph.addPass("Main scene (sky + opaque + mask + blend + debug)", {
      {frame.shadow, G::Usage::SampledDepth},
      {frame.hdr, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
       vk::AttachmentStoreOp::eStore, false, background},
      {frame.motion, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
       vk::AttachmentStoreOp::eStore, false, vk::ClearValue{.color=vk::ClearColorValue{std::array<float,4>{0,0,0,0}}}},
      {frame.depth, G::Usage::DepthAttachment, vk::AttachmentLoadOp::eClear,
       vk::AttachmentStoreOp::eStore, false, farDepth}}, std::move(mainBuffers));
  auto displayInput=frame.hdr;
  if(slot.taaEnabled){frame.taa=taa_->addPass(graph,frame.hdr,frame.depth,frame.motion);displayInput=frame.taa->color[frame.taa->write];}
  frame.outputPass = graph.addPass("Display output (exposure + filmic + sRGB)", {
      {displayInput, G::Usage::SampledColor},
      {frame.output, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eDontCare,
       vk::AttachmentStoreOp::eStore, true}});
  if (uiDrawCallback_)
    frame.uiPass = graph.addPass("UI", {{frame.output, G::Usage::ColorAttachment,
        vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore}});
  graph.exportImage(frame.motion, G::Usage::SampledColor);
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
    bool cluster = graph.clusterPass && pass.id == *graph.clusterPass;
    bool shadow = graph.shadowPass && pass.id == *graph.shadowPass;
    bool main = pass.id == graph.mainPass;
    bool output = pass.id == graph.outputPass;
    bool temporal=graph.taa && pass.id==graph.taa->resolve;
    unsigned firstQuery = cluster ? 10 : shadow ? 1 : main ? 3 : output ? 5 : 7;
    if (event == RenderGraph::Event::Begin) {
      device_.beginLabel(*command, pass.name.c_str());
      if(temporal){if(*frames_[state.frameIndex].taaTimestamps)command.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe,*frames_[state.frameIndex].taaTimestamps,0);}
      else if(cluster||shadow||main||output||(graph.uiPass&&pass.id==*graph.uiPass))timestamp(command, firstQuery);
      activePass_ = shadow ? ActivePass::eShadow : main ? ActivePass::eMain : ActivePass::eNone;
      if (cluster) {
        command.bindPipeline(vk::PipelineBindPoint::eCompute, *clusterPipeline_);
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *clusterPipelineLayout_, 0,
                                   {frames_[state.frameIndex].descriptorSet}, {});
      }
      if (shadow || main)
        command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipelineLayout_, 0,
                                   {frames_[state.frameIndex].descriptorSet}, {});
    } else if (event == RenderGraph::Event::Draw) {
      if (cluster) {
        command.dispatch((frames_[state.frameIndex].clusterGrid.grid.w + 63) / 64, 1, 1);
      } else if (shadow) {
        if (!requestedShadows_) return; // The graph still records the clear and store.
        recordShadowTiles(scene);
      } else if (main) {
        if (scene.sky) recordEnvironment();
        for (auto list : {scene.opaque, scene.mask, scene.transparent})
          for (auto const &item : list) {
            std::uint32_t index = 0;
            auto const &temporal = frames_[state.frameIndex].temporal;
            if (temporal) {
              auto found = temporal->indices.find(item.objectIndex);
              if (found != temporal->indices.end()) index = found->second;
            }
            recordObject(item.meshId, item.materialId, item.modelMatrix, index);
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
      } else if(temporal){taa_->draw(*command,taaPush_);
      } else if (output) {
        hdrOutput_->drawDisplay(*command, displaySettings_,graph.taa?int(graph.taa->write):-1);
      } else if (graph.uiPass && pass.id == *graph.uiPass && uiDrawCallback_)
        uiDrawCallback_(*command);
    } else {
      if(temporal){if(*frames_[state.frameIndex].taaTimestamps)command.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe,*frames_[state.frameIndex].taaTimestamps,1);}
      else if(cluster||shadow||main||output||(graph.uiPass&&pass.id==*graph.uiPass))timestamp(command, firstQuery + 1);
      device_.endLabel(*command);
      activePass_ = ActivePass::eNone;
      if (output && !graph.uiPass) { timestamp(command, 7); timestamp(command, 8); }
    }
  });
  timestamp(command, 9);
  device_.endLabel(*command);
  command.end();
}
