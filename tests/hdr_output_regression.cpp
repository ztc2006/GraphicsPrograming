#include "hdr_output_regression.hpp"
#include "frustum.hpp"
#include "gltf_loader.hpp"
#include "hdr_output.hpp"
#include "lighting_presets.hpp"
#include "renderer.hpp"
#include "viewer_scene_prepare.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glm/gtc/packing.hpp>
#include <iostream>
#include <limits>
#include <numbers>

struct RendererHdrTestAccess {
  static void environment(Renderer &r, BakedEnvironment const &bake) {
    // Called only after completed frames; descriptors are never mutated in
    // flight.
    TextureLoader loader(r.device_, r.device_.resourceLedger().scope(
                                        ResourceLedger::Domain::Persistent));
    UploadBatch upload(r.device_);
    auto cube = loader.createFromHdrCube(bake.cubeRgba,
                                         bake.levels.front().size, upload);
    auto lut = loader.createFromHdrPixels(bake.brdfLut.rgba, bake.brdfLut.width,
                                          bake.brdfLut.height, upload,
                                          TextureWrap::ClampToEdge);
    upload.finish();
    r.environmentPrefilter_ = std::move(cube);
    r.environmentBrdfLut_ = std::move(lut);
    r.environmentSh_ = bake.sh;
    r.globalEnvironment_ = bake;
    r.probeValid_ = false;
    vk::DescriptorImageInfo cubeInfo{
        .sampler = r.environmentPrefilter_.sampler(),
        .imageView = r.environmentPrefilter_.imageView(),
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    vk::DescriptorImageInfo lutInfo{
        .sampler = r.environmentBrdfLut_.sampler(),
        .imageView = r.environmentBrdfLut_.imageView(),
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    for (auto const &frame : r.frames_)
      r.device_.logicalDevice().updateDescriptorSets(
          {vk::WriteDescriptorSet{.dstSet = frame.descriptorSet,
                                  .dstBinding = 4,
                                  .descriptorCount = 1,
                                  .descriptorType =
                                      vk::DescriptorType::eCombinedImageSampler,
                                  .pImageInfo = &cubeInfo},
           vk::WriteDescriptorSet{.dstSet = frame.descriptorSet,
                                  .dstBinding = 5,
                                  .descriptorCount = 1,
                                  .descriptorType =
                                      vk::DescriptorType::eCombinedImageSampler,
                                  .pImageInfo = &lutInfo}},
          {});
  }
  static TextureResources const &prefilter(Renderer const &r) {
    return r.environmentPrefilter_;
  }
  static TextureResources const &brdf(Renderer const &r) {
    return r.environmentBrdfLut_;
  }
  static HdrOutput &output(Renderer &renderer) { return *renderer.hdrOutput_; }
  static vk::Image aoImage(Renderer const &r) {
    return r.gtao_->image(Gtao::Composite);
  }
  static vk::Image temporalImage(Renderer const &r) {
    return r.taa_->colorImage(1 - r.taa_->writeIndex());
  }

  static RenderGraph::State const &hdrState(Renderer const &r) {
    return r.hdrState();
  }
  static RenderGraph::State const &depthState(Renderer const &r) {
    return r.imageStates_.depth;
  }
  static RenderGraph::State const &shadowState(Renderer const &r) {
    return r.imageStates_.shadow;
  }
  static float probeEnergy(Renderer const &r) {
    float sum = 0;
    for (auto const &v : r.probeSh_)
      sum += glm::length(v);
    return sum;
  }
  static IndoorLightingGpu indoor(Renderer const &r, unsigned slot) {
    IndoorLightingGpu g;
    r.frames_[slot].indoorBuffer.read(std::as_writable_bytes(std::span{&g, 1}));
    return g;
  }
  static vk::Image depth(Renderer const &r) {
    return *r.depthResources_.storage.image;
  }
  static vk::Image shadow(Renderer const &r) {
    return *r.shadowResources_.storage.image;
  }
  static std::pair<vk::Buffer, ClusterGrid> clusterBuffer(Renderer const &r) {
    auto slot = (r.currentFrame_ + r.framesInFlight_ - 1) % r.framesInFlight_;
    return {*r.frames_[slot].clusterIndices.buffer, r.frames_[slot].clusterGrid};
  }
  static DisplaySettings settings(Renderer const &renderer) {
    return renderer.displaySettings_;
  }
};
namespace {
void require(bool condition, char const *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void record(Device const &device,
            std::function<void(vk::CommandBuffer)> const &draw) {
  vk::raii::CommandPool pool(
      device.logicalDevice(),
      vk::CommandPoolCreateInfo{
          .flags = vk::CommandPoolCreateFlagBits::eTransient,
          .queueFamilyIndex = device.graphicsQueueFamilyIndex()});
  vk::raii::CommandBuffers commands(
      device.logicalDevice(),
      vk::CommandBufferAllocateInfo{.commandPool = *pool,
                                    .level = vk::CommandBufferLevel::ePrimary,
                                    .commandBufferCount = 1});
  auto &command = commands.front();
  command.begin(vk::CommandBufferBeginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
  draw(*command);
  command.end();
  vk::raii::Fence fence(device.logicalDevice(), vk::FenceCreateInfo{});
  vk::CommandBuffer raw = *command;
  device.graphicsQueue().submit(
      {vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &raw}},
      *fence);
  require(device.logicalDevice().waitForFences({*fence}, true, UINT64_MAX) ==
              vk::Result::eSuccess,
          "HDR regression GPU submission did not finish");
}
void barrier(vk::CommandBuffer command, vk::Image image, vk::ImageLayout before,
             vk::ImageLayout after, vk::PipelineStageFlags2 dstStage,
             vk::AccessFlags2 dstAccess,
             vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor) {
  vk::ImageMemoryBarrier2 imageBarrier{
      .srcStageMask = before == vk::ImageLayout::eUndefined
                          ? vk::PipelineStageFlags2{}
                          : vk::PipelineStageFlagBits2::eAllCommands,
      .srcAccessMask = before == vk::ImageLayout::eUndefined
                           ? vk::AccessFlags2{}
                           : vk::AccessFlagBits2::eMemoryRead |
                                 vk::AccessFlagBits2::eMemoryWrite,
      .dstStageMask = dstStage,
      .dstAccessMask = dstAccess,
      .oldLayout = before,
      .newLayout = after,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {aspect, 0, 1, 0, 1}};
  command.pipelineBarrier2(vk::DependencyInfo{
      .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &imageBarrier});
}
std::vector<std::byte>
pixel(Device const &device, vk::Image image, vk::ImageLayout layout,
      vk::Offset3D position, std::size_t bytes,
      vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor) {
  auto readback =
      device.createBuffer(bytes, vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible);
  record(device, [&](vk::CommandBuffer command) {
    barrier(command, image, layout, vk::ImageLayout::eTransferSrcOptimal,
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferRead, aspect);
    command.copyImageToBuffer(
        image, vk::ImageLayout::eTransferSrcOptimal, *readback.buffer,
        {vk::BufferImageCopy{.imageSubresource = {aspect, 0, 0, 1},
                             .imageOffset = position,
                             .imageExtent = {1, 1, 1}}});
    barrier(command, image, vk::ImageLayout::eTransferSrcOptimal, layout,
            vk::PipelineStageFlagBits2::eAllCommands,
            vk::AccessFlagBits2::eMemoryRead |
                vk::AccessFlagBits2::eMemoryWrite,
            aspect);
    vk::BufferMemoryBarrier2 host{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = *readback.buffer,
        .size = bytes};
    command.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host});
  });
  std::vector<std::byte> result(bytes);
  readback.read(result);
  return result;
}
std::array<float, 4> hdrPixel(Device const &device, HdrOutput const &output,
                              vk::Offset3D position) {
  auto data = pixel(device, output.sceneImage(),
                    vk::ImageLayout::eShaderReadOnlyOptimal, position, 8);
  std::array<std::uint16_t, 4> packed;
  std::memcpy(packed.data(), data.data(), 8);
  std::array<float, 4> result;
  for (int c = 0; c < 4; ++c)
    result[c] = glm::unpackHalf1x16(packed[c]);
  return result;
}
struct Target {
  ResourceLedger::Lease viewAccounting;
  GpuImage storage;
  vk::raii::ImageView view = nullptr;
};
Target target(Device const &device, vk::Extent2D extent, vk::Format format) {
  Target result;
  auto scope =
      device.resourceLedger().scope(ResourceLedger::Domain::Persistent);
  result.storage = device.createImage(
      vk::ImageCreateInfo{.imageType = vk::ImageType::e2D,
                          .format = format,
                          .extent = {extent.width, extent.height, 1},
                          .mipLevels = 1,
                          .arrayLayers = 1,
                          .samples = vk::SampleCountFlagBits::e1,
                          .tiling = vk::ImageTiling::eOptimal,
                          .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                   vk::ImageUsageFlagBits::eTransferSrc,
                          .sharingMode = vk::SharingMode::eExclusive},
      std::uint64_t(extent.width) * extent.height * 4,
      vk::MemoryPropertyFlagBits::eDeviceLocal, scope);
  result.view = vk::raii::ImageView(
      device.logicalDevice(),
      vk::ImageViewCreateInfo{
          .image = *result.storage.image,
          .viewType = vk::ImageViewType::e2D,
          .format = format,
          .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
  result.viewAccounting = scope.track({.imageViews = 1});
  return result;
}
float filmic(float c) {
  return std::clamp((c * (2.51f * c + .03f)) / (c * (2.43f * c + .59f) + .14f),
                    0.f, 1.f);
}
float srgb(float c) {
  return c <= .0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - .055f;
}
void checkDisplay(std::vector<std::byte> const &data,
                  std::array<float, 4> linear, DisplaySettings settings,
                  vk::Format format) {
  bool bgra = format == vk::Format::eB8G8R8A8Srgb ||
              format == vk::Format::eB8G8R8A8Unorm;
  for (unsigned channel = 0; channel < 3; ++channel) {
    float exposed =
        std::max(linear[channel], 0.f) * std::exp2(settings.exposureEv);
    float mapped =
        settings.toneMap ? filmic(exposed) : std::clamp(exposed, 0.f, 1.f);
    int expected = int(std::round(255 * srgb(mapped)));
    int actual = std::to_integer<int>(data[bgra ? 2 - channel : channel]);
    if (std::abs(actual - expected) > 2)
      std::cerr << "HDR output channel " << channel << " actual=" << actual
                << " expected=" << expected << '\n';
    require(std::abs(actual - expected) <= 2,
            "Display transfer/exposure/tone mapping differs from reference");
  }
  require(data[3] == std::byte{255},
          "Display alpha must be opaque and unencoded");
}
void displayGraph(Device const &device, HdrOutput &output,
                  Target const &destination, vk::Format format,
                  RenderGraph::State &hdrState, DisplaySettings settings,
                  std::span<std::array<float, 4> const> palette = {},
                  bool uiMarker = false) {
  using G = RenderGraph;
  G graph;
  auto hdr = graph.importImage(
      {"HDR palette", output.sceneImage(), output.sceneView(),
       HdrOutput::sceneFormat, output.extent(), vk::ImageAspectFlagBits::eColor,
       vk::ImageUsageFlagBits::eColorAttachment |
           vk::ImageUsageFlagBits::eSampled |
           vk::ImageUsageFlagBits::eTransferSrc,
       false, hdrState});
  auto color = graph.importImage({"Display", *destination.storage.image,
                                  *destination.view, format, output.extent(),
                                  vk::ImageAspectFlagBits::eColor,
                                  vk::ImageUsageFlagBits::eColorAttachment |
                                      vk::ImageUsageFlagBits::eTransferSrc});
  std::optional<G::PassId> scene, ui;
  if (!palette.empty())
    scene = graph.addPass("Palette",
                          {{hdr,
                            G::Usage::ColorAttachment,
                            vk::AttachmentLoadOp::eClear,
                            vk::AttachmentStoreOp::eStore,
                            false,
                            {.color = vk::ClearColorValue{palette[0]}}}});
  auto display =
      graph.addPass("Display", {{hdr, G::Usage::SampledColor},
                                {color, G::Usage::ColorAttachment,
                                 vk::AttachmentLoadOp::eDontCare,
                                 vk::AttachmentStoreOp::eStore, true}});
  if (uiMarker)
    ui = graph.addPass(
        "UI", {{color, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eLoad,
                vk::AttachmentStoreOp::eStore}});
  auto plan = graph.compile();
  record(device, [&](vk::CommandBuffer command) {
    plan.record(command, [&](G::Pass const &pass, G::Event event) {
      if (event != G::Event::Draw)
        return;
      if (scene && pass.id == *scene) {
        for (unsigned index = 1; index < palette.size(); ++index)
          command.clearAttachments(
              {vk::ClearAttachment{
                  .aspectMask = vk::ImageAspectFlagBits::eColor,
                  .colorAttachment = 0,
                  .clearValue = {.color =
                                     vk::ClearColorValue{palette[index]}}}},
              {vk::ClearRect{.rect = {{int(index % 2), int(index / 2)}, {1, 1}},
                             .baseArrayLayer = 0,
                             .layerCount = 1}});
      } else if (pass.id == display)
        output.drawDisplay(command, settings);
      else if (ui && pass.id == *ui)
        command.clearAttachments(
            {vk::ClearAttachment{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .colorAttachment = 0,
                .clearValue = {.color =
                                   vk::ClearColorValue{std::array<float, 4>{
                                       .25f, .5f, .75f, 1.f}}}}},
            {vk::ClearRect{.rect = {{0, 0}, {1, 1}},
                           .baseArrayLayer = 0,
                           .layerCount = 1}});
    });
  });
  hdrState = plan.finalState(hdr);
}
float depthPixel(Device const &device, vk::Image image, vk::Offset3D position) {
  auto data = pixel(device, image, vk::ImageLayout::eDepthReadOnlyOptimal,
                    position, 4, vk::ImageAspectFlagBits::eDepth);
  float result;
  std::memcpy(&result, data.data(), sizeof(result));
  return result;
}
} // namespace
void exerciseHdrOutput(Device const &device) {
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "Display regression baseline not empty");
  std::array<std::array<float, 4>, 4> colors{{{0.f, .003f, .004f, .25f},
                                              {.18f, .5f, 1.f, .5f},
                                              {2.f, 4.f, 16.f, 1.f},
                                              {.001f, .25f, 64.f, .75f}}};
  for (auto format : {vk::Format::eR8G8B8A8Srgb, vk::Format::eB8G8R8A8Srgb,
                      vk::Format::eR8G8B8A8Unorm, vk::Format::eB8G8R8A8Unorm}) {
    HdrOutput output(device, {2, 2}, format);
    RenderGraph::State hdrState;
    for (auto settings :
         {DisplaySettings{0.f, true}, DisplaySettings{2.f, true},
          DisplaySettings{-2.f, true}, DisplaySettings{0.f, false},
          DisplaySettings{1.f, false}}) {
      auto destination = target(device, output.extent(), format);
      displayGraph(device, output, destination, format, hdrState, settings,
                   colors);
      for (unsigned index = 0; index < colors.size(); ++index) {
        vk::Offset3D position{int(index % 2), int(index / 2), 0};
        auto source = hdrPixel(device, output, position);
        for (unsigned c = 0; c < 4; ++c) {
          auto bits = glm::packHalf1x16(colors[index][c]);
          auto reference = glm::unpackHalf1x16(bits);
          auto lower = glm::unpackHalf1x16(bits ? bits - 1 : 0);
          auto upper = glm::unpackHalf1x16(bits + 1);
          auto tolerance = std::max(reference - lower, upper - reference);
          if (std::abs(source[c] - reference) > tolerance)
            std::cerr << "HDR palette pixel=" << index << " channel=" << c
                      << " source=" << source[c]
                      << " input=" << colors[index][c]
                      << " half reference=" << reference << '\n';
          require(std::abs(source[c] - reference) <= tolerance,
                  "HDR palette exceeds one FP16 ULP or flipped");
        }
        checkDisplay(pixel(device, *destination.storage.image,
                           vk::ImageLayout::eColorAttachmentOptimal, position,
                           4),
                     source, settings, format);
      }
    }
    // Display-referred UI marker, recorded after the common output, must retain
    // its value for different scene exposures.
    for (float ev : {-4.f, 4.f}) {
      auto destination = target(device, output.extent(), format);
      displayGraph(device, output, destination, format, hdrState, {ev, true},
                   {}, true);
      // The UI LOAD/STORE pass must keep untouched output pixels.
      checkDisplay(pixel(device, *destination.storage.image,
                         vk::ImageLayout::eColorAttachmentOptimal, {1, 1, 0},
                         4),
                   hdrPixel(device, output, {1, 1, 0}), {ev, true}, format);
      auto data = pixel(device, *destination.storage.image,
                        vk::ImageLayout::eColorAttachmentOptimal, {0, 0, 0}, 4);
      // Clear on sRGB encodes linear input; UNORM stores clear input verbatim.
      auto marker = std::array<float, 4>{.25f, .5f, .75f, 1.f};
      if (!displayUsesHardwareSrgb(format))
        for (unsigned c = 0; c < 3; ++c)
          marker[c] = marker[c] <= .04045f
                          ? marker[c] / 12.92f
                          : std::pow((marker[c] + .055f) / 1.055f, 2.4f);
      checkDisplay(data, marker, {0, false}, format);
    }
  }
  for (auto settings :
       {DisplaySettings{std::numeric_limits<float>::quiet_NaN(), true},
        DisplaySettings{17.f, true}}) {
    bool rejected = false;
    try {
      validateDisplaySettings(settings);
    } catch (std::runtime_error const &) {
      rejected = true;
    }
    require(rejected, "Invalid display EV accepted");
  }
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "Display resource destruction did not return to zero");
  std::cout << "PASS display shader: 4 sRGB/UNORM RGBA/BGRA formats, 5 "
               "exposure/tone variants, palette orientation/half precision, UI "
               "after output, zero release\n";
}
void exerciseHdrScene(Device const &device, SwapChain const &swapchain,
                      unsigned framesInFlight) {
  {
    Renderer renderer(device, framesInFlight);
    renderer.recreateForSwapChain(swapchain);
    AssetLibrary assets;
    Mesh triangle;
    for (auto position :
         {glm::vec3{-1, -1, .5f}, glm::vec3{3, -1, .5f}, glm::vec3{-1, 3, .5f}})
      triangle.vertices.push_back(Vertex{.position = position,
                                         .color = {1, 1, 1},
                                         .normal = {0, 0, 1},
                                         .uv = {0, 0}});
    triangle.indices = {0, 1, 2};
    assets.meshes.push_back(triangle);
    Material back;
    back.name = "HDR opaque";
    back.emissiveFactor = {1, 2, 3};
    back.metallicFactor = .2f;
    back.doubleSided = true;
    Material front;
    front.name = "HDR blend";
    front.emissiveFactor = {4, 1, .25f};
    front.metallicFactor = .8f;
    front.alphaMode = AlphaMode::Blend;
    front.tint.a = .5f;
    front.doubleSided = true;
    assets.materials = {back, front};
    auto candidate = renderer.prepareScene(assets);
    renderer.waitSceneUpload(candidate);
    require(renderer.commitScene(candidate), "HDR test scene failed to commit");
    auto &output = RendererHdrTestAccess::output(renderer);
    vk::Offset3D center{int(swapchain.extent().width / 2),
                        int(swapchain.extent().height / 2), 0};
    unsigned uiCalls = 0;
    renderer.setUiDrawCallback([&](vk::CommandBuffer) {
      require(
          RendererHdrTestAccess::hdrState(renderer).layout ==
              vk::ImageLayout::eShaderReadOnlyOptimal,
          "Renderer UI was invoked while scene HDR rendering was still active");
      ++uiCalls;
    });
    LightingSettings lighting;
    lighting.intensity = 0;
    lighting.environmentIntensity = 0;
    lighting.shadowDebugMode = 0;
    for (int iteration = 0; iteration < 3; ++iteration) {
      lighting.exposureEv = iteration == 0 ? 0 : 2;
      lighting.pbrDebugMode = iteration == 2 ? 2 : 0;
      std::array opaque{Renderer::DrawItem{.meshId = 0, .materialId = 0}};
      std::array blend{Renderer::DrawItem{
          .meshId = 0,
          .materialId = 1,
          .modelMatrix = glm::translate(glm::mat4(1), {0.f, 0.f, -.25f})}};
      require(renderer.renderFrame({.opaque = opaque,
                                    .transparent = blend,
                                    .allOpaque = opaque,
                                    .sky = false},
                                   glm::mat4(1), {0, 0, 2}, lighting,
                                   false) == Renderer::FrameResult::eSuccess,
              "HDR scene frame failed");
      device.logicalDevice().waitIdle();
      renderer.collectCompletedWork();
      auto const &depthState = RendererHdrTestAccess::depthState(renderer);
      require(depthState.defined &&
                  depthState.layout == vk::ImageLayout::eDepthReadOnlyOptimal &&
                  depthPixel(device, RendererHdrTestAccess::depth(renderer),
                             center) == .5f,
              "Main depth was discarded, not exported read-only, or "
              "overwritten by alpha blend");
      if (iteration == 0) {
        require(depthPixel(device, RendererHdrTestAccess::shadow(renderer),
                           {0, 0, 0}) == 1.f,
                "Initially disabled shadow descriptor did not receive defined "
                "far depth");
        require(renderer.renderGraphDump().find("Shadow initialization") !=
                    std::string::npos,
                "Missing first-frame disabled shadow initialization");
      } else
        require(renderer.renderGraphDump().find("Shadow initialization") ==
                    std::string::npos,
                "Disabled shadow initialization repeated after contents became "
                "defined");
      auto source = hdrPixel(device, output, center);
      auto expected = iteration == 2
                          ? std::array<float, 4>{.5f, .5f, .5f, 1.f}
                          : std::array<float, 4>{2.5f, 1.5f, 1.625f, 1.f};
      for (unsigned c = 0; c < 4; ++c)
        require(std::abs(source[c] - expected[c]) < .002f,
                "Scene HDR clipped radiance, exposed early, tone mapped before "
                "blending or corrupted alpha");
      auto settings = RendererHdrTestAccess::settings(renderer);
      require(iteration != 2 || (!settings.toneMap && settings.exposureEv == 0),
              "Data debug did not bypass display curve/exposure");
      auto destination =
          target(device, output.extent(), swapchain.imageFormat());
      auto hdrState = RendererHdrTestAccess::hdrState(renderer);
      displayGraph(device, output, destination, swapchain.imageFormat(),
                   hdrState, settings);
      checkDisplay(pixel(device, *destination.storage.image,
                         vk::ImageLayout::eColorAttachmentOptimal, center, 4),
                   expected, settings, swapchain.imageFormat());
    }
    std::array<float, 4> firstSky{};
    for (int iteration = 0; iteration < 3; ++iteration) {
      lighting.exposureEv = iteration == 1 ? 2 : 0;
      lighting.environmentIntensity = iteration == 2 ? 2 : 1;
      lighting.pbrDebugMode = 0;
      require(renderer.beginFrame(glm::mat4(1), {0, 0, 2}, lighting, false) ==
                  Renderer::FrameResult::eSuccess,
              "Sky HDR frame failed");
      renderer.drawEnvironment();
      renderer.endFrame();
      device.logicalDevice().waitIdle();
      renderer.collectCompletedWork();
      auto sky = hdrPixel(device, output, center);
      if (iteration == 0)
        firstSky = sky;
      require(firstSky[0] + firstSky[1] + firstSky[2] > .01f,
              "Sky fixture has no radiance");
      for (unsigned c = 0; c < 3; ++c) {
        auto expectedSky = firstSky[c] * (iteration == 2 ? 2 : 1);
        require(std::abs(sky[c] - expectedSky) <
                    std::max(.002f, .002f * expectedSky),
                "Sky was tone mapped/exposed early or environment intensity is "
                "nonlinear");
      }
    }
    // Display settings are validated before acquisition/fence reset; retry must
    // remain usable, and a nested begin must not mutate the active settings.
    lighting.exposureEv = std::numeric_limits<float>::quiet_NaN();
    bool invalidRejected = false;
    try {
      renderer.beginFrame(glm::mat4(1), {0, 0, 2}, lighting, false);
    } catch (std::runtime_error const &) {
      invalidRejected = true;
    }
    require(invalidRejected, "Renderer accepted nonfinite EV");
    lighting.exposureEv = 0;
    require(renderer.beginFrame(glm::mat4(1), {0, 0, 2}, lighting, false) ==
                Renderer::FrameResult::eSuccess,
            "Valid frame after rejected EV failed");
    bool callbackRejected = false;
    try {
      renderer.setUiDrawCallback({});
    } catch (std::runtime_error const &) {
      callbackRejected = true;
    }
    require(callbackRejected, "Active graph allowed its UI contract to change");
    lighting.exposureEv = 3;
    bool nestedRejected = false;
    try {
      renderer.beginFrame(glm::mat4(1), {0, 0, 2}, lighting, false);
    } catch (std::runtime_error const &) {
      nestedRejected = true;
    }
    require(nestedRejected &&
                RendererHdrTestAccess::settings(renderer).exposureEv == 0,
            "Rejected nested frame changed active display settings");
    renderer.drawEnvironment();
    renderer.endFrame();
    device.logicalDevice().waitIdle();
    renderer.collectCompletedWork();
    // Fault after HDR target/view/descriptors exist: failed replacement must
    // reclaim candidates and preserve live resources. Use a private shader
    // directory so another viewer/test never loses its output shader.
    auto live = renderer.resourceSnapshot();
    auto liveImage = output.sceneImage();
    auto liveDepthState = RendererHdrTestAccess::depthState(renderer);
    auto liveHdrState = RendererHdrTestAccess::hdrState(renderer);
    bool resizeRejected = false;
    {
      auto original = std::filesystem::current_path();
      auto temporary =
          std::filesystem::temp_directory_path() /
          ("vulkan-hdr-output-failure-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
      struct Restore {
        std::filesystem::path original, temporary;
        ~Restore() {
          std::error_code error;
          std::filesystem::current_path(original, error);
          std::filesystem::remove_all(temporary, error);
        }
      } restore{original, temporary};
      std::filesystem::create_directories(temporary / "shaders");
      for (auto const &entry :
           std::filesystem::directory_iterator(original / "shaders"))
        if (entry.path().extension() == ".spv" &&
            entry.path().filename() != "display.frag.spv")
          std::filesystem::copy_file(entry.path(), temporary / "shaders" /
                                                       entry.path().filename());
      std::filesystem::current_path(temporary);
      try {
        renderer.recreateForSwapChain(swapchain);
      } catch (std::runtime_error const &) {
        resizeRejected = true;
      }
    }
    require(resizeRejected &&
                RendererHdrTestAccess::output(renderer).sceneImage() ==
                    liveImage &&
                RendererHdrTestAccess::depthState(renderer) == liveDepthState &&
                RendererHdrTestAccess::hdrState(renderer) == liveHdrState,
            "Failed HDR output replacement consumed live resources");
    auto failed = renderer.resourceSnapshot();
    for (std::size_t i = 0; i < ResourceLedger::domainCount; ++i)
      if (i != std::size_t(ResourceLedger::Domain::AllocatorBlocks))
        require(failed.domains[i] == live.domains[i],
                "HDR output construction failure leaked resources/ranges");
    lighting.exposureEv = 0;
    require(renderer.beginFrame(glm::mat4(1), {0, 0, 2}, lighting, false) ==
                Renderer::FrameResult::eSuccess,
            "Drawing preserved HDR resources after failed replacement failed");
    renderer.drawEnvironment();
    renderer.endFrame();
    device.logicalDevice().waitIdle();
    renderer.collectCompletedWork();
    std::cout << "FRAME_GRAPH_WITH_UI_BEGIN\n"
              << renderer.renderGraphDump() << "FRAME_GRAPH_WITH_UI_END\n";
    renderer.setUiDrawCallback({});
    require(uiCalls == 8 && renderer.gpuTimings().valid &&
                renderer.gpuTimings().outputMs > 0,
            "HDR output/UI scheduling or separate timestamp query missing");
    std::array opaque{Renderer::DrawItem{.meshId = 0, .materialId = 0}};
    std::array casters{opaque[0],
                       Renderer::DrawItem{.meshId = 0,
                                          .materialId = 0,
                                          .modelMatrix = glm::translate(
                                              glm::mat4(1), {10.f, 0.f, 0.f})}};
    std::array blend{Renderer::DrawItem{
        .meshId = 0,
        .materialId = 1,
        .modelMatrix = glm::translate(glm::mat4(1), {0.f, 0.f, -.25f})}};
    lighting.environmentIntensity = 0;
    lighting.intensity = 0;
    auto frameId = renderer.submittedFrameId();
    bool casterRejected = false;
    try {
      renderer.renderFrame({.allOpaque = blend, .sky = false}, glm::mat4(1),
                           {0, 0, 2}, lighting, true);
    } catch (std::runtime_error const &) {
      casterRejected = true;
    }
    require(casterRejected && renderer.submittedFrameId() == frameId,
            "Invalid caster consumed a frame before validation");
    for (bool shadows : {false, true, false, true}) {
      lighting.shadowDebugMode = shadows ? 1 : 3;
      lighting.exposureEv = 2;
      require(renderer.renderFrame({.opaque = opaque,
                                    .transparent = blend,
                                    .allOpaque = casters,
                                    .sky = false},
                                   glm::mat4(1), {0, 0, 2}, lighting,
                                   shadows) == Renderer::FrameResult::eSuccess,
              "Frame after invalid caster or shadow toggle failed");
      device.logicalDevice().waitIdle();
      renderer.collectCompletedWork();
      auto const &draws = renderer.drawStatistics();
      require(draws.main == 2 && draws.shadow == (shadows ? 2u : 0u),
              "Graph lost blend/main draws or off-camera shadow casters");
      require(RendererHdrTestAccess::shadowState(renderer).defined &&
                  RendererHdrTestAccess::shadowState(renderer).layout ==
                      vk::ImageLayout::eDepthReadOnlyOptimal &&
                  depthPixel(device, RendererHdrTestAccess::depth(renderer),
                             center) == .5f,
              "Shadow toggle or scene depth export failed");
      if (!shadows) {
        auto radiance = hdrPixel(device, output, center);
        require(
            std::abs(radiance[0] - 2.5f) < .002f &&
                RendererHdrTestAccess::settings(renderer).exposureEv == 2 &&
                RendererHdrTestAccess::settings(renderer).toneMap,
            "Disabled shadow debug leaked into shading or display settings");
        require(renderer.gpuTimings().shadowMs == 0,
                "Disabled initialized shadow frame has nonzero shadow timing");
      }
    }
    std::cout << "FRAME_GRAPH_BEGIN\n"
              << renderer.renderGraphDump() << "FRAME_GRAPH_END\n";
    auto shadowState = RendererHdrTestAccess::shadowState(renderer);
    renderer.recreateForSwapChain(swapchain);
    require(!RendererHdrTestAccess::depthState(renderer).defined &&
                !RendererHdrTestAccess::hdrState(renderer).defined &&
                RendererHdrTestAccess::shadowState(renderer) == shadowState,
            "Recreation retained new-target state or discarded persistent "
            "shadow state");
    lighting.shadowDebugMode = 0;
    require(renderer.renderFrame({.opaque = opaque, .sky = false}, glm::mat4(1),
                                 {0, 0, 2}, lighting,
                                 false) == Renderer::FrameResult::eSuccess,
            "Recreated graph frame failed");
    device.logicalDevice().waitIdle();
    require(depthPixel(device, RendererHdrTestAccess::depth(renderer),
                       center) == .5f &&
                renderer.renderGraphDump().find("Shadow initialization") ==
                    std::string::npos,
            "Recreated target depth or persistent shadow state mismatch");
  }
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "HDR scene shutdown leaked resources");
  std::cout << "PASS HDR scene: actual opaque/alpha-blend emissive >1, linear "
               "compositing, global exposure after blending, sky intensity, "
               "debug bypass, rejected EV/nested frame, failed replacement, "
               "graph UI LOAD preservation, sampled depth, initial disabled "
               "shadow, toggles, "
               "off-camera casters, graph recreation, output timestamps, zero "
               "shutdown\n";
}

namespace {
std::vector<std::byte> materialTga(unsigned width, unsigned height,
                                   std::span<unsigned char const> rgba) {
  std::vector<std::byte> bytes(18, std::byte{0});
  bytes[2] = std::byte{2};
  bytes[12] = std::byte(width & 255);
  bytes[13] = std::byte(width >> 8);
  bytes[14] = std::byte(height & 255);
  bytes[15] = std::byte(height >> 8);
  bytes[16] = std::byte{32};
  bytes[17] = std::byte{0x28};
  for (unsigned i = 0; i < width * height; ++i)
    for (auto c : {2, 1, 0, 3})
      bytes.push_back(std::byte(rgba[i * 4 + c]));
  return bytes;
}
std::vector<float> hdrImageSource(Device const &device, vk::Image source,
                                  vk::Extent2D extent) {
  std::vector<std::uint16_t> packed(std::size_t(extent.width) * extent.height * 4);
  auto readback = device.createBuffer(packed.size() * 2, vk::BufferUsageFlagBits::eTransferDst,
                                      vk::MemoryPropertyFlagBits::eHostVisible);
  record(device, [&](vk::CommandBuffer command) {
    barrier(command, source, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::ImageLayout::eTransferSrcOptimal,
            vk::PipelineStageFlagBits2::eCopy,
            vk::AccessFlagBits2::eTransferRead);
    command.copyImageToBuffer(
        source, vk::ImageLayout::eTransferSrcOptimal, *readback.buffer,
        {vk::BufferImageCopy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {extent.width, extent.height, 1}}});
    vk::BufferMemoryBarrier2 b{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite, .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = *readback.buffer, .size = VK_WHOLE_SIZE};
    command.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b});
    barrier(command, source, vk::ImageLayout::eTransferSrcOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eShaderSampledRead);
  });
  readback.read(std::as_writable_bytes(std::span{packed}));
  std::vector<float> pixels(packed.size());
  std::transform(packed.begin(), packed.end(), pixels.begin(), [](auto v) { return glm::unpackHalf1x16(v); });
  return pixels;
}
std::vector<float> hdrImage(Device const &device, HdrOutput const &output) {
  return hdrImageSource(device, output.sceneImage(), output.extent());
}
std::vector<std::uint32_t> clusterWords(Device const &device, Renderer const &r) {
  auto [source, grid] = RendererHdrTestAccess::clusterBuffer(r);
  require(grid.screen.z == 1, "Cluster test accidentally used full fallback");
  auto bytes = clusterListBytes(grid);
  auto readback = device.createBuffer(bytes, vk::BufferUsageFlagBits::eTransferDst, vk::MemoryPropertyFlagBits::eHostVisible);
  record(device, [&](vk::CommandBuffer command) {
    vk::BufferMemoryBarrier2 before{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite, .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = source, .size = bytes};
    command.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &before});
    command.copyBuffer(source, *readback.buffer, {vk::BufferCopy{.size = bytes}});
    vk::BufferMemoryBarrier2 after{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite, .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = *readback.buffer, .size = bytes};
    command.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &after});
  });
  std::vector<std::uint32_t> words(bytes / 4);
  readback.read(std::as_writable_bytes(std::span{words}));
  return words;
}
Mesh materialTriangle() {
  Mesh mesh;
  for (auto position :
       {glm::vec3(-1, -1, .5f), glm::vec3(3, -1, .5f), glm::vec3(-1, 3, .5f)})
    mesh.vertices.push_back(Vertex{.position = position,
                                   .color = {1, 1, 1},
                                   .normal = {0, 0, 1},
                                   .uv = {.0625f, .0625f}});
  mesh.indices = {0, 1, 2};
  return mesh;
}
} // namespace
void exerciseMaterialContract(Device const &device, SwapChain const &swapchain,
                              unsigned framesInFlight) {
  {
    Renderer renderer(device, framesInFlight);
    renderer.recreateForSwapChain(swapchain);
    AssetLibrary assets;
    auto triangle = materialTriangle();
    assets.meshes.push_back(triangle); // 0: constant source UV.
    // Keep source nearest samples inside texels, not exactly on driver rounding ties.
    auto minified = triangle;
    for (auto &v : minified.vertices)
      v.uv = {(v.position.x + 1) * swapchain.extent().width / 8.f + .01f,
              (v.position.y + 1) * swapchain.extent().height / 8.f + .01f};
    assets.meshes.push_back(minified); // 1: camera texel-center LOD 1.
    auto shadowMinified = triangle;
    for (auto &v : shadowMinified.vertices)
      v.uv = {(v.position.x + 1) * 256 + .01f, (-v.position.y + 1) * 256 + .01f};
    assets.meshes.push_back(shadowMinified); // 2: light texel-center LOD 1.
    auto vertexAlpha = triangle;
    for (auto &v : vertexAlpha.vertices)
      v.alpha = .25f;
    assets.meshes.push_back(vertexAlpha); // 3.
    auto normalMesh = triangle;
    for (auto &v : normalMesh.vertices) {
      v.normal = glm::normalize(glm::vec3(1, 1, 1));
      v.tangent = {glm::normalize(glm::vec3(1, -1, 0)), 1};
    }
    assets.meshes.push_back(normalMesh); // 4.
    auto back = normalMesh;
    back.indices = {0, 2, 1};
    assets.meshes.push_back(back); // 5.
    auto degenerate = triangle;
    for (auto &v : degenerate.vertices) {
      v.normal = {1, 0, 0};
      v.tangent = {1, 0, 0, 1};
    }
    assets.meshes.push_back(degenerate); // 6.
    auto mirroredUv = triangle;
    for (auto &v : mirroredUv.vertices)
      v.normalUv = {v.position.x, -v.position.y};
    generateMeshTangents(mirroredUv, true);
    assets.meshes.push_back(mirroredUv); // 7: actual Mikk mirrored normal UV.
    auto alphaMinified = minified;
    for (auto &v : alphaMinified.vertices)
      v.alpha = .9f;
    assets.meshes.push_back(
        alphaMinified); // 8: non-unit alpha disables baked mips.

    std::array<unsigned char, 4> translucent{255, 255, 255, 204};
    Material fixed;
    fixed.alphaMode = AlphaMode::Mask;
    fixed.doubleSided = true;
    fixed.albedoBytes = materialTga(1, 1, translucent); // 0: .8 source alpha.
    Material obj = fixed;
    obj.albedoBytes.clear();
    auto opacityPath =
        std::filesystem::temp_directory_path() /
        ("vulkan-material-opacity-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".tga");
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code e;
        std::filesystem::remove(path, e);
      }
    } cleanup{opacityPath};
    std::array<unsigned char, 4> opacity{0, 255, 255, 255};
    auto opacityBytes = materialTga(1, 1, opacity);
    {
      std::ofstream out(opacityPath, std::ios::binary);
      out.write(reinterpret_cast<char const *>(opacityBytes.data()),
                opacityBytes.size());
      require(bool(out), "Opacity fixture write failed");
    }
    obj.alphaPath =
        opacityPath.string(); // 1: R=0, A=1; old max(RGBA) accepts it.
    Material white = obj;
    white.alphaPath.clear(); // 2: vertex alpha.
    std::array<unsigned char, 256> mask{};
    for (unsigned y = 0; y < 8; ++y)
      for (unsigned x = 0; x < 8; ++x) {
        unsigned i = (y * 8 + x) * 4, block = (y / 2) * 4 + x / 2;
        mask[i] = mask[i + 1] = mask[i + 2] = 255;
        mask[i + 3] = x % 2 ? block * 2 : 160 + block;
      }
    Material covered = white;
    covered.albedoBytes = materialTga(8, 8, mask);
    covered.albedoSampler = {.mag = TextureFilter::Nearest,
                             .min = TextureFilter::Nearest,
                             .mip = TextureMipFilter::Nearest,
                             .maxAnisotropy = 1}; // 3.
    Material compound = fixed;
    compound.alphaPath = obj.alphaPath; // 4: two sources, no baked coverage.
    Material normal = white;
    normal.alphaMode = AlphaMode::Opaque;
    std::array<unsigned char, 4> tangentPixel{204, 230, 242, 255};
    normal.normalBytes = materialTga(1, 1, tangentPixel); // 5.
    Material single = normal;
    single.doubleSided = false; // 6: reflected single-sided culling.
    Material blend = white;
    blend.alphaMode = AlphaMode::Blend; // 7: vertex alpha in blending.
    blend.doubleSided = false;
    Material parallax = covered;
    parallax.heightPath = obj.alphaPath;
    parallax.parallaxScale = 0; // 8: editable POM coverage fallback.
    assets.materials = {fixed,  obj,    white, covered, compound,
                        normal, single, blend, parallax};
    auto candidate = renderer.prepareScene(assets);
    renderer.waitSceneUpload(candidate);
    require(renderer.commitScene(candidate),
            "Material contract scene failed to commit");
    auto stable = renderer.resourceStatistics();
    LightingSettings lighting;
    lighting.direction = {0, 0, 1};
    lighting.intensity = 0;
    lighting.environmentIntensity = 0;
    lighting.shadowOrthoExtent = 1;
    lighting.pbrDebugMode = 1;
    vk::Offset3D center{int(swapchain.extent().width / 2),
                        int(swapchain.extent().height / 2), 0};
    auto finish = [&] {
      device.logicalDevice().waitIdle();
      renderer.collectCompletedWork();
    };
    auto draw = [&](MeshId mesh, MaterialId material,
                    glm::mat4 model = glm::mat4(1),
                    glm::mat4 view = glm::mat4(1), bool shadows = true) {
      require(renderer.beginFrame(view, {0, 0, 2}, lighting, shadows) ==
                  Renderer::FrameResult::eSuccess,
              "Material test begin frame failed");
      renderer.drawObject(mesh, material, model);
      renderer.endFrame();
      finish();
    };
    auto checkMask = [&](MeshId mesh, MaterialId material, bool accepts) {
      draw(mesh, material);
      auto main =
          depthPixel(device, RendererHdrTestAccess::depth(renderer), center);
      auto shadow = depthPixel(device, RendererHdrTestAccess::shadow(renderer),
                               {1024, 1024, 0});
      require(accepts ? main < .99f && shadow < .99f : main == 1 && shadow == 1,
              "Main/shadow opacity semantics disagree with expected discard");
    };
    checkMask(0, 0, true);
    renderer.setMaterialAlphaParams(0, AlphaMode::Mask, .9f);
    checkMask(0, 0, false);
    renderer.setMaterialAlphaParams(0, AlphaMode::Mask, .5f);
    checkMask(0, 0, true);
    renderer.setMaterialTint(0, {1, 1, 1, .5f});
    checkMask(0, 0, false);
    renderer.setMaterialTint(0, {1, 1, 1, 1});
    checkMask(0, 0, true);
    checkMask(0, 1, false); // Independent opacity is linear R.
    checkMask(3, 2, false); // Vertex alpha participates in both passes.
    renderer.setMaterialAlphaParams(2, AlphaMode::Opaque, .5f);
    checkMask(3, 2, true);
    renderer.setMaterialAlphaParams(2, AlphaMode::Mask, .5f);
    checkMask(3, 2, false);
    checkMask(0, 4, false); // Compound opacity source uses its original level.

    auto countDepth = [&](vk::Image image, vk::Offset3D origin) {
      unsigned count = 0;
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
          count +=
              depthPixel(device, image, {origin.x + x, origin.y + y, 0}) < .99f;
      return count;
    };
    draw(1, 3);
    require(countDepth(RendererHdrTestAccess::depth(renderer), {0, 0, 0}) == 8,
            "Production MASK failed to preserve 8/16 camera LOD 1 coverage");
    draw(2, 3);
    require(countDepth(RendererHdrTestAccess::shadow(renderer),
                       {1024, 1024, 0}) == 8,
            "Production MASK failed to preserve 8/16 light LOD 1 coverage");
    renderer.setMaterialAlphaParams(3, AlphaMode::Mask, .4f);
    draw(1, 3);
    require(
        countDepth(RendererHdrTestAccess::depth(renderer), {0, 0, 0}) == 0,
        "Edited cutoff sampled stale baked coverage instead of source alpha");
    renderer.setMaterialAlphaParams(3, AlphaMode::Mask, .5f);
    draw(1, 3);
    require(countDepth(RendererHdrTestAccess::depth(renderer), {0, 0, 0}) == 8,
            "Restored cutoff did not restore prebound coverage chain");
    renderer.setMaterialTint(3, {1, 1, 1, .9f});
    draw(1, 3);
    require(countDepth(RendererHdrTestAccess::depth(renderer), {0, 0, 0}) == 0,
            "Edited alpha factor sampled stale coverage");
    renderer.setMaterialTint(3, {1, 1, 1, 1});
    draw(8, 3);
    require(countDepth(RendererHdrTestAccess::depth(renderer), {0, 0, 0}) == 0,
            "Non-unit vertex alpha failed to select source-level fallback");
    draw(1, 8);
    require(countDepth(RendererHdrTestAccess::depth(renderer), {0, 0, 0}) == 8,
            "Inactive parallax unnecessarily disabled coverage");
    renderer.setMaterialSurfaceParams(8, 1, .01f);
    draw(1, 8);
    require(countDepth(RendererHdrTestAccess::depth(renderer), {0, 0, 0}) == 0,
            "Active parallax sampled baked coverage instead of source");

    lighting.pbrDebugMode = 4;
    glm::mat4 view(1);
    view[1][1] = -1; // Same Vulkan Y convention as the viewer.
    auto checkNormal = [&](MeshId mesh, MaterialId material, glm::mat4 model,
                           bool backFace, float scale) {
      renderer.setMaterialSurfaceParams(material, scale, 0);
      draw(mesh, material, model, view, false);
      require(depthPixel(device, RendererHdrTestAccess::depth(renderer),
                         center) < .99f,
              "Reflected single-sided/front/back normal fixture vanished");
      auto const &v = assets.meshes[mesh].vertices[0];
      glm::vec3 N = glm::normalize(
          glm::transpose(glm::inverse(glm::mat3(model))) * v.normal);
      glm::vec3 T = glm::mat3(model) * glm::vec3(v.tangent);
      T -= N * glm::dot(N, T);
      if (glm::dot(T, T) < 1e-6f)
        T = glm::cross(
            std::abs(N.y) < .999f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0), N);
      T = glm::normalize(T);
      glm::vec3 B = glm::cross(N, T) * v.tangent.w *
                    (glm::determinant(glm::mat3(model)) < 0 ? -1.f : 1.f);
      glm::vec3 tangentNormal = glm::vec3(204, 230, 242) / 255.f * 2.f - 1.f;
      tangentNormal.x *= scale;
      tangentNormal.y *= scale;
      auto expected =
          glm::normalize(glm::mat3(T, B, N) * glm::normalize(tangentNormal)) *
          (backFace ? -1.f : 1.f);
      auto value =
          hdrPixel(device, RendererHdrTestAccess::output(renderer), center);
      glm::vec3 actual = glm::vec3(value[0], value[1], value[2]) * 2.f - 1.f;
      if (glm::length(actual - expected) >= .02f) {
        std::cerr << "Normal mismatch mesh=" << mesh << " actual=" << actual.x
                  << "," << actual.y << "," << actual.z
                  << " expected=" << expected.x << "," << expected.y << ","
                  << expected.z << '\n';
      }
      require(glm::length(actual - expected) < .02f &&
                  std::abs(glm::length(actual) - 1) < .015f,
              "Production tangent normal failed "
              "transform/handedness/backface/scale/fallback contract");
    };
    auto positive = glm::scale(glm::mat4(1), {2.f, .5f, 1.5f});
    auto negative = glm::scale(glm::mat4(1), {-2.f, .5f, 1.5f});
    checkNormal(4, 5, positive, false, .5f);
    checkNormal(4, 5, negative, false, .5f);
    checkNormal(4, 6, negative, false, .5f);
    draw(0, 6, negative, view, true);
    require(depthPixel(device, RendererHdrTestAccess::shadow(renderer),
                       {1024, 1024, 0}) < .99f,
            "Reflected single-sided shadow caster was culled");
    checkNormal(5, 5, positive, true, .5f);
    checkNormal(4, 5, positive, false, 0);
    checkNormal(6, 5, glm::mat4(1), false, 1);
    checkNormal(7, 5, glm::mat4(1), false, 1);

    lighting.pbrDebugMode = 1;
    require(renderer.beginFrame(glm::mat4(1), {0, 0, 2}, lighting, false) ==
                Renderer::FrameResult::eSuccess,
            "Blend background begin failed");
    renderer.endFrame();
    finish();
    auto background =
        hdrPixel(device, RendererHdrTestAccess::output(renderer), center);
    draw(3, 7, negative, view, false);
    auto blended =
        hdrPixel(device, RendererHdrTestAccess::output(renderer), center);
    for (unsigned c = 0; c < 3; ++c)
      require(std::abs(blended[c] - (.25f + .75f * background[c])) < .002f,
              "BLEND ignored vertex alpha or applied MASK coverage");
    renderer.setMaterialAlphaParams(3, AlphaMode::Blend, .5f);
    draw(1, 3, glm::mat4(1), glm::mat4(1), false);
    auto editedBlend =
        hdrPixel(device, RendererHdrTestAccess::output(renderer), {0, 0, 0});
    for (unsigned c = 0; c < 3; ++c)
      require(std::abs(editedBlend[c] - background[c]) < .002f,
              "MASK to BLEND reused scaled coverage alpha instead of source");
    renderer.setMaterialAlphaParams(3, AlphaMode::Mask, .5f);
    draw(1, 3);
    require(countDepth(RendererHdrTestAccess::depth(renderer), {0, 0, 0}) == 8,
            "Restoring MASK did not restore coverage binding");
    require(renderer.resourceStatistics().sceneImageCopies ==
                    stable.sceneImageCopies &&
                renderer.resourceStatistics().pipelineBuilds ==
                    stable.pipelineBuilds,
            "Material edits rebuilt images or pipelines");
    std::cout << "PASS production material: main/shadow R/vertex/discard, "
                 "camera/light LOD1 coverage=8/16, edits/restoration, "
                 "nonuniform/reflected/single/back/Mikk/scale0/fallback normal "
                 "pixels, blend\n";
  }
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "Material views/descriptors/tangent geometry leaked");
}

void exerciseEnvironmentIbl(Device const &device, SwapChain const &swapchain,
                            unsigned framesInFlight) {
  auto before = device.resourceLedger().snapshot().current;
  {
    Renderer renderer(device, framesInFlight);
    renderer.recreateForSwapChain(swapchain);
    HdrImage constant{
        .width = 8, .height = 4, .rgba = std::vector<float>(8 * 4 * 4)};
    for (std::size_t p = 0; p < constant.rgba.size(); p += 4) {
      constant.rgba[p] = 4;
      constant.rgba[p + 1] = 1;
      constant.rgba[p + 2] = 12;
      constant.rgba[p + 3] = 1;
    }
    auto bake = bakeEnvironment(constant, {.faceSize = 8,
                                           .prefilterSamples = 128,
                                           .lutSize = 32,
                                           .lutSamples = 1024});
    auto tagged = bake;
    for (unsigned mip = 0; mip < tagged.levels.size(); ++mip) {
      auto level = tagged.levels[mip];
      for (unsigned face = 0; face < 6; ++face)
        for (unsigned y = 0; y < level.size; ++y)
          for (unsigned x = 0; x < level.size; ++x) {
            auto p =
                level.offset +
                ((std::size_t(face) * level.size + y) * level.size + x) * 4;
            tagged.cubeRgba[p] = 2 + face + 2 * mip;
            tagged.cubeRgba[p + 1] = 4 + mip;
            tagged.cubeRgba[p + 2] = 8 + face;
          }
    }
    for (std::size_t p = 0; p < tagged.brdfLut.rgba.size(); p += 4) {
      tagged.brdfLut.rgba[p] = .25f;
      tagged.brdfLut.rgba[p + 1] = .125f;
    }
    RendererHdrTestAccess::environment(renderer, tagged);
    // Read every uploaded texel, including all six layers and terminal levels.
    auto readImage = [&](TextureResources const &texture, unsigned layers,
                         std::span<float const> expected) {
      auto readback = device.createBuffer(
          expected.size_bytes(), vk::BufferUsageFlagBits::eTransferDst,
          vk::MemoryPropertyFlagBits::eHostVisible);
      record(device, [&](vk::CommandBuffer command) {
        vk::ImageMemoryBarrier2 image{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead |
                             vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture.image(),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0,
                                 texture.mipLevels(), 0, layers}};
        command.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &image});
        std::vector<vk::BufferImageCopy> copies;
        if (layers == 6)
          for (unsigned mip = 0; mip < tagged.levels.size(); ++mip) {
            auto level = tagged.levels[mip];
            copies.push_back(vk::BufferImageCopy{
                .bufferOffset = level.offset * sizeof(float),
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0,
                                     6},
                .imageExtent = {level.size, level.size, 1}});
          }
        else
          copies.push_back(vk::BufferImageCopy{
              .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
              .imageExtent = {tagged.brdfLut.width, tagged.brdfLut.height, 1}});
        command.copyImageToBuffer(texture.image(),
                                  vk::ImageLayout::eTransferSrcOptimal,
                                  *readback.buffer, copies);
        image.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        image.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
        image.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
        image.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
        image.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        image.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        command.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &image});
        vk::BufferMemoryBarrier2 host{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *readback.buffer,
            .size = expected.size_bytes()};
        command.pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host});
      });
      std::vector<float> actual(expected.size());
      readback.read(std::as_writable_bytes(std::span(actual)));
      require(std::ranges::equal(actual, expected),
              "IBL six-face/mip/LUT GPU upload differs from float bake");
    };
    readImage(RendererHdrTestAccess::prefilter(renderer), 6, tagged.cubeRgba);
    readImage(RendererHdrTestAccess::brdf(renderer), 1, tagged.brdfLut.rgba);
    AssetLibrary assets;
    std::array normals{glm::vec3(1, 0, 0),
                       glm::vec3(-1, 0, 0),
                       glm::vec3(0, 1, 0),
                       glm::vec3(0, -1, 0),
                       glm::vec3(0, 0, 1),
                       glm::vec3(0, 0, -1),
                       glm::normalize(glm::vec3(1, 0, 1)),
                       glm::normalize(glm::vec3(1, 1, 1))};
    for (auto n : normals) {
      Mesh triangle;
      for (auto position : {glm::vec3(-1, -1, .5f), glm::vec3(3, -1, .5f),
                            glm::vec3(-1, 3, .5f)})
        triangle.vertices.push_back(Vertex{.position = position,
                                           .color = {1, 1, 1},
                                           .normal = n,
                                           .uv = {0, 0}});
      triangle.indices = {0, 1, 2};
      assets.meshes.push_back(triangle);
    }
    for (float r : {.04f, .2f, .65f, 1.f}) {
      Material m;
      m.name = "IBL metal";
      m.doubleSided = true;
      m.metallicFactor = 1;
      m.roughnessFactor = r;
      assets.materials.push_back(m);
    }
    Material dielectric;
    dielectric.name = "IBL dielectric";
    dielectric.doubleSided = true;
    dielectric.metallicFactor = 0;
    dielectric.roughnessFactor = .65f;
    assets.materials.push_back(dielectric);
    auto scene = renderer.prepareScene(assets);
    renderer.waitSceneUpload(scene);
    require(renderer.commitScene(scene), "IBL fixture scene did not commit");
    auto center = vk::Offset3D{int(swapchain.extent().width / 2),
                               int(swapchain.extent().height / 2), 0};
    LightingSettings lighting;
    lighting.intensity = 0;
    lighting.specularStrength = 0;
    lighting.environmentIntensity = 1;
    lighting.environmentDiffuseStrength = 1;
    lighting.environmentSpecularStrength = 1;
    lighting.shadowDebugMode = 0;
    lighting.pbrDebugMode = 8;
    glm::mat4 view(1);
    view[1][1] = -1; // Match the viewer's Vulkan projection.
    auto draw = [&](unsigned face, unsigned material, float noV = 1.f) {
      require(renderer.beginFrame(
                  view,
                  (normals[face] * noV +
                   glm::vec3(1, 0, 0) * std::sqrt(1 - noV * noV)) *
                          1000.f +
                      glm::vec3(0, 0, .5f),
                  lighting, false) == Renderer::FrameResult::eSuccess,
              "IBL frame begin failed");
      renderer.drawObject(face, material, glm::mat4(1));
      renderer.endFrame();
      device.logicalDevice().waitIdle();
      renderer.collectCompletedWork();
      return hdrPixel(device, RendererHdrTestAccess::output(renderer), center);
    };
    auto closePixel = [&](std::array<float, 4> actual, glm::vec3 expected) {
      for (unsigned c = 0; c < 3; ++c) {
        if (std::abs(actual[c] - expected[c]) >
            std::max(.008f, .003f * expected[c]))
          std::cerr << "IBL pixel actual=" << actual[c]
                    << " expected=" << expected[c] << '\n';
        require(std::abs(actual[c] - expected[c]) <=
                    std::max(.008f, .003f * expected[c]),
                "Production IBL cube direction/roughness/LUT/linear radiance "
                "differs");
      }
    };
    for (unsigned face = 0; face < 6; ++face)
      for (unsigned m = 0; m < 4; ++m) {
        float lod = assets.materials[m].roughnessFactor *
                    float(tagged.levels.size() - 1);
        glm::vec3 expected{2 + face + 2 * lod, 4 + lod, 8.f + face};
        expected *= .375f;
        lighting.exposureEv = 0;
        lighting.environmentIntensity = 1;
        lighting.environmentRotation = 0;
        closePixel(draw(face, m), expected);
        if (face == 4 && m == 2) {
          lighting.exposureEv = 3;
          closePixel(draw(face, m), expected);
          lighting.environmentIntensity = 2;
          closePixel(draw(face, m), expected * 2.f);
        }
      }
    lighting.exposureEv = 0;
    lighting.environmentIntensity = 1;
    lighting.environmentRotation = float(std::numbers::pi / 2);
    closePixel(draw(0, 3), glm::vec3(13, 7, 13) *
                               .375f); // +X rotates to -Z (face 5), LOD3.
    lighting.environmentRotation = 0;
    float dielectricLod = .65f * float(tagged.levels.size() - 1);
    auto dielectricExpected =
        glm::vec3(6 + 2 * dielectricLod, 4 + dielectricLod, 12) *
        (.04f * .25f + .125f);
    closePixel(draw(4, 4), dielectricExpected);
    closePixel(draw(4, 4, .1f),
               glm::vec3(3 + 2 * dielectricLod, 4 + dielectricLod, 9) *
                   (.04f * .25f + .125f));
    // At this grazing camera the reflection points to -X, not the +Z face.
    // LUT already includes Fresnel angular dependence.
    lighting.pbrDebugMode = 7;
    closePixel(draw(4, 4), glm::vec3(4, 1, 12) * .96f);
    closePixel(draw(4, 0), glm::vec3(0)); // Pure metal has no diffuse IBL.
    RendererHdrTestAccess::environment(renderer, bake);
    lighting.pbrDebugMode = 8;
    // Compare the real A/B texture interpolation with the CPU bake. Its
    // integration itself has independent hemisphere/analytic CPU references.
    auto lut = [&](float noV, float roughness) {
      float px = noV * bake.brdfLut.width - .5f,
            py = roughness * bake.brdfLut.height - .5f;
      int x = int(std::floor(px)), y = int(std::floor(py));
      auto fetch = [&](int xx, int yy) {
        xx = std::clamp(xx, 0, int(bake.brdfLut.width) - 1);
        yy = std::clamp(yy, 0, int(bake.brdfLut.height) - 1);
        auto p = (std::size_t(yy) * bake.brdfLut.width + unsigned(xx)) * 4;
        return glm::vec2(bake.brdfLut.rgba[p], bake.brdfLut.rgba[p + 1]);
      };
      return glm::mix(glm::mix(fetch(x, y), fetch(x + 1, y), px - x),
                      glm::mix(fetch(x, y + 1), fetch(x + 1, y + 1), px - x),
                      py - y);
    };
    for (unsigned m = 1; m < 5; ++m)
      for (float nv : {.1f, .5f, 1.f}) {
        auto ab = lut(nv, assets.materials[m].roughnessFactor);
        float f0 = m == 4 ? .04f : 1.f;
        auto expected = glm::vec3(4, 1, 12) * (f0 * ab.x + ab.y);
        closePixel(draw(4, m, nv), expected);
        if (nv == 1) {
          closePixel(draw(6, m), expected);
          closePixel(draw(7, m), expected);
        }
      }
    std::cout << "PASS IBL GPU: all six faces/mips/LUT exact readback; "
                 "production cube axis/rotation/trilinear roughness, "
                 "split-sum, SH/metal, exposure independence and intensity\n";
  }
  require(device.resourceLedger().snapshot().current == before,
          "IBL test GPU resources did not return to baseline");
}

void exerciseSpecularExtension(Device const &device, SwapChain const &swapchain,
                               unsigned framesInFlight) {
  auto before = device.resourceLedger().snapshot().current;
  {
    Renderer renderer(device, framesInFlight);
    renderer.recreateForSwapChain(swapchain);
    HdrImage constant{
        .width = 8, .height = 4, .rgba = std::vector<float>(8 * 4 * 4)};
    for (std::size_t i = 0; i < constant.rgba.size(); i += 4) {
      constant.rgba[i] = 4;
      constant.rgba[i + 1] = 1;
      constant.rgba[i + 2] = 12;
      constant.rgba[i + 3] = 1;
    }
    auto bake = bakeEnvironment(constant, {.faceSize = 8,
                                           .prefilterSamples = 64,
                                           .lutSize = 32,
                                           .lutSamples = 1024});
    RendererHdrTestAccess::environment(renderer, bake);
    AssetLibrary assets;
    auto mesh = materialTriangle();
    for (auto &v : mesh.vertices) {
      v.specularUv = {1.25f, .25f};      // Repeat selects left, not base UV.
      v.specularColorUv = {1.25f, .25f}; // Clamp selects right independently.
    }
    assets.meshes.push_back(mesh);
    auto otherUv = mesh;
    for (auto &v : otherUv.vertices)
      v.specularUv.x = .75f;
    assets.meshes.push_back(otherUv);
    struct Factors {
      float weight;
      glm::vec3 color;
    };
    std::array factors{Factors{1, glm::vec3(1)}, Factors{0, glm::vec3(1)},
                       Factors{.35f, {2, .5f, .25f}}, Factors{.6f, {50, 2, 1}},
                       Factors{1, glm::vec3(0)}};
    for (auto f : factors)
      for (float m : {0.f, .4f, 1.f}) {
        Material mat;
        mat.doubleSided = true;
        mat.roughnessFactor = .65f;
        mat.metallicFactor = m;
        mat.specularFactor = f.weight;
        mat.specularColorFactor = f.color;
        mat.tint = {.6f, .3f, .1f, 1};
        assets.materials.push_back(mat);
      }
    Material textured = assets.materials[0];
    textured.specularFactor = .6f;
    textured.specularColorFactor = {2, 1, .5f};
    std::array<unsigned char, 8> pixels{240, 100, 10, 51, 128, 64, 192, 204};
    textured.specularBytes = textured.specularColorBytes =
        materialTga(2, 1, pixels);
    textured.specularSampler.min = textured.specularSampler.mag =
        TextureFilter::Nearest;
    textured.specularSampler.mip = TextureMipFilter::None;
    textured.specularSampler.u = TextureWrap::Repeat;
    textured.specularColorSampler = textured.specularSampler;
    textured.specularColorSampler.u = TextureWrap::ClampToEdge;
    assets.materials.push_back(textured);
    auto scene = renderer.prepareScene(assets);
    renderer.waitSceneUpload(scene);
    require(renderer.commitScene(scene),
            "Specular reference scene failed to commit");
    // Two interpretations of one exact encoded image: linear A and sRGB RGB.
    require(renderer.resourceStatistics().lastSceneUpload.imageCopies == 6,
            "Specular texture interpretation/cache incorrectly aliased or "
            "added fallbacks");
    auto reload = renderer.prepareScene(assets);
    renderer.waitSceneUpload(reload);
    require(renderer.commitScene(reload) &&
                renderer.resourceStatistics().lastSceneUpload.imageCopies == 0,
            "Specular reload copied immutable shared images again");
    scene.reset();
    reload.reset();
    auto center = vk::Offset3D{int(swapchain.extent().width / 2),
                               int(swapchain.extent().height / 2), 0};
    LightingSettings lighting;
    lighting.color = {4, 1, 12};
    lighting.intensity = 1;
    lighting.shadowDebugMode = 0;
    lighting.environmentIntensity = 0;
    glm::mat4 view(1);
    view[1][1] = -1;
    auto draw = [&](unsigned material, double nv, unsigned meshId = 0) {
      double x = std::sqrt(1 - nv * nv);
      lighting.direction = {-float(x), 0, float(nv)};
      require(renderer.beginFrame(view,
                                  glm::vec3(float(x), 0, float(nv)) * 10000.f +
                                      glm::vec3(0, 0, .5f),
                                  lighting,
                                  false) == Renderer::FrameResult::eSuccess,
              "Specular frame failed to begin");
      renderer.drawObject(meshId, material, glm::mat4(1));
      renderer.endFrame();
      device.logicalDevice().waitIdle();
      renderer.collectCompletedWork();
      return hdrPixel(device, RendererHdrTestAccess::output(renderer), center);
    };
    auto close = [&](std::array<float, 4> actual, glm::dvec3 expected) {
      for (unsigned c = 0; c < 3; ++c) {
        if (std::abs(actual[c] - expected[c]) >
            std::max(.008, .003 * expected[c]))
          std::cerr << "Specular actual=" << actual[c]
                    << " expected=" << expected[c] << '\n';
        require(std::isfinite(actual[c]) && expected[c] >= 0 &&
                    std::abs(actual[c] - expected[c]) <=
                        std::max(.008, .003 * expected[c]),
                "Specular production GLSL differs from independent numeric "
                "reference");
      }
      require(actual[3] == 1, "Specular texture alpha affected opacity");
    };
    auto lut = [&](double nv) {
      double px = nv * bake.brdfLut.width - .5,
             py = .65 * bake.brdfLut.height - .5;
      int x = int(std::floor(px)), y = int(std::floor(py));
      auto fetch = [&](int xx, int yy) {
        xx = std::clamp(xx, 0, int(bake.brdfLut.width) - 1);
        yy = std::clamp(yy, 0, int(bake.brdfLut.height) - 1);
        auto index = (std::size_t(yy) * bake.brdfLut.width + unsigned(xx)) * 4;
        return glm::dvec2(bake.brdfLut.rgba[index],
                          bake.brdfLut.rgba[index + 1]);
      };
      return glm::mix(glm::mix(fetch(x, y), fetch(x + 1, y), px - x),
                      glm::mix(fetch(x, y + 1), fetch(x + 1, y + 1), px - x),
                      py - y);
    };
    auto check = [&](unsigned id, double nv, double weight, glm::dvec3 color,
                     unsigned meshId = 0) {
      double m = assets.materials[id].metallicFactor;
      glm::dvec3 base(assets.materials[id].tint);
      glm::dvec3 df0 = glm::min(.04 * color, glm::dvec3(1));
      double fc = std::pow(1 - nv, 5);
      glm::dvec3 dielectric = weight * (df0 * (1 - fc) + fc);
      glm::dvec3 metal = base * (1 - fc) + fc;
      double diffuse =
          (1 - std::max({dielectric.x, dielectric.y, dielectric.z})) * (1 - m);
      // Here H=N, NoL=NoV; D=1/(pi*r^4), G1=nv/(nv*(1-k)+k).
      double r = .65, k = (r + 1) * (r + 1) / 8;
      double g1 = nv / (nv * (1 - k) + k);
      double microfacet =
          g1 * g1 / (4 * nv * nv * std::numbers::pi * r * r * r * r);
      lighting.pbrDebugMode = 0;
      lighting.intensity = 1;
      lighting.diffuseStrength = lighting.specularStrength = 1;
      lighting.environmentIntensity = 0;
      close(draw(id, nv, meshId),
            glm::dvec3(4, 1, 12) * nv *
                (diffuse * base / std::numbers::pi +
                 glm::mix(dielectric, metal, m) * microfacet));
      lighting.pbrDebugMode = 8;
      lighting.intensity = 0;
      lighting.environmentIntensity = 1;
      auto ab = lut(nv);
      glm::dvec3 f0 = glm::mix(df0 * weight, base, m);
      double f90 = weight * (1 - m) + m;
      auto expected = glm::dvec3(4, 1, 12) * (f0 * ab.x + f90 * ab.y);
      close(draw(id, nv, meshId), expected);
      if (id == 6 && nv == .5) {
        lighting.exposureEv = 3;
        close(draw(id, nv, meshId), expected);
        lighting.exposureEv = 0;
        lighting.environmentIntensity = 2;
        close(draw(id, nv, meshId), expected * 2.);
        lighting.environmentIntensity = 1;
      }
      lighting.pbrDebugMode = 7;
      auto envF =
          weight * (df0 + (glm::max(glm::dvec3(1 - r), df0) - df0) * fc);
      double envD = (1 - std::max({envF.x, envF.y, envF.z})) * (1 - m);
      close(draw(id, nv, meshId), glm::dvec3(4, 1, 12) * envD * base);
      lighting.pbrDebugMode = 9;
      close(draw(id, nv, meshId), glm::dvec3(weight));
      lighting.pbrDebugMode = 10;
      close(draw(id, nv, meshId), df0 * weight);
    };
    for (unsigned id = 0; id < 15; ++id)
      for (double nv : {.1, .5, 1.}) {
        auto f = factors[id / 3];
        check(id, nv, f.weight, glm::dvec3(f.color));
      }
    auto decode = [](double u) {
      return u <= .04045 ? u / 12.92 : std::pow((u + .055) / 1.055, 2.4);
    };
    glm::dvec3 color =
        glm::dvec3(2, 1, .5) *
        glm::dvec3(decode(128. / 255), decode(64. / 255), decode(192. / 255));
    for (double nv : {.1, .5, 1.}) {
      check(15, nv, double(textured.specularFactor) * 51 / 255, color);
      check(15, nv, double(textured.specularFactor) * 204 / 255, color, 1);
    }
    // A corrupt replacement rolls back without publishing partial extension
    // assets.
    auto corrupt = assets;
    corrupt.materials[15].specularColorBytes = {std::byte{0}};
    bool rejected = false;
    try {
      (void)renderer.prepareScene(corrupt);
    } catch (std::exception const &) {
      rejected = true;
    }
    require(rejected, "Invalid specular image silently substituted");
    check(15, 1., double(textured.specularFactor) * 51 / 255, color);
    device.logicalDevice().waitIdle();
    renderer.collectCompletedWork();
    std::cout << "PASS specular GPU: factors/default/zero/color>1/black, metal "
                 "mixtures, "
                 "NoV=.1/.5/1 direct+IBL+scalar diffuse, sRGB/A/independent "
                 "UV+samplers, "
                 "debug/HDR/EV, cache reload/rollback and lifetime\n";
  }
  require(device.resourceLedger().snapshot().current == before,
          "Specular extension leaked scene/textures/UBO/descriptors");
}

void exerciseSpecularAa(Device const &device, SwapChain const &swapchain,
                        unsigned framesInFlight) {
  auto before = device.resourceLedger().snapshot().current;
  {
    Renderer renderer(device, framesInFlight);
    renderer.recreateForSwapChain(swapchain);
    HdrImage source{
        .width = 8, .height = 4, .rgba = std::vector<float>(8 * 4 * 4, 1)};
    auto bake = bakeEnvironment(source, {.faceSize = 8,
                                         .prefilterSamples = 16,
                                         .lutSize = 32,
                                         .lutSamples = 64});
    // Tag both axes used by the production split-sum with analytic affine
    // signals; this rejects inconsistent cube-LOD/LUT roughness independently
    // of the existing bake integration's separate numerical tests.
    for (unsigned mip = 0; mip < bake.levels.size(); ++mip) {
      auto m = bake.levels[mip];
      for (std::size_t i = m.offset; i < m.offset + 6 * m.size * m.size * 4;
           i += 4) {
        bake.cubeRgba[i] = 4 + 2 * mip;
        bake.cubeRgba[i + 1] = 1 + mip;
        bake.cubeRgba[i + 2] = 12 + 3 * mip;
      }
    }
    for (unsigned y = 0; y < 32; ++y)
      for (unsigned x = 0; x < 32; ++x) {
        auto i = (y * 32 + x) * 4;
        double r = (y + .5) / 32;
        bake.brdfLut.rgba[i] = float(.2 + .5 * r);
        bake.brdfLut.rgba[i + 1] = float(.1 + .1 * r);
      }
    RendererHdrTestAccess::environment(renderer, bake);
    AssetLibrary assets;
    auto mesh = materialTriangle();
    for (auto &v : mesh.vertices)
      v.normalUv = glm::vec2(v.position) * 8192.f;
    assets.meshes.push_back(mesh); // 0: coarse / terminal mip.
    for (auto &v : mesh.vertices)
      v.normalUv = {.0625f, .0625f};
    assets.meshes.push_back(mesh); // 1: authored texel.
    for (auto &v : mesh.vertices)
      v.normalUv =
          glm::vec2(v.position) * glm::vec2(swapchain.extent().width / 8.f,
                                            swapchain.extent().height / 8.f);
    assets.meshes.push_back(mesh); // 2: nearest LOD1, exact rho=2.
    auto curve = materialTriangle();
    curve.vertices[0].normal = glm::normalize(glm::vec3(-.8f, -.6f, 1));
    curve.vertices[1].normal = glm::normalize(glm::vec3(.9f, -.2f, 1));
    curve.vertices[2].normal = glm::normalize(glm::vec3(-.1f, .9f, 1));
    assets.meshes.push_back(curve); // 3: interpolated geometric curvature.
    std::array<unsigned char, 256> normals{}, flat{}, stripes{};
    for (unsigned y = 0; y < 8; ++y)
      for (unsigned x = 0; x < 8; ++x) {
        auto i = (y * 8 + x) * 4;
        normals[i] = x % 2 ? 51 : 204;
        stripes[i] = x < 4 ? 51 : 204;
        flat[i] = 204;
        for (auto *pixels : {&normals, &flat, &stripes}) {
          (*pixels)[i + 1] = 128;
          (*pixels)[i + 2] = 230;
          (*pixels)[i + 3] = 17;
        }
      }
    Material normal;
    normal.normalBytes = materialTga(8, 8, normals);
    normal.normalSampler = {.mag = TextureFilter::Nearest,
                            .min = TextureFilter::Nearest,
                            .mip = TextureMipFilter::Nearest,
                            .maxAnisotropy = 1};
    normal.roughnessFactor = .08f;
    normal.doubleSided = false;
    normal.tint = {.6f, .3f, .1f, 1};
    assets.materials.push_back(normal); // 0 dielectric.
    auto metal = normal;
    metal.metallicFactor = 1;
    assets.materials.push_back(metal); // 1
    auto smooth = normal;
    smooth.normalBytes = materialTga(8, 8, flat);
    assets.materials.push_back(smooth); // 2
    auto plain = normal;
    plain.normalBytes.clear();
    plain.roughnessFactor = 0;
    assets.materials.push_back(plain); // 3 floor.
    auto mask = normal;
    mask.alphaMode = AlphaMode::Mask;
    mask.alphaCutoff = .3f;
    mask.tint.a = .5f;
    assets.materials.push_back(mask); // 4
    auto low = normal;
    low.normalBytes = materialTga(8, 8, stripes);
    assets.materials.push_back(low); // 5
    auto pom = normal;
    pom.heightPath = "assets/render_tests/coverage.png";
    pom.parallaxScale = .04f;
    assets.materials.push_back(pom); // 6
    auto rough = normal;
    rough.roughnessFactor = 1;
    assets.materials.push_back(rough); // 7 saturation.
    auto prepared = renderer.prepareScene(assets);
    renderer.waitSceneUpload(prepared);
    require(renderer.commitScene(prepared), "AA fixture commit failed");
    auto reload = renderer.prepareScene(assets);
    renderer.waitSceneUpload(reload);
    require(renderer.commitScene(reload) &&
                renderer.resourceStatistics().lastSceneUpload.imageCopies == 0,
            "AA reload uploaded normal metadata again");
    prepared.reset();
    reload.reset();
    auto center = vk::Offset3D{int(swapchain.extent().width / 2),
                               int(swapchain.extent().height / 2), 0};
    LightingSettings lighting;
    lighting.shadowDebugMode = 0;
    lighting.environmentIntensity = 0;
    lighting.exposureEv = 3; // Data views must bypass this display setting.
    lighting.color = {4, 1, 12};
    lighting.intensity = 1;
    glm::mat4 view(1);
    view[1][1] = -1;
    auto N = glm::normalize(glm::dvec3(1. / 255, 1. / 255, 1));
    auto draw = [&](unsigned meshId, unsigned matId, unsigned debug,
                    glm::mat4 model = glm::mat4(1), double nv = 1.) {
      lighting.pbrDebugMode = int(debug);
      auto V =
          glm::normalize(N * nv + glm::dvec3(std::sqrt(1 - nv * nv), 0, 0));
      lighting.direction = glm::vec3(N);
      require(renderer.beginFrame(
                  view, glm::vec3(V) * 10000.f + glm::vec3(0, 0, .5f), lighting,
                  false) == Renderer::FrameResult::eSuccess,
              "AA frame begin failed");
      renderer.drawObject(meshId, matId, model);
      renderer.endFrame();
      device.logicalDevice().waitIdle();
      renderer.collectCompletedWork();
      auto display = RendererHdrTestAccess::settings(renderer);
      if (debug == 3 || (debug >= 9 && debug <= 13))
        require(display.exposureEv == 0 && !display.toneMap,
                "AA diagnostic data was exposed/tone mapped");
      else if (debug == 8)
        require(display.exposureEv == 3 && display.toneMap,
                "IBL radiance incorrectly treated as a data diagnostic");
      return hdrPixel(device, RendererHdrTestAccess::output(renderer), center);
    };
    auto close = [&](std::array<float, 4> actual, glm::dvec3 expected,
                     double tolerance = .002) {
      for (unsigned c = 0; c < 3; ++c) {
        if (!std::isfinite(actual[c]) ||
            std::abs(actual[c] - expected[c]) >
                std::max(tolerance, .005 * expected[c]))
          std::cerr << "AA actual=" << actual[c] << " expected=" << expected[c]
                    << "\n";
        require(std::isfinite(actual[c]) &&
                    std::abs(actual[c] - expected[c]) <=
                        std::max(tolerance, .005 * expected[c]),
                "AA production pixel differs from independent reference");
      }
    };
    double nx = 153. / 255, ny = 1. / 255, nz = 205. / 255;
    double meanLength =
        std::sqrt((ny * ny + nz * nz) / (nx * nx + ny * ny + nz * nz));
    double d = std::round((1 - meanLength) * 255) / 255;
    double kernel = std::min(1., 2 * d / (1 - d));
    auto filtered = [&](double r, double scale = 1.) {
      return std::pow(std::min(1., std::pow(r, 4) + kernel * scale * scale),
                      .25);
    };
    close(draw(0, 0, 12), glm::dvec3(kernel));
    close(draw(0, 0, 3), glm::dvec3(filtered(.08)));
    close(draw(0, 0, 11), glm::dvec3(.08));
    close(draw(1, 0, 3),
          glm::dvec3(.08)); // LOD0 never reads authored A as loss.
    close(draw(2, 0, 3), glm::dvec3(filtered(.08)));
    close(draw(2, 5, 3),
          glm::dvec3(.08)); // Low frequency still resolved at LOD1.
    close(draw(0, 5, 3), glm::dvec3(filtered(.08)));
    close(draw(0, 2, 12),
          glm::dvec3(0)); // Tilted flat normals preserve roughness.
    close(draw(0, 2, 3), glm::dvec3(.08));
    close(draw(0, 3, 3), glm::dvec3(.04));
    close(draw(0, 7, 3), glm::dvec3(1));
    auto alpha = draw(0, 4, 3);
    close(alpha, glm::dvec3(filtered(.08)));
    require(alpha[3] == .5f, "Normal alpha metadata affected MASK opacity");
    close(draw(0, 6, 3, glm::mat4(1), .2),
          glm::dvec3(filtered(.08))); // POM offsets/gradients.
    for (float scale : {0.f, .5f, -.5f, 2.f}) {
      renderer.setMaterialSurfaceParams(0, scale, .04f);
      close(draw(0, 0, 3), glm::dvec3(filtered(.08, scale)));
    }
    renderer.setMaterialSurfaceParams(0, 1, .04f);
    for (float sx : {2.f, -2.f}) {
      auto model = glm::scale(glm::mat4(1), glm::vec3(sx, .5f, 1));
      close(draw(0, 0, 3, model), glm::dvec3(filtered(.08)));
    }
    lighting.specularAaEnabled = false;
    close(draw(0, 0, 3), glm::dvec3(.08));
    lighting.specularAaEnabled = true;
    // Independent finite-difference estimate at the actual raster 2x2 quad.
    auto geometricNormal = [&](int px, int py) {
      double x = 2 * (px + .5) / swapchain.extent().width - 1;
      double y = 1 - 2 * (py + .5) / swapchain.extent().height;
      double b = (x + 1) / 4, c = (y + 1) / 4;
      return glm::normalize(glm::dvec3(curve.vertices[0].normal) * (1 - b - c) +
                            glm::dvec3(curve.vertices[1].normal) * b +
                            glm::dvec3(curve.vertices[2].normal) * c);
    };
    auto dx = geometricNormal((center.x & ~1) + 1, center.y) -
              geometricNormal(center.x & ~1, center.y);
    auto dy = geometricNormal(center.x, (center.y & ~1) + 1) -
              geometricNormal(center.x, center.y & ~1);
    double gk = std::min(.2, .3 * (glm::dot(dx, dx) + glm::dot(dy, dy)));
    close(draw(3, 3, 13), glm::dvec3(gk), 1e-7);
    close(draw(3, 3, 3), glm::dvec3(std::pow(std::pow(.04, 4) + gk, .25)),
          .0003);
    // HDR direct and IBL must consume the same broadened r, dielectric + metal.
    for (unsigned id : {0u, 1u}) {
      double r = filtered(.08);
      glm::dvec3 base(normal.tint), f0 = id == 1 ? base : glm::dvec3(.04);
      auto expected =
          (id == 1 ? glm::dvec3(0) : .96 * base / std::numbers::pi) +
          f0 / (4 * std::numbers::pi * std::pow(r, 4));
      close(draw(0, id, 0), glm::dvec3(4, 1, 12) * expected, .012);
      lighting.environmentIntensity = 1;
      double lod = 3 * r, lutR = std::clamp(r, .5 / 32, 31.5 / 32);
      auto ibl = glm::dvec3(4 + 2 * lod, 1 + lod, 12 + 3 * lod) *
                 (f0 * (.2 + .5 * lutR) + .1 + .1 * lutR);
      close(draw(0, id, 8), ibl, .012);
      close(draw(0, id, 8, glm::mat4(1), .1), ibl, .012);
      lighting.environmentIntensity = 0;
    }
    auto corrupt = assets;
    corrupt.materials[0].normalBytes = {std::byte{0}};
    bool rejected = false;
    try {
      (void)renderer.prepareScene(corrupt);
    } catch (std::exception const &) {
      rejected = true;
    }
    require(rejected, "Corrupt normal replacement did not roll back");
    close(draw(0, 0, 3), glm::dvec3(filtered(.08)));
    std::cout << "PASS specular AA GPU: "
                 "loss/LOD/frequency/flat/floor/saturation, scale0/negative, "
                 "mirror/nonuniform, MASK/POM, geometry finite difference, HDR "
                 "direct/IBL/grazing, toggle/reload/rollback/lifetime\n";
  }
  require(device.resourceLedger().snapshot().current == before,
          "AA resources leaked at shutdown");
}

void exercisePunctualLights(Device const &device, SwapChain const &swapchain,
                            unsigned framesInFlight) {
  auto before = device.resourceLedger().snapshot().current;
  {
    Renderer renderer(device, framesInFlight);
    renderer.recreateForSwapChain(swapchain);
    AssetLibrary assets;
    auto mesh = materialTriangle();
    for (auto &v : mesh.vertices)
      v.normalUv = glm::vec2(v.position) * 8192.f;
    assets.meshes.push_back(mesh);
    for (float m : {0.f, .4f, 1.f}) {
      Material mat;
      mat.doubleSided = true;
      mat.roughnessFactor = .65f;
      mat.metallicFactor = m;
      mat.specularFactor = .35f;
      mat.specularColorFactor = {2, .5f, .25f};
      mat.tint = {.6f, .3f, .1f, 1};
      assets.materials.push_back(mat);
    }
    auto blend = assets.materials[0];
    blend.alphaMode = AlphaMode::Blend;
    blend.tint.a = .5f;
    assets.materials.push_back(blend); // 3.
    auto mask = blend;
    mask.alphaMode = AlphaMode::Mask;
    mask.alphaCutoff = .3f;
    assets.materials.push_back(mask); // 4.
    auto aa = assets.materials[0];
    aa.roughnessFactor = .08f;
    std::array<unsigned char, 256> normals{};
    for (unsigned y = 0; y < 8; ++y)
      for (unsigned x = 0; x < 8; ++x) {
        auto i = (y * 8 + x) * 4;
        normals[i] = x % 2 ? 51 : 204;
        normals[i + 1] = 128;
        normals[i + 2] = 230;
        normals[i + 3] = 17;
      }
    aa.normalBytes = materialTga(8, 8, normals);
    aa.normalSampler = {.mag = TextureFilter::Nearest,
                        .min = TextureFilter::Nearest,
                        .mip = TextureMipFilter::Nearest,
                        .maxAnisotropy = 1};
    assets.materials.push_back(aa); // 5: minified normal variance.
    auto glowing = assets.materials[0];
    glowing.emissiveFactor = {4, 1, 2};
    std::array<unsigned char, 4> blackAo{0, 0, 0, 255};
    glowing.occlusionBytes = materialTga(1, 1, blackAo);
    assets.materials.push_back(
        glowing); // 6: AO affects indirect only; emissive once.
    auto prepared = renderer.prepareScene(assets);
    renderer.waitSceneUpload(prepared);
    require(renderer.commitScene(prepared),
            "Punctual fixture failed to commit");
    prepared.reset();
    auto stable = renderer.resourceStatistics();
    auto center = vk::Offset3D{int(swapchain.extent().width / 2),
                               int(swapchain.extent().height / 2), 0};
    // Identity view with Vulkan Y flip; this is the exact center sample's world
    // position.
    glm::vec3 surface{1.f / swapchain.extent().width,
                      -1.f / swapchain.extent().height, .5f};
    glm::mat4 view(1);
    view[1][1] = -1;
    LightingSettings settings;
    settings.sunEnabled = false;
    settings.environmentIntensity = 0;
    settings.shadowDebugMode = 0;
    settings.diffuseStrength = settings.specularStrength = 1;
    settings.exposureEv = 2;
    settings.pbrDebugMode = 14;
    glm::dvec3 N(0, 0, 1), V(0, 0, 1);
    auto brdf = [&](unsigned matId, glm::dvec3 L,
                    glm::dvec3 normal = glm::dvec3(0, 0, 1),
                    double roughness = .65) {
      auto const &mat = assets.materials[matId];
      double m = mat.metallicFactor, nl = std::max(glm::dot(normal, L), 0.),
             nv = std::max(glm::dot(normal, V), 0.);
      auto H = glm::normalize(L + V);
      double nh = std::max(glm::dot(normal, H), 0.),
             vh = std::max(glm::dot(V, H), 0.);
      double a2 = std::pow(roughness, 4), den = nh * nh * (a2 - 1) + 1;
      double D = a2 / std::max(std::numbers::pi * den * den, 1e-6);
      double k = (roughness + 1) * (roughness + 1) / 8;
      double G = (nv / std::max(nv * (1 - k) + k, 1e-6)) *
                 (nl / std::max(nl * (1 - k) + k, 1e-6));
      auto f0 =
          glm::min(.04 * glm::dvec3(mat.specularColorFactor), glm::dvec3(1)) *
          double(mat.specularFactor);
      double grazing = std::pow(1 - vh, 5);
      auto df = f0 + (double(mat.specularFactor) - f0) * grazing;
      auto base = glm::dvec3(mat.tint);
      auto mixed = (1 - m) * f0 + m * base;
      auto f = mixed + ((1 - m) * mat.specularFactor + m - mixed) * grazing;
      auto diffuse = (1 - std::max({df.x, df.y, df.z})) * (1 - m) * base /
                     std::numbers::pi;
      return nl * (diffuse + D * G * f / std::max(4 * nv * nl, 1e-4));
    };
    auto draw = [&](unsigned matId = 0, glm::mat4 model = glm::mat4(1)) {
      Renderer::DrawItem item{
          .meshId = 0, .materialId = matId, .modelMatrix = model};
      auto items = std::span<Renderer::DrawItem const>{&item, 1};
      bool transparent = assets.materials[matId].alphaMode == AlphaMode::Blend;
      require(
          renderer.renderFrame(
              {.opaque =
                   transparent ? std::span<Renderer::DrawItem const>{} : items,
               .transparent =
                   transparent ? items : std::span<Renderer::DrawItem const>{},
               .sky = false},
              view, surface + glm::vec3(V) * 10000.f, settings,
              false) == Renderer::FrameResult::eSuccess,
          "Punctual frame failed");
      device.logicalDevice().waitIdle();
      renderer.collectCompletedWork();
      auto display = RendererHdrTestAccess::settings(renderer);
      require(display.exposureEv == 2 && display.toneMap,
              "Direct radiance debug bypassed HDR exposure");
      return hdrPixel(device, RendererHdrTestAccess::output(renderer), center);
    };
    auto close = [&](std::array<float, 4> actual, glm::dvec3 expected) {
      for (unsigned c = 0; c < 3; ++c) {
        if (!std::isfinite(actual[c]) || std::abs(actual[c] - expected[c]) >
                                             std::max(.003, .007 * expected[c]))
          std::cerr << "Punctual actual=" << actual[c]
                    << " expected=" << expected[c] << '\n';
        require(std::isfinite(actual[c]) &&
                    std::abs(actual[c] - expected[c]) <=
                        std::max(.003, .007 * expected[c]),
                "Punctual GLSL differs from independent double reference");
      }
    };
    close(draw(), glm::dvec3(0));
    PunctualLight point;
    point.position = surface + glm::vec3(0, 0, 1);
    point.intensity = 16;
    point.color = {1, .25f, .5f};
    auto irradiance = glm::dvec3(16, 4, 8);
    for (unsigned count : {1u, 16u, 32u, 64u, 65u}) {
      settings.punctualLights.assign(count, point);
      for (auto &l : settings.punctualLights)
        l.intensity /= count;
      close(draw(), irradiance * brdf(0, N));
    }
    settings.punctualLights = {point};
    for (unsigned mat : {1u, 2u})
      close(draw(mat), irradiance * brdf(mat, N));
    V = {.6, 0, .8};
    close(draw(2),
          irradiance *
              brdf(2, N)); // Camera motion changes per-light Fresnel/H.
    V = N;
    close(draw(0, glm::translate(glm::mat4(1), glm::vec3(0, 0, .1f))),
          irradiance / .81 * brdf(0, N));
    settings.punctualLights[0].position = surface + glm::vec3(0, 0, 2);
    close(draw(), irradiance * .25 * brdf(0, N));
    settings.punctualLights[0].range = 4;
    close(draw(), irradiance * .234375 * brdf(0, N));
    settings.punctualLights[0].range = 2;
    close(draw(), glm::dvec3(0));
    settings.punctualLights[0] = point;
    settings.punctualLights[0].position = surface;
    close(draw(), glm::dvec3(0));
    settings.punctualLights[0] = point;
    settings.punctualLights[0].position = surface + glm::vec3(.6f, 0, .8f);
    close(draw(), irradiance * brdf(0, glm::dvec3(.6, 0, .8)));
    auto spot = point;
    spot.type = PunctualLightType::Spot;
    spot.innerCone = .2f;
    spot.outerCone = .6f;
    settings.punctualLights = {spot};
    close(draw(), irradiance * brdf(0, N));
    double middle = (std::cos(.2) + std::cos(.6)) * .5;
    settings.punctualLights[0].direction = {
        float(std::sqrt(1 - middle * middle)), 0, -float(middle)};
    close(draw(), irradiance * .25 * brdf(0, N));
    settings.punctualLights[0].direction = {1, 0, 0};
    close(draw(), glm::dvec3(0));
    settings.punctualLights[0] = spot;
    settings.punctualLights[0].innerCone = 0;
    settings.punctualLights[0].outerCone = 1e-6f;
    close(draw(), irradiance * brdf(0, N));
    auto sun = point;
    sun.type = PunctualLightType::Directional;
    sun.direction = {0, 0, -4};
    sun.position = {99, 100, 1000};
    settings.punctualLights = {sun};
    close(draw(), irradiance * brdf(0, N));
    settings.punctualLights[0].enabled = false;
    close(draw(), glm::dvec3(0));
    settings.punctualLights = {point};
    close(draw(4),
          irradiance *
              brdf(4, N)); // MASK accepted; cutoff leaves opaque output.
    close(draw(3), .5 * irradiance * brdf(3, N) + glm::dvec3(.025, .035, .05));
    // Normal mip variance feeds the identical filtered roughness for a point
    // light.
    auto normal = glm::normalize(glm::dvec3(1. / 255, 1. / 255, 1));
    double nx = 153. / 255, ny = 1. / 255, nz = 205. / 255;
    double loss = std::round((1 - std::sqrt((ny * ny + nz * nz) /
                                            (nx * nx + ny * ny + nz * nz))) *
                             255) /
                  255;
    double filtered =
        std::pow(std::min(1., std::pow(.08, 4) + 2 * loss / (1 - loss)), .25);
    close(draw(5), irradiance * brdf(5, N, normal, filtered));
    settings.pbrDebugMode = 0;
    settings.punctualLights.assign(16, point);
    for (auto &l : settings.punctualLights)
      l.intensity /= 16;
    close(draw(6), irradiance * brdf(6, N) + glm::dvec3(4, 1, 2));
    settings.pbrDebugMode = 14;
    settings.punctualLights = {point};
    settings.punctualLights[0].intensity =
        std::numeric_limits<float>::quiet_NaN();
    bool rejected = false;
    try {
      renderer.beginFrame(view, surface + glm::vec3(0, 0, 2), settings, false);
    } catch (std::runtime_error const &) {
      rejected = true;
    }
    require(rejected, "Invalid lights reached frame acquisition");
    settings.punctualLights[0] = point;
    close(draw(), irradiance * brdf(0, N));
    require(renderer.resourceStatistics().sceneImageCopies ==
                    stable.sceneImageCopies &&
                renderer.resourceStatistics().pipelineBuilds ==
                    stable.pipelineBuilds,
            "Light edits/growth uploaded scene images or rebuilt pipelines");
    // Real perspective production compute + every HDR pixel, independently
    // compared with the original full-light path. Existing double-reference
    // tests above remain the BRDF oracle; this tests conservative assignment.
    glm::vec3 camera(0, 0, 3);
    glm::mat4 model = glm::scale(glm::mat4(1), glm::vec3(8, 8, 1));
    unsigned testMaterial = 0;
    auto perspectiveDraw = [&](bool clustered) {
      auto projection = glm::perspective(glm::radians(75.f),
          float(swapchain.extent().width) / swapchain.extent().height, .1f, 50.f);
      projection[1][1] *= -1;
      auto vp = projection * glm::lookAt(camera, glm::vec3(0), glm::vec3(0, 1, 0));
      settings.clusteredLights = clustered;
      Renderer::DrawItem item{.meshId = 0, .materialId = testMaterial, .modelMatrix = model};
      auto items = std::span<Renderer::DrawItem const>(&item, 1);
      bool blend = testMaterial == 3;
      require(renderer.renderFrame({.opaque = blend ? std::span<Renderer::DrawItem const>{} : items,
          .transparent = blend ? items : std::span<Renderer::DrawItem const>{}}, vp, camera, settings, false)
          == Renderer::FrameResult::eSuccess, "Clustered perspective frame failed");
      device.logicalDevice().waitIdle(); renderer.collectCompletedWork();
      require(renderer.gpuTimings().clustered == (clustered && !settings.punctualLights.empty()),
              "Production culling path/timestamp did not match requested perspective path");
      return hdrImage(device, RendererHdrTestAccess::output(renderer));
    };
    unsigned pairs = 0;
    auto compare = [&] {
      auto full = perspectiveDraw(false), clustered = perspectiveDraw(true);
      require(full.size() == clustered.size(), "HDR comparison extent changed");
      for (std::size_t i = 0; i < full.size(); ++i)
        require(std::isfinite(clustered[i]) && std::abs(clustered[i] - full[i]) <=
            std::max(.003f, .007f * std::abs(full[i])), "Cluster culling removed a contributing light");
      ++pairs;
    };
    settings.pbrDebugMode = 14; settings.exposureEv = 0;
    PunctualLight finite; finite.position = {0, 0, 1.5f}; finite.range = 20; finite.intensity = 2;
    for (unsigned count : {0u, 1u, 16u, 32u, 64u, 65u}) {
      settings.punctualLights.assign(count, finite);
      for (auto &light : settings.punctualLights) light.intensity /= std::max(count, 1u);
      compare();
      if (count == 65) {
        auto words = clusterWords(device, renderer); bool overflow = false;
        for (std::size_t i = 0; i < words.size(); i += 65) overflow |= words[i] == UINT32_MAX;
        require(overflow, "65-light case did not exercise production overflow fallback");
      }
    }
    settings.punctualLights.assign(16, finite);
    for (unsigned i = 1; i < 16; ++i) settings.punctualLights[i].position.x = 1000 + i;
    compare();
    auto words = clusterWords(device, renderer);
    bool culled = false;
    for (std::size_t i = 0; i < words.size(); i += 65) {
      require(words[i] <= 1, "Distant finite lights were not culled");
      culled |= words[i] == 1;
    }
    require(culled, "Culling test had no active light");
    for (unsigned id : {1u, 2u, 3u, 4u, 5u, 6u}) { testMaterial = id; compare(); }
    testMaterial = 0;
    PunctualLight global = finite; global.range = 0;
    PunctualLight direction = finite; direction.type = PunctualLightType::Directional; direction.direction = {0, 0, -1};
    PunctualLight cone = finite; cone.type = PunctualLightType::Spot; cone.direction = {0, 0, -1};
    settings.punctualLights = {finite, global, direction, cone}; compare();
    camera = {1, .4f, 3}; settings.punctualLights[0].position.x += .5f; compare();
    camera = {0, 0, 3};
    for (unsigned slice : {8u, 12u, 18u}) for (float side : {-.0001f, .0001f}) {
      float depth = .1f * std::pow(500.f, float(slice) / 24) * (1 + side);
      model[3][2] = 2.5f - depth;
      settings.punctualLights[0].position.z = 3 - depth + .5f;
      compare();
    }
    require(renderer.gpuTimings().cullingMs > 0 && renderer.renderGraphDump().find("Cluster light assignment") != std::string::npos,
            "Compute pass/timing missing from production graph");
    std::cout << "PASS clustered production HDR: " << pairs << " full-image pairs, finite cull, "
                 "0/1/16/32/64/65, overflow/global/spot, materials/MASK/BLEND, movement/slice boundaries\n";
    std::cout << "PASS punctual production HDR: 0/1/16/32/64/65 lights, "
                 "growth, inverse square/range/zero, "
                 "spot cosine ramp/narrow cone, directional/movement, "
                 "PBR/specular AA, MASK/BLEND, camera/object motion, "
                 "AO/emissive once, invalid retry\n";
  }
  require(device.resourceLedger().snapshot().current == before,
          "Punctual light regression leaked GPU storage");
}

void exerciseIndoorLighting(Device const &device, SwapChain const &swapchain,
                            unsigned framesInFlight) {
  Renderer renderer(device, framesInFlight);
  renderer.recreateForSwapChain(swapchain);
  AssetLibrary assets;
  assets.meshes.push_back(materialTriangle());
  Material diffuse;
  diffuse.doubleSided = true;
  diffuse.specularFactor = 0;
  Material metal = diffuse;
  metal.metallicFactor = 1;
  metal.roughnessFactor = .3f;
  Material glow=diffuse;glow.emissiveFactor={2,0,0};
  assets.materials = {diffuse, metal, glow};
  // Six complete room walls, separate from the analytic receiving surface.
  for (unsigned face=0;face<6;++face) {
    Mesh wall;
    for (auto uv:{glm::vec2{-1,-1},glm::vec2{1,-1},glm::vec2{1,1},glm::vec2{-1,1}}) {
      auto n=environmentCubeDirection(face,0,0);
      auto d=environmentCubeDirection(face,uv.x,uv.y);
      auto pos=d*(2.f/std::max({std::abs(d.x),std::abs(d.y),std::abs(d.z)}));
      wall.vertices.push_back(Vertex{.position=pos,.color={1,1,1},.normal=-n,.uv={0,0}});
    }
    wall.indices={0,1,2,0,2,3};assets.meshes.push_back(wall);
  }
  auto halfEmitter=assets.meshes[5]; // +Z wall, emission only in +X half.
  for (auto &v:halfEmitter.vertices) {v.position.x=(v.position.x+2)*.5f;v.position.z-=.01f;}
  assets.meshes.push_back(halfEmitter); // mesh7.
  auto prepared = renderer.prepareScene(assets);
  renderer.waitSceneUpload(prepared);
  require(renderer.commitScene(prepared), "Indoor fixture commit failed");
  std::array receiver{Renderer::DrawItem{.meshId = 0, .materialId = 0}};
  std::array casters{receiver[0], Renderer::DrawItem{.meshId=0, .materialId=0,
    .modelMatrix=glm::translate(glm::mat4(1), glm::vec3{0,0,.5f})}};
  LightingSettings lighting;
  lighting.direction = {0,0,1};
  lighting.color = {1,1,1};
  lighting.environmentIntensity = 0;
  lighting.pbrDebugMode = 14;
  lighting.shadowOrthoExtent = 2;
  lighting.shadowLightDistance = 4;
  lighting.shadowFarPlane = 10;
  lighting.shadowPcfRadius = 0;
  Camera fixtureCamera;
  fixtureCamera.position={0,0,2};fixtureCamera.target={0,0,.5f};
  auto viewProj=fixtureCamera.viewProj(float(swapchain.extent().width)/swapchain.extent().height);
  glm::vec3 cameraPosition{0,0,2};
  auto draw = [&](std::span<Renderer::DrawItem const> shadowCasters, bool shadows=true) {
    require(renderer.renderFrame({.opaque=receiver,.allOpaque=shadowCasters,.sky=false},
      viewProj, cameraPosition, lighting, shadows)==Renderer::FrameResult::eSuccess,
      "Indoor fixture frame failed");
    device.logicalDevice().waitIdle();
    renderer.collectCompletedWork();
    auto c=hdrPixel(device, RendererHdrTestAccess::output(renderer),
      {int(swapchain.extent().width/2),int(swapchain.extent().height/2),0});
    return std::max({c[0],c[1],c[2]});
  };
  float open=draw(receiver), blocked=draw(casters);
  std::cout << "Indoor sun direct: open="<<open<<" blocked="<<blocked<<std::endl;
  require(open>.2f && blocked<open*.01f, "Default sun shadow did not block direct light");
  lighting.sunEnabled=false;
  PunctualLight spot;
  spot.type=PunctualLightType::Spot;spot.position={0,0,2};spot.direction={0,0,-1};
  spot.intensity=10;spot.castsShadow=true;
  lighting.punctualLights={spot};
  for (bool clustered:{false,true}) {
    lighting.clusteredLights=clustered;
    float lit=draw(receiver), dark=draw(casters), disabled=draw(casters,false);
    require(!clustered || renderer.clusterGrid().screen.z==1, "Spot test silently used full-light fallback");
    std::cout<<"Indoor spot direct clustered="<<clustered<<": open="<<lit<<" blocked="<<dark<<" disabled="<<disabled<<std::endl;
    require(lit>1 && dark<lit*.01f && std::abs(disabled-lit)<.003f,
      "Spot shadow/disable or clustered shading failed");
    casters[1].modelMatrix=glm::translate(glm::mat4(1),glm::vec3{10,0,.5f});
    require(draw(casters)>lit*.99f,"Moving blocker did not update spot shadow");
    casters[1].modelMatrix=glm::translate(glm::mat4(1),glm::vec3{0,0,.5f});
  }
  lighting.punctualLights.assign(5,spot);
  float fifth=draw(casters);
  require(renderer.spotShadowsAssigned()==4 && renderer.spotShadowsRequested()==5 && fifth>1 && fifth<1.43f,
    "Fifth light was dropped instead of losing only its shadow");
  auto edge=spot;
  edge.direction=glm::normalize(glm::vec3{-1,0,-1});
  edge.outerCone=glm::radians(45.f)+.002f;
  edge.shadowPcfRadius=4;
  lighting.punctualLights={edge};lighting.pbrDebugMode=16;
  float edgeVisibility=draw(casters);
  std::cout<<"Indoor atlas edge visibility (PCF 4)="<<edgeVisibility<<std::endl;
  require(edgeVisibility<.01f,"Spot PCF bled outside its atlas tile");
  lighting.punctualLights.clear();
  lighting.pbrDebugMode=8;
  lighting.environmentIntensity=1;
  receiver[0].materialId=1;
  std::vector<Renderer::DrawItem> room;
  for (unsigned i=0;i<6;++i) room.push_back({.meshId=i+1,.materialId=0});
  lighting.localProbe={true,{-1.99f,-1.99f,-1.99f},{1.99f,1.99f,1.99f},{0,0,0}};
  float before=draw(casters);
  std::cout<<"Indoor before room capture="<<before<<std::endl;
  require(before>.1f,"Global IBL control was already black");
  auto frameId=renderer.submittedFrameId();
  renderer.captureLocalProbe({.allOpaque=room,.sky=true},lighting);
  require(renderer.submittedFrameId()==frameId && renderer.localProbeMatches(lighting),
    "Probe capture corrupted normal frame IDs or publication");
  float closed=draw(casters);
  std::cout<<"Indoor closed room reflected sky="<<closed<<std::endl;
  require(closed<.001f,"Indoor metal still reflects global sky through closed room");
  lighting.localProbe.enabled=false;
  require(draw(casters)>.1f,"Probe disable did not restore global control");
  lighting.localProbe.enabled=true;
  auto openRoom=room;openRoom.erase(openRoom.begin()+4); // +Z window/open wall.
  renderer.captureLocalProbe({.allOpaque=openRoom,.sky=true},lighting);
  float window=draw(casters);
  std::cout<<"Indoor open wall reflected sky="<<window<<std::endl;
  require(window>.1f,"Probe cannot see the actual opening in room geometry");
  room[4].materialId=2;
  renderer.captureLocalProbe({.allOpaque=room,.sky=true},lighting);
  auto rgb=hdrPixel(device, RendererHdrTestAccess::output(renderer),
      {int(swapchain.extent().width/2),int(swapchain.extent().height/2),0});
  // Draw after publication so the sample uses the new captured cube.
  float emission=draw(casters);
  rgb=hdrPixel(device, RendererHdrTestAccess::output(renderer),
      {int(swapchain.extent().width/2),int(swapchain.extent().height/2),0});
  require(emission>.5f && rgb[1]<.001f && rgb[2]<.001f,
    "Metal did not reflect the local red emitter, or old sky leaked into capture");
  room[4].materialId=0;
  auto asymmetric=room;asymmetric.push_back({.meshId=7,.materialId=2});
  renderer.captureLocalProbe({.allOpaque=asymmetric,.sky=true},lighting);
  cameraPosition={1,0,2};float negativeX=draw(casters);
  cameraPosition={-1,0,2};float positiveX=draw(casters);
  std::cout<<"Indoor half-emitter reflection: -X="<<negativeX<<" +X="<<positiveX<<std::endl;
  require(positiveX>.5f && negativeX<positiveX*.2f,"Captured cube reflection is mirrored horizontally");
  cameraPosition={0,0,2};
  renderer.captureLocalProbe({.allOpaque=room,.sky=true},lighting);
  require(draw(casters)<.001f,"Refreshing a closed room retained stale bright probe");
  require(!renderer.localProbeMatches(lighting), "Changed geometry not reported as stale probe");
  renderer.captureLocalProbe({.allOpaque=casters,.sky=true},lighting);
  require(renderer.localProbeMatches(lighting),"Refreshed geometry still reported stale");
  renderer.setMaterialTint(0,{.2f,.2f,.2f,1});
  require(!renderer.localProbeMatches(lighting),"Edited material not reported as stale probe");
  auto replacement=renderer.prepareScene(assets);renderer.waitSceneUpload(replacement);
  require(renderer.commitScene(replacement) && !renderer.localProbeValid(),
    "Scene switch retained a stale room probe");
  std::cout<<"Indoor direct/shadow/closed/open/emitter/refresh/scene-switch regressions passed"<<std::endl;
}

void exerciseKitchenScene(Device const &device, SwapChain const &swapchain,
                          unsigned slots, std::filesystem::path const &path,
                          std::filesystem::path const &directory,
                          bool colourDiagnostic, bool visibilityDiagnostic,
                          bool aoDiagnostic) {
  auto imported=loadStaticGltfScene(path,{});
  require(imported.lights.empty(),"Kitchen control unexpectedly acquired asset lights");
  Renderer r(device,slots);r.recreateForSwapChain(swapchain);
  if(visibilityDiagnostic)r.setShadowCasterCullingEnabled(false);
  AssetLibrary assets{.meshes=std::move(imported.meshes),.materials=std::move(imported.materials)};
  auto prepared=r.prepareScene(assets);r.waitSceneUpload(prepared);
  require(r.commitScene(prepared),"Kitchen scene did not commit");
  Aabb bounds;
  std::vector<Renderer::DrawItem> opaque,mask,transparent;
  for (auto const &obj:imported.objects) {
    Renderer::DrawItem item{.objectIndex = opaque.size() + mask.size() +
                                           transparent.size(),
                            .meshId = obj.meshId,
                            .materialId = obj.materialId,
                            .modelMatrix = obj.transform.matrix(),
                            .worldBounds = obj.worldBounds};
    auto mode=assets.materials[obj.materialId].alphaMode;
    (mode==AlphaMode::Opaque?opaque:mode==AlphaMode::Mask?mask:transparent).push_back(item);
    if (obj.worldBounds.valid) {
      if (!bounds.valid) bounds=obj.worldBounds;
      else {bounds.min=glm::min(bounds.min,obj.worldBounds.min);bounds.max=glm::max(bounds.max,obj.worldBounds.max);}
    }
  }
  LightingSettings lighting;Camera camera;
  auto center=(bounds.min+bounds.max)*.5f;float radius=glm::length(bounds.max-bounds.min)*.5f;
  camera.farPlane=radius*20;
  lighting.shadowTarget=center;lighting.shadowOrthoExtent=radius*1.1f;
  lighting.shadowLightDistance=radius*2.5f;lighting.shadowNearPlane=radius*.001f;
  lighting.shadowFarPlane=radius*5;
  applyKitchenLightingPreset(lighting,camera);
  std::sort(transparent.begin(),transparent.end(),[&](auto const &a,auto const &b) {
    return glm::length((a.worldBounds.min+a.worldBounds.max)*.5f-camera.position)>
      glm::length((b.worldBounds.min+b.worldBounds.max)*.5f-camera.position);
  });
  auto scene=Renderer::SceneDrawList{.opaque=opaque,.mask=mask,.transparent=transparent,
    .allOpaque=opaque,.allMask=mask,.sky=true};
  std::filesystem::create_directories(directory);
  auto image=[&](char const *name, bool shadows=true) {
    require(r.renderFrame(scene,camera.viewProj(float(swapchain.extent().width)/swapchain.extent().height),camera.position,lighting,shadows)
        ==Renderer::FrameResult::eSuccess,"Kitchen frame failed");
    device.logicalDevice().waitIdle();r.collectCompletedWork();
    auto pixels =
        aoDiagnostic && r.taaActive()
            ? hdrImageSource(device, RendererHdrTestAccess::temporalImage(r),
                             swapchain.extent())
        : aoDiagnostic && r.aoActive()
            ? hdrImageSource(device, RendererHdrTestAccess::aoImage(r),
                             swapchain.extent())
            : hdrImage(device, RendererHdrTestAccess::output(r));
    std::ofstream out(directory/(std::string(name)+".ppm"),std::ios::binary);
    auto size=swapchain.extent();out<<"P6\n"<<size.width<<' '<<size.height<<"\n255\n";
    for (std::size_t i=0;i<pixels.size();i+=4) for(unsigned c=0;c<3;++c) {
      require(std::isfinite(pixels[i+c]),"Kitchen contains invalid HDR pixels");
      float v=aoDiagnostic && r.aoSettings().debug!=AoDebug::None?std::clamp(pixels[i+c],0.f,1.f):filmic(pixels[i+c]);v=v<=.0031308f?v*12.92f:1.055f*std::pow(v,1/2.4f)-.055f;
      out.put(char(std::lround(std::clamp(v,0.f,1.f)*255)));
    }
    if (!std::string_view(name).starts_with("motion_")) {
      std::ofstream raw(directory / (std::string(name) + ".rgba32f"),
                        std::ios::binary);
      raw.write(reinterpret_cast<char const *>(pixels.data()),
                pixels.size() * 4);
    }
    return pixels;
  };
  auto testLights=lighting.punctualLights;
  lighting.punctualLights.clear();lighting.localProbe.enabled=false;
  image("asset_global");
  lighting.punctualLights=testLights;
  image("spots_global");
  lighting.localProbe.enabled=true;
  r.captureLocalProbe(scene,lighting);
  require(r.localProbeValid() && r.spotShadowsAssigned()==2,"Kitchen preset failed to publish probe or two shadows");
  auto local=image("spots_local");
  lighting.localProbe.enabled=false;lighting.pbrDebugMode=8;
  auto globalReflection=image("specular_global");
  lighting.localProbe.enabled=true;
  auto localReflection=image("specular_local");
  double difference=0;for(std::size_t i=0;i<localReflection.size();i+=4)
    for(unsigned c=0;c<3;++c) difference+=std::abs(localReflection[i+c]-globalReflection[i+c]);
  require(difference>10,"Kitchen reflection probe had no observable effect");
  if (aoDiagnostic) {
    lighting.pbrDebugMode = 0;
    lighting.localProbe.enabled = true;
    r.setAoSettings({.enabled = false});
    auto before = image("ao_off");
    r.setAoSettings({.enabled = true});
    auto after = image("ao_on");
    double reduced = 0;
    std::size_t changed = 0;
    for (std::size_t i = 0; i < after.size(); i += 4)
      for (unsigned c = 0; c < 3; ++c) {
        require(after[i + c] <= before[i + c] + .004,
                "AO brightened linear lighting");
        reduced += before[i + c] - after[i + c];
        changed += before[i + c] - after[i + c] > .002;
      }
    require(reduced > 1 && changed > 100, "Kitchen AO did not affect contacts");
    r.setAoSettings({.enabled = true, .radius = .5, .strength = 0});
    auto zero = image("ao_strength_zero");
    for (std::size_t i = 0; i < before.size(); ++i)
      require(std::abs(before[i] - zero[i]) < .004,
              "AO zero strength altered scene");
    r.setAoSettings({.enabled = true, .debug = AoDebug::Raw});
    image("ao_raw");
    r.setAoSettings({.enabled = true, .debug = AoDebug::Filtered});
    image("ao_filtered");
    r.setAoSettings({.enabled = true});
    auto probeBefore = image("ao_probe_before");
    r.captureLocalProbe(scene, lighting);
    auto probeAfter = image("ao_probe_recaptured");
    for (std::size_t i = 0; i < probeBefore.size(); ++i)
      require(std::abs(probeBefore[i] - probeAfter[i]) < .004,
              "AO contaminated static probe");
    r.setTaaEnabled(true);
    auto origin = camera.position;
    for (unsigned path = 0; path < 2; ++path) {
      r.invalidateTemporalHistory();
      for (unsigned frame = 0; frame < 24; ++frame) {
        float t = float(frame) / 23;
        camera.position =
            origin +
            glm::vec3((path ? .8f : .08f) * std::sin(t * glm::pi<float>() * 2),
                      0, 0);
        std::sort(
            transparent.begin(), transparent.end(),
            [&](auto const &a, auto const &b) {
              return glm::length((a.worldBounds.min + a.worldBounds.max) * .5f -
                                 camera.position) >
                     glm::length((b.worldBounds.min + b.worldBounds.max) * .5f -
                                 camera.position);
            });
        auto name = std::string(path ? "motion_fast_" : "motion_slow_") +
                    std::to_string(frame);
        image(name.c_str());
        require(r.gpuTimings().valid && r.gpuTimings().aoMs > 0 &&
                    r.gpuTimings().taaMs > 0,
                "AO/TAA completion timing missing");
      }
    }
    std::cout << "PASS kitchen AO HDR off/on/debug, zero strength/probe "
                 "isolation, 48 deterministic TAA motion frames; reduced="
              << reduced << ", changed channels=" << changed << "\n";
    return;
  }
  if(visibilityDiagnostic) {
    lighting.pbrDebugMode=0;
    auto fullScene=scene;
    std::array<Camera,3> views{camera,camera,camera};
    views[1].position={-1.8f,1.5f,0};views[1].target={1.3f,.9f,1.8f};
    views[2].position={7,5,8};views[2].target={.5f,1.5f,1.5f};
    auto difference=[](auto const &a,auto const &b) {
      double maximum=0;for(std::size_t p=0;p<a.size();p+=4)for(unsigned c=0;c<3;++c)
        maximum=std::max(maximum,double(std::abs(a[p+c]-b[p+c])));
      return maximum;
    };
    bool savedCamera=false,savedShadow=false;
    std::ofstream metrics(directory/"visibility-pairs.csv");
    metrics<<"view,mode,main_draws,shadow_draws,max_linear_hdr_difference\n";
    for(unsigned view=0;view<views.size();++view) {
      camera=views[view];
      std::sort(transparent.begin(),transparent.end(),[&](auto const &a,auto const &b) {
        return glm::length((a.worldBounds.min+a.worldBounds.max)*.5f-camera.position)>
          glm::length((b.worldBounds.min+b.worldBounds.max)*.5f-camera.position);
      });
      scene=fullScene;scene.allTransparent=transparent;
      r.setShadowCasterCullingEnabled(false);
      auto prefix=std::string("visibility_")+std::to_string(view);
      auto reference=image((prefix+"_full").c_str());auto full=r.drawStatistics();
      metrics<<view<<",full,"<<full.main<<','<<full.shadow<<",0\n";
      std::array<std::vector<Renderer::DrawItem>,3> visible;
      auto extent=swapchain.extent();
      auto frustum=extractFrustum(camera.viewProj(float(extent.width)/extent.height),
          {1.f/extent.width,1.f/extent.height});
      std::array lists{fullScene.opaque,fullScene.mask,fullScene.transparent};
      for(unsigned list=0;list<lists.size();++list)
        for(auto const &item:lists[list])if(intersectsFrustum(frustum,item.worldBounds))visible[list].push_back(item);
      for(unsigned mode=1;mode<=3;++mode) {
        scene=fullScene;scene.allTransparent=transparent;
        if(mode!=2){scene.opaque=visible[0];scene.mask=visible[1];scene.transparent=visible[2];}
        r.setShadowCasterCullingEnabled(mode!=1);
        auto result=image((prefix+"_"+std::to_string(mode)).c_str());auto draws=r.drawStatistics();
        auto error=difference(reference,result);
        metrics<<view<<','<<mode<<','<<draws.main<<','<<draws.shadow<<','<<error<<'\n';
        require(error<.001,"Kitchen culling changed visible HDR");
        savedCamera|=draws.main<full.main;savedShadow|=draws.shadow<full.shadow;
      }
    }
    camera=views[0];
    std::sort(transparent.begin(),transparent.end(),[&](auto const &a,auto const &b) {
      return glm::length((a.worldBounds.min+a.worldBounds.max)*.5f-camera.position)>
        glm::length((b.worldBounds.min+b.worldBounds.max)*.5f-camera.position);
    });
    scene=fullScene;scene.allTransparent=transparent;
    r.setShadowCasterCullingEnabled(false);
    auto probeReference=image("visibility_probe_before");
    r.setShadowCasterCullingEnabled(true);r.captureLocalProbe(scene,lighting);
    auto probeCulled=image("visibility_probe_after");
    require(difference(probeReference,probeCulled)<.001,"Culling changed recaptured room-probe HDR");
    require(savedCamera && savedShadow,"Kitchen culling saved no camera/shadow work");
    std::cout<<"PASS three kitchen views: camera/shadow/combined culling HDR pairs and draw savings\n";
  }
  if(colourDiagnostic) {
    lighting.localProbe.enabled=true;lighting.pbrDebugMode=1;image("base_colour");
    lighting.pbrDebugMode=14;image("direct_warm");
    lighting.pbrDebugMode=7;image("diffuse_probe_warm");
    lighting.pbrDebugMode=0;image("all_no_shadows",false);
    lighting.sunEnabled=false;lighting.punctualLights.clear();image("frozen_probe_no_direct");
    lighting.pbrDebugMode=7;auto frozen=image("diffuse_probe_after_lights_off");
    lighting.sunEnabled=true;lighting.punctualLights=testLights;
    lighting.color={1,1,1};for(auto &light:lighting.punctualLights)light.color={1,1,1};
    lighting.environmentIntensity=0;lighting.pbrDebugMode=14;image("direct_neutral");
    lighting.environmentIntensity=1;lighting.pbrDebugMode=7;auto stale=image("diffuse_stale_probe_neutral_lights");
    require(!r.localProbeMatches(lighting),"Colour diagnostic did not flag changed-light static probe");
    double delta=0;for(std::size_t i=0;i<frozen.size();++i)delta+=std::abs(frozen[i]-stale[i]);
    require(delta<.001,"Changing direct lights unexpectedly mutated static captured IBL");
    r.captureLocalProbe(scene,lighting);image("diffuse_refreshed_neutral_probe");
    lighting.pbrDebugMode=0;image("all_neutral_refreshed");
    for(auto &light:lighting.punctualLights)light.intensity*=8;
    image("strong_white_direct_old_probe");
    std::cout<<"COLOUR diagnostic: changing direct lights leaves raw diffuse-probe RGB unchanged, delta="<<delta<<"; refreshed/neutral/base/shadow controls exported\n";
  }
  std::cout<<"PASS full kitchen: "<<opaque.size()<<" opaque, "<<mask.size()<<" mask, "<<transparent.size()
    <<" blend, 2 spot shadows, local reflection HDR difference="<<difference<<"; images="<<directory<<std::endl;
}

void exerciseVisibilityCulling(Device const &device, SwapChain const &swapchain,
                               unsigned slots) {
  Renderer r(device,slots);r.recreateForSwapChain(swapchain);
  AssetLibrary assets;
  Mesh mesh;
  for(auto position : {glm::vec3{-1,-1,0},glm::vec3{1,-1,0},
                       glm::vec3{1,1,0},glm::vec3{-1,1,0}})
    mesh.vertices.push_back(Vertex{.position=position,.color={1,1,1},.normal={0,0,1}});
  mesh.indices={0,1,2,0,2,3};assets.meshes.push_back(mesh);
  Material material;material.doubleSided=true;material.specularFactor=0;
  assets.materials.push_back(material);
  auto masked=material;masked.alphaMode=AlphaMode::Mask;assets.materials.push_back(masked);
  auto blended=material;blended.alphaMode=AlphaMode::Blend;blended.tint.a=.5f;assets.materials.push_back(blended);
  auto prepared=r.prepareScene(assets);r.waitSceneUpload(prepared);
  require(r.commitScene(prepared),"Visibility scene failed to commit");
  auto object=[&](glm::vec3 position,glm::vec3 scale,std::size_t identity) {
    Renderer::DrawItem item{.objectIndex=identity,.meshId=0,.materialId=0,
      .modelMatrix=glm::translate(glm::mat4{1},position)*glm::scale(glm::mat4{1},scale)};
    for(auto const &vertex:mesh.vertices) {
      auto p=glm::vec3(item.modelMatrix*glm::vec4(vertex.position,1));
      if(!item.worldBounds.valid)item.worldBounds={p,p,true};
      else {item.worldBounds.min=glm::min(item.worldBounds.min,p);item.worldBounds.max=glm::max(item.worldBounds.max,p);}
    }
    return item;
  };
  std::array all{object({0,0,-1},{1,1,1},0),
                 object({.5f,0,-.3f},{.08f,.08f,1},1),
                 object({50,0,-1},{-1,2,1},2)};
  Camera camera;camera.position={0,0,0};camera.target={0,0,-1};camera.nearPlane=.1f;camera.farPlane=5;
  LightingSettings lighting;lighting.environmentIntensity=0;lighting.color={1,1,1};
  lighting.direction=glm::normalize(glm::vec3{1,0,1});lighting.shadowPcfRadius=0;
  lighting.sunCascades.count=2;lighting.sunCascades.distance=4;lighting.sunCascades.casterDistance=6;
  auto vp=camera.viewProj(float(swapchain.extent().width)/swapchain.extent().height);
  auto draw=[&](std::span<Renderer::DrawItem const> visible,std::span<Renderer::DrawItem const> casters,bool cull) {
    r.setShadowCasterCullingEnabled(cull);
    require(r.renderFrame({.opaque=visible,.allOpaque=casters,.sky=false},vp,camera.position,lighting,true)
        ==Renderer::FrameResult::eSuccess,"Visibility frame failed");
    device.logicalDevice().waitIdle();r.collectCompletedWork();
    return hdrImage(device,RendererHdrTestAccess::output(r));
  };
  auto delta=[](auto const &a,auto const &b) {
    double sum=0;for(std::size_t i=0;i<a.size();i+=4)for(unsigned c=0;c<3;++c)sum+=std::abs(a[i+c]-b[i+c]);
    return sum;
  };
  auto reference=draw(all,all,false);auto fullDraws=r.drawStatistics().shadow;
  std::array withoutBlocker{all[0],all[2]};
  auto missingShadow=draw(withoutBlocker,withoutBlocker,false);
  require(delta(reference,missingShadow)>1,"Off-camera caster fixture did not affect the visible receiver");
  auto shadowCulled=draw(all,all,true);auto culledDraws=r.drawStatistics().shadow;
  require(delta(reference,shadowCulled)<.001,"Shadow caster culling changed visible HDR");
  require(culledDraws<fullDraws,"Shadow culling retained all candidates");
  std::vector<Renderer::DrawItem> visible;
  auto frustum=extractFrustum(vp,{1.f/swapchain.extent().width,1.f/swapchain.extent().height});
  for(auto const &item:all)if(intersectsFrustum(frustum,item.worldBounds))visible.push_back(item);
  require(visible.size()==1,"Camera frustum failed to remove off-camera objects");
  auto combined=draw(visible,all,true);
  require(delta(reference,combined)<.001,"Camera culling lost an off-camera object's shadow");
  auto known=r.drawStatistics().shadow;
  lighting.shadowDebugMode=3;lighting.sunCascades.debugIndex=1;
  auto debugReference=draw(all,all,false);auto debugCulled=draw(all,all,true);
  require(delta(debugReference,debugCulled)<.001 && r.drawStatistics().shadow>known,
      "Receiver culling removed a tile used by shadow debugging");
  lighting.shadowDebugMode=1;
  all[2].worldBounds.valid=false;auto unknown=draw(visible,all,true);
  require(r.drawStatistics().shadow>known,"Missing caster bounds were not kept conservatively");
  require(delta(reference,unknown)<.001,"Conservative unknown bounds changed HDR");
  all[2]=object({50,0,-1},{-1,2,1},2);
  std::array casters{all[1],all[2]};
  for(unsigned mode : {1u,2u}) {
    auto receiver=all[0];receiver.materialId=mode;
    std::array receivers{receiver};
    auto mixed=[&](bool cull) {
      r.setShadowCasterCullingEnabled(cull);
      Renderer::SceneDrawList list{.allOpaque=casters,.sky=false};
      if(mode==1){list.mask=receivers;list.allMask=receivers;}
      else {list.transparent=receivers;list.allTransparent=receivers;}
      require(r.renderFrame(list,vp,camera.position,lighting,true)==Renderer::FrameResult::eSuccess,
          "Mask/blend receiver frame failed");
      device.logicalDevice().waitIdle();r.collectCompletedWork();
      return hdrImage(device,RendererHdrTestAccess::output(r));
    };
    auto mixedReference=mixed(false);auto mixedCulled=mixed(true);
    require(delta(mixedReference,mixedCulled)<.001,"Mask/blend receiver culling changed HDR");
  }
  std::cout<<"PASS visibility: shadow draws "<<fullDraws<<" -> "<<culledDraws
      <<", visible objects 3 -> 1, off-camera shadow/unknown bounds preserved\n";
}

void exerciseSunCascades(Device const &device, SwapChain const &swapchain,
                         unsigned slots) {
  auto before=device.resourceLedger().snapshot().current;
  {
    Renderer r(device,slots);r.recreateForSwapChain(swapchain);
    AssetLibrary assets;assets.meshes.push_back(materialTriangle());
    for(auto &v:assets.meshes[0].vertices) v.position.z=0; // receiver depth equals its transform
    Material diffuse;diffuse.doubleSided=true;diffuse.specularFactor=0;
    assets.materials.push_back(diffuse);
    auto prepared=r.prepareScene(assets);r.waitSceneUpload(prepared);
    require(r.commitScene(prepared),"CSM fixture commit failed");
    Camera camera;camera.position={0,0,0};camera.target={0,0,-1};
    camera.nearPlane=.1f;camera.farPlane=100;
    LightingSettings light;light.direction={0,0,1};light.color={1,1,1};
    light.environmentIntensity=0;light.shadowPcfRadius=0;light.pbrDebugMode=14;
    light.sunCascades.distance=40;
    std::array receiver{Renderer::DrawItem{.meshId=0,.materialId=0}};
    std::array casters{receiver[0],receiver[0]};
    auto place=[&](float depth) {
      auto scale=glm::scale(glm::mat4(1),glm::vec3{depth,depth,1});
      receiver[0].modelMatrix=glm::translate(glm::mat4(1),glm::vec3{0,0,-depth})*scale;
      casters[0]=receiver[0];
      casters[1].modelMatrix=glm::translate(glm::mat4(1),glm::vec3{0,0,.5f-depth})*scale;
    };
    auto draw=[&](std::span<Renderer::DrawItem const> all,bool shadows=true) {
      require(r.renderFrame({.opaque=receiver,.allOpaque=all,.sky=false},
          camera.viewProj(float(swapchain.extent().width)/swapchain.extent().height),
          camera.position,light,shadows)==Renderer::FrameResult::eSuccess,"CSM frame failed");
      device.logicalDevice().waitIdle();r.collectCompletedWork();
      return hdrPixel(device,RendererHdrTestAccess::output(r),
          {int(swapchain.extent().width/2),int(swapchain.extent().height/2),0});
    };
    auto config=buildSunCascades(camera.viewProj(float(swapchain.extent().width)/swapchain.extent().height),
        camera.position,light.direction,light.sunCascades,0);
    for(unsigned i=0;i<4;++i) {
      float start=i?config.splits[i-1]:camera.nearPlane;
      place((start+config.splits[i])*.5f);
      auto open=draw(receiver),blocked=draw(casters),disabled=draw(casters,false);
      require(open[0]>.3f && blocked[0]<.001f && std::abs(open[0]-disabled[0])<.001f,
          "A sun cascade failed to block direct light or preserve disabled shading");
      draw(casters);require(r.sunCascadesAssigned()==4,"CSM silently fell back to legacy");
      std::cout<<"CSM level "<<i<<": open="<<open[0]<<" blocked="<<blocked[0]<<std::endl;
    }
    // Both maps must contain the receiving/casting geometry throughout the blend.
    for(unsigned i=0;i<3;++i) {
      float previous=i?config.splits[i-1]:camera.nearPlane;
      float band=(config.splits[i]-previous)*light.sunCascades.blendFraction;
      for(float t:{.05f,.5f,.95f}) {
        place(config.splits[i]-band*(1-t));
        auto blocked=draw(casters);
        require(blocked[0]<.001f,"Cascade overlap leaks during dual-map blend");
      }
    }
    light.shadowDebugMode=2;
    // A caster between the two light-space near planes shadows exactly one
    // map. Unlike two equally dark maps, this makes ignored blending fail.
    auto upstream=[](glm::mat4 m) { auto p=glm::inverse(m)*glm::vec4{0,0,0,1};return p.z/p.w; };
    float z0=upstream(config.viewProj[0]), z1=upstream(config.viewProj[1]);
    require(std::abs(z0-z1)>.1f,"Blend fixture has no distinct clip limits");
    float band=(config.splits[0]-camera.nearPlane)*light.sunCascades.blendFraction;
    for(float t:{.25f,.5f,.75f}) {
      float depth=config.splits[0]-band*(1-t);place(depth);
      casters[1].modelMatrix=glm::translate(glm::mat4(1),glm::vec3{0,0,(z0+z1)*.5f})*
          glm::scale(glm::mat4(1),glm::vec3{depth,depth,1});
      auto value=draw(casters)[0];float expected=z0>z1?t:1-t;
      std::cout<<"CSM distinct-map blend weight="<<t<<" visibility="<<value<<std::endl;
      require(std::abs(value-expected)<.01f,"Distinct cascade samples were not blended linearly");
    }
    light.shadowDebugMode=1;light.pbrDebugMode=14;place(8);
    receiver[0].modelMatrix = glm::translate(glm::mat4(1),glm::vec3{0,0,-8})*
        glm::rotate(glm::mat4(1),glm::radians(30.f),glm::vec3{0,1,0})*
        glm::scale(glm::mat4(1),glm::vec3{8,8,1});
    for(float radius:{0.f,1.f,4.f}) {
      light.shadowPcfRadius=radius;
      float open=draw({})[0],self=draw(receiver)[0];
      std::cout<<"CSM tilted self-shadow PCF="<<radius<<": open="<<open<<" self="<<self<<std::endl;
      require(self>open*.99f,"Tilted receiver has PCF self-shadow acne");
    }
    light.shadowPcfRadius=0;light.shadowDebugMode=2;
    place(38.9f); // last blend band is ~2.24; visibility halfway to white.
    auto fade=draw(casters);
    require(fade[0]>.3f && fade[0]<.8f,"Last cascade did not fade smoothly to visibility");
    place(41);require(draw(casters)[0]>.99f,"Outside shadow distance extinguishes direct light");
    light.shadowDebugMode=1;light.pbrDebugMode=17;place(2);
    auto color=draw(receiver);
    require(color[0]>.99f && color[1]<.16f,"Cascade color debug not linear/raw");
    require(!RendererHdrTestAccess::settings(r).toneMap,"Cascade debug was tone mapped");
    light.pbrDebugMode=14;place(8);
    // The caster is behind the eye and absent from main visibility; still casts.
    casters[1].modelMatrix=glm::translate(glm::mat4(1),glm::vec3{0,0,2})*
        glm::scale(glm::mat4(1),glm::vec3{8,8,1});
    require(draw(casters)[0]<.001f,"Off-camera upstream caster dropped from CSM");
    casters[1].modelMatrix=glm::translate(glm::mat4(1),glm::vec3{100,0,2});
    require(draw(casters)[0]>.3f,"Moving upstream caster retained stale depth");
    // Sun disabled still permits shadowed spots in the right half of the atlas.
    place(2);light.sunEnabled=false;
    PunctualLight spot;spot.type=PunctualLightType::Spot;spot.position={0,0,0};
    spot.direction={0,0,-1};spot.intensity=10;spot.castsShadow=true;
    light.punctualLights={spot};
    require(draw(receiver)[0]>.5f && draw(casters)[0]<.001f && r.sunCascadesAssigned()==0 &&
        r.spotShadowsAssigned()==1,"Sun disable disturbed local atlas shadows");
    light.sunEnabled=true;
    light.punctualLights.clear();light.sunCascades.enabled=false;
    light.shadowTarget={0,0,-2};light.shadowLightDistance=4;light.shadowOrthoExtent=4;
    require(draw(casters)[0]<.001f && r.sunCascadesAssigned()==0,"Legacy comparison shadow broken");
    light.sunCascades.enabled=true;
    auto id=r.submittedFrameId();auto invalid=light;invalid.sunCascades.count=3;
    bool rejected=false;
    try {r.renderFrame({.opaque=receiver,.allOpaque=casters,.sky=false},camera.viewProj(1),
        camera.position,invalid,true);}catch(std::runtime_error const &){rejected=true;}
    require(rejected && r.submittedFrameId()==id,"Invalid CSM acquired/submitted a frame");
    draw(casters);require(r.sunCascadesAssigned()==4,"Valid retry after invalid CSM failed");
    require(r.drawStatistics().shadow==8,"CSM did not retain complete caster lists per cascade");
    std::cout<<"PASS CSM GPU: four levels, overlap, far fade, upstream/moving caster, legacy/spot/disabled, invalid retry, fade="<<fade[0]<<std::endl;
  }
  require(device.resourceLedger().snapshot().current==before,"CSM leaked GPU resources");
}
void exerciseKitchenGeometry(Device const &device, SwapChain const &swapchain,
                             unsigned slots, std::filesystem::path const &path,
                             std::filesystem::path const &directory,
                             bool repaired) {
  auto imported = loadStaticGltfScene(path, {});
  require(imported.meshes.size() == 299 && imported.materials.size() == 90,
          "Unexpected pinned kitchen layout");
  if (repaired) {
    auto repair = prepareImportedSceneForViewer(imported);
    std::cout << "Viewer kitchen repair: zero=" << repair.zeroArea
              << " duplicates=" << repair.duplicates
              << " light cards=" << repair.lightCards << std::endl;
    require(repair.zeroArea == 219 && repair.duplicates == 6 &&
                repair.lightCards == 4,
            "Pinned kitchen cleanup/role counts differ");
  }
  AssetLibrary assets{.meshes = imported.meshes,
                      .materials = imported.materials};
  Renderer r(device, slots);
  r.recreateForSwapChain(swapchain);
  r.setTaaEnabled(false);
  r.setAoSettings({.enabled = false});
  std::filesystem::create_directories(directory);
  auto prepare = [&](AssetLibrary const &a) {
    auto candidate = r.prepareScene(a);
    r.waitSceneUpload(candidate);
    require(r.commitScene(candidate), "Geometry diagnostic commit failed");
  };
  std::ofstream metrics(directory / "gpu-controls.csv");
  metrics << "name,center_r,center_g,center_b\n";
  auto image = [&](std::string const &name,
                   Renderer::SceneDrawList const &scene, Camera const &camera,
                   LightingSettings const &lighting) {
    require(r.renderFrame(scene,
                          camera.viewProj(float(swapchain.extent().width) /
                                          swapchain.extent().height),
                          camera.position, lighting,
                          false) == Renderer::FrameResult::eSuccess,
            "Geometry diagnostic frame failed");
    device.logicalDevice().waitIdle();
    r.collectCompletedWork();
    auto pixels = hdrImage(device, RendererHdrTestAccess::output(r));
    auto size = swapchain.extent();
    std::ofstream out(directory / (name + ".ppm"), std::ios::binary);
    out << "P6\n" << size.width << ' ' << size.height << "\n255\n";
    for (std::size_t i = 0; i < pixels.size(); i += 4)
      for (unsigned c = 0; c < 3; ++c) {
        float v = lighting.pbrDebugMode == 4
                      ? std::clamp(pixels[i + c], 0.f, 1.f)
                      : filmic(pixels[i + c]);
        v = v <= .0031308f ? 12.92f * v
                           : 1.055f * std::pow(v, 1 / 2.4f) - .055f;
        out.put(char(std::lround(std::clamp(v, 0.f, 1.f) * 255)));
      }
    std::ofstream raw(directory / (name + ".rgba32f"), std::ios::binary);
    raw.write(reinterpret_cast<char const *>(pixels.data()), pixels.size() * 4);
    auto center =
        4 * (std::size_t(size.height / 2) * size.width + size.width / 2);
    metrics << name << ',' << pixels[center] << ',' << pixels[center + 1] << ','
            << pixels[center + 2] << '\n';
    return pixels;
  };
  auto center = [&](auto const &p) {
    auto s = swapchain.extent();
    auto k = 4 * (std::size_t(s.height / 2) * s.width + s.width / 2);
    return glm::vec3{p[k], p[k + 1], p[k + 2]};
  };
  LightingSettings dark;
  dark.sunEnabled = false;
  dark.environmentIntensity = 0;
  Camera inside;
  inside.position = {.06f, 1.9f, -1.5f};
  inside.target = {.06f, 1.9f, -3.1173f};
  inside.nearPlane = .02f;
  inside.farPlane = 40;
  Camera outside = inside;
  outside.position = {.06f, 1.9f, -4.7f};
  AssetLibrary emitter;
  emitter.meshes.push_back(assets.meshes[295]);
  emitter.materials.push_back(assets.materials[86]);
  require(!emitter.materials[0].doubleSided &&
              emitter.materials[0].alphaMode == AlphaMode::Opaque,
          "Window emitter material changed");
  prepare(emitter);
  std::array one{Renderer::DrawItem{.objectIndex = 0}};
  Renderer::SceneDrawList emitterScene{
      .opaque = one, .allOpaque = one, .sky = false};
  auto front = image("window_emitter_inside", emitterScene, inside, dark);
  auto back = image("window_emitter_outside", emitterScene, outside, dark);
  require(glm::length(center(front) - glm::vec3(1)) < .002f,
          "Emitter front did not cover window center");
  require(glm::length(center(back) - glm::vec3(.05, .07, .1)) < .002f,
          "Emitter back was not culled");
  emitter.materials[0].doubleSided = true;
  prepare(emitter);
  auto twoSided =
      image("window_emitter_two_sided_control", emitterScene, outside, dark);
  require(glm::length(center(twoSided) - glm::vec3(1)) < .002f,
          "Emitter control did not render both sides");
  if (repaired) {
    one[0].primaryVisible = false;
    one[0].shadowCaster = false;
    auto hidden = image("window_hidden_light_card", emitterScene, inside, dark);
    require(glm::length(center(hidden) - glm::vec3(.05, .07, .1)) < .002,
            "Light-card primary visibility flag ignored");
    auto capture = dark;
    capture.localProbe = {true, {-4, -4, -4}, {4, 4, 4}, {0, 0, 0}};
    r.captureLocalProbe(emitterScene, capture);
    require(RendererHdrTestAccess::probeEnergy(r) > .01f,
            "Hidden light card lost probe illumination");
    auto shadowControl = capture;
    shadowControl.sunEnabled = true;
    r.setShadowCasterCullingEnabled(false);
    require(r.renderFrame(emitterScene, inside.viewProj(1), inside.position,
                          shadowControl,
                          true) == Renderer::FrameResult::eSuccess,
            "Light-card shadow control failed");
    device.logicalDevice().waitIdle();
    r.collectCompletedWork();
    require(r.drawStatistics().main == 0 && r.drawStatistics().shadow == 0,
            "Light card remained primary/shadow geometry");
    r.setShadowCasterCullingEnabled(true);
    one[0].primaryVisible = true;
    one[0].shadowCaster = true;
  }
  prepare(assets);
  std::vector<Renderer::DrawItem> opaque, blend, noEmitter;
  for (unsigned i = 0; i < imported.objects.size(); ++i) {
    auto const &o = imported.objects[i];
    Renderer::DrawItem item{.objectIndex = i,
                            .meshId = o.meshId,
                            .materialId = o.materialId,
                            .modelMatrix = o.transform.matrix(),
                            .worldBounds = o.worldBounds};
    item.primaryVisible = o.primaryVisible;
    item.shadowCaster = o.shadowCaster;
    (assets.materials[o.materialId].alphaMode == AlphaMode::Blend ? blend
                                                                  : opaque)
        .push_back(item);
    if (assets.materials[o.materialId].alphaMode != AlphaMode::Blend &&
        o.meshId != 295)
      noEmitter.push_back(item);
  }
  auto sort = [&](Camera const &c) {
    std::sort(blend.begin(), blend.end(), [&](auto const &a, auto const &b) {
      return glm::length((a.worldBounds.min + a.worldBounds.max) * .5f -
                         c.position) >
             glm::length((b.worldBounds.min + b.worldBounds.max) * .5f -
                         c.position);
    });
  };
  Renderer::SceneDrawList scene{.opaque = opaque,
                                .transparent = blend,
                                .allOpaque = opaque,
                                .allTransparent = blend,
                                .sky = true};
  LightingSettings lighting;
  Camera preset;
  applyKitchenLightingPreset(lighting, preset);
  sort(inside);
  auto full = image("window_full_inside", scene, inside, lighting);
  if (!repaired)
    require(glm::all(glm::greaterThanEqual(center(full), glm::vec3(.99f))),
            "Full kitchen window center did not see emitter radiance");
  auto windowDepth = [&] {
    auto size = swapchain.extent();
    auto bytes = pixel(device, RendererHdrTestAccess::depth(r),
                       RendererHdrTestAccess::depthState(r).layout,
                       {int(size.width / 2), int(size.height / 2), 0},
                       sizeof(float), vk::ImageAspectFlagBits::eDepth);
    float d;
    std::memcpy(&d, bytes.data(), sizeof(d));
    return d;
  };
  float emitterDepth = windowDepth();
  auto without = scene;
  without.opaque = noEmitter;
  without.allOpaque = noEmitter;
  auto removed =
      image("window_without_emitter_control", without, inside, lighting);
  require((repaired ? emitterDepth > .999f : emitterDepth < .999f) &&
              windowDepth() > .999f,
          "Window primary depth still blocked by light card");
  sort(outside);
  image("window_full_outside", scene, outside, lighting);
  Camera micro = preset;
  micro.position = {-.75f, 1.85f, -1.95f};
  micro.target = {-2.2f, 1.14f, -1.95f};
  micro.fovRadians = glm::radians(35.f);
  micro.nearPlane = .02f;
  micro.farPlane = 40;
  sort(micro);
  r.captureLocalProbe(scene, lighting);
  lighting.localProbe.enabled = true;
  if (repaired) {
    require(r.detailReflectionProbeValid(),
            "Microwave detail probe did not publish");
    auto opaqueOnly = scene;
    opaqueOnly.transparent = {};
    lighting.pbrDebugMode = 8;
    lighting.detailReflectionProbe.enabled = false;
    auto leaking = image("microwave_room_only_failure_control", opaqueOnly,
                         micro, lighting);
    lighting.detailReflectionProbe.enabled = true;
    auto protectedSpec =
        image("microwave_detail_specular", opaqueOnly, micro, lighting);
    if (slots == 2) {
      auto latest = unsigned((r.submittedFrameId() - 1) % 2);
      auto a = RendererHdrTestAccess::indoor(r, latest),
           b = RendererHdrTestAccess::indoor(r, 1 - latest);
      require((a.counts.w & 4u) != 0 && (b.counts.w & 4u) == 0 &&
                  a.detailMin ==
                      glm::vec4(lighting.detailReflectionProbe.minimum, 0),
              "Detail probe SSBO flags/bounds aliased between slots");
    }

    std::cout << "Microwave isolated specular: room="
              << glm::length(center(leaking))
              << " detail=" << glm::length(center(protectedSpec)) << std::endl;
    require(glm::length(center(protectedSpec)) <
                glm::length(center(leaking)) * .5f,
            "Microwave still samples room Floor instead of cavity");
    lighting.pbrDebugMode = 14;
    lighting.detailReflectionProbe.enabled = false;
    auto directA = image("microwave_direct_room", scene, micro, lighting);
    lighting.detailReflectionProbe.enabled = true;
    auto directB = image("microwave_direct_detail", scene, micro, lighting);
    for (std::size_t i = 0; i < directA.size(); ++i)
      require(directA[i] == directB[i],
              "Detail reflection changed direct lighting");
    auto oldId = r.submittedFrameId();
    auto invalid = lighting;
    invalid.detailReflectionProbe.position =
        invalid.detailReflectionProbe.minimum;
    bool rejected = false;
    try {
      r.captureLocalProbe(scene, invalid);
    } catch (std::runtime_error const &) {
      rejected = true;
    }
    require(rejected && r.detailReflectionProbeValid() &&
                r.submittedFrameId() == oldId,
            "Failed detail capture invalidated live probe/history");
    lighting.pbrDebugMode = 0;
  }
  auto local = image("microwave_local_all", scene, micro, lighting);
  lighting.pbrDebugMode = 4;
  image("microwave_normals", scene, micro, lighting);
  lighting.pbrDebugMode = 8;
  auto spec = image("microwave_local_specular", scene, micro, lighting);
  lighting.localProbe.enabled = false;
  auto global =
      image("microwave_global_specular_control", scene, micro, lighting);
  lighting.localProbe.enabled = true;
  lighting.pbrDebugMode = 0;
  lighting.environmentSpecularStrength = 0;
  auto noSpec = image("microwave_no_specular_control", scene, micro, lighting);
  double difference = 0, sourceDifference = 0;
  for (std::size_t i = 0; i < local.size(); i += 4)
    for (unsigned c = 0; c < 3; ++c) {
      difference += std::abs(local[i + c] - noSpec[i + c]);
      sourceDifference += std::abs(spec[i + c] - global[i + c]);
    }
  require(difference > 10 && sourceDifference > 10,
          "Microwave specular/probe source controls had no observable effect");
  std::cout
      << "PASS window actual GPU: opaque single-sided emitter front="
      << center(front).x << ", back=" << center(back).x
      << ", two-sided control=" << center(twoSided).x
      << "; full scene/removal matches. Microwave specular-off difference="
      << difference << ", local/global source difference=" << sourceDifference
      << std::endl;
}


void exerciseDielectricRaster(Device const &device, SwapChain const &swapchain,
                              unsigned slots) {
  auto before=device.resourceLedger().snapshot().current;
  {
    Renderer r(device,slots);
    r.recreateForSwapChain(swapchain);
    r.setTaaEnabled(false);
    r.setAoSettings({.enabled=false});
    HdrImage constant{.width=8,.height=4,.rgba=std::vector<float>(8*4*4,1)};
    auto bake=bakeEnvironment(constant,{.faceSize=8,.prefilterSamples=32,
                                        .lutSize=8,.lutSamples=64});
    std::array colors{glm::vec3(1,0,0),glm::vec3(0,1,0),glm::vec3(0,0,1),
                      glm::vec3(1,1,0),glm::vec3(2,3,4),glm::vec3(8,5,2)};
    for(auto level:bake.levels)
      for(unsigned f=0;f<6;++f)
        for(unsigned i=0;i<level.size*level.size;++i)
          for(unsigned c=0;c<3;++c)
            bake.cubeRgba[level.offset+(f*level.size*level.size+i)*4+c]=colors[f][c];
    RendererHdrTestAccess::environment(r,bake);
    Mesh plane;
    for(auto pos:{glm::vec3(-1,-1,.5),glm::vec3(3,-1,.5),glm::vec3(-1,3,.5)})
      plane.vertices.push_back(Vertex{.position=pos,.color={1,1,1},.normal={0,0,1}});
    plane.indices={0,1,2};
    Mesh box;
    for(auto pos:{glm::vec3(-2,-2,.4),glm::vec3(2,-2,.4),glm::vec3(2,2,.4),glm::vec3(-2,2,.4),
                  glm::vec3(-2,-2,.5),glm::vec3(2,-2,.5),glm::vec3(2,2,.5),glm::vec3(-2,2,.5)})
      box.vertices.push_back(Vertex{.position=pos,.color={1,1,1},.normal={0,0,1}});
    // Opposite triangle orders must select the same nearest optical surface.
    box.indices={0,2,1,0,3,2,0,1,5,0,5,4,1,2,6,1,6,5,2,3,7,2,7,6,3,0,4,3,4,7,4,5,6,4,6,7};
    require(classifyDielectricMesh(box).solid,"Raster optical box is not closed");
    AssetLibrary assets;
    assets.meshes={plane,box};
    auto reordered=box;
    std::rotate(reordered.indices.begin(),reordered.indices.end()-6,reordered.indices.end());
    assets.meshes.push_back(reordered);
    Material m;
    m.doubleSided=true;m.alphaMode=AlphaMode::Blend;m.metallicFactor=0;
    m.roughnessFactor=.04f;m.tint.a=.35f;
    m.optical.enabled=true;m.optical.coverage=0;
    assets.materials.push_back(m);
    m.optical.ior=1;m.optical.thickness=.1;m.optical.absorption={1,2,3};
    assets.materials.push_back(m);
    m.optical.ior=1.5;m.optical.solid=true;m.optical.absorption={0,0,0};
    assets.materials.push_back(m);
    auto candidate=r.prepareScene(assets);r.waitSceneUpload(candidate);
    require(r.commitScene(candidate),"Raster glass commit failed");
    LightingSettings l;l.sunEnabled=false;l.intensity=0;l.environmentIntensity=1;
    l.environmentSpecularStrength=1;l.environmentDiffuseStrength=0;
    // Depth decreases toward the +Z camera, matching the optical view vector.
    glm::mat4 vp(1);vp[1][1]=-1;vp[2][2]=-1;vp[3][2]=1;
    auto center=vk::Offset3D{int(swapchain.extent().width/2),int(swapchain.extent().height/2),0};
    auto draw=[&](unsigned mesh,unsigned material,glm::vec3 v){
      require(r.beginFrame(vp,glm::vec3(0,0,.5)+1000.f*v,l,false)==Renderer::FrameResult::eSuccess,
              "Raster glass begin failed");
      r.drawObject(mesh,material,glm::mat4(1));r.endFrame();
      device.logicalDevice().waitIdle();r.collectCompletedWork();
      auto p=hdrPixel(device,RendererHdrTestAccess::output(r),center);
      return glm::vec3(p[0],p[1],p[2]);
    };
    auto close=[&](glm::vec3 actual,glm::vec3 expected){
      if(glm::length(actual-expected)>.025)
        std::cerr<<"Raster glass actual "<<actual.x<<' '<<actual.y<<' '<<actual.z
                 <<" expected "<<expected.x<<' '<<expected.y<<' '<<expected.z<<'\n';
      require(glm::length(actual-expected)<.025,"Raster shared IOR/Beer/cube direction differs");
    };
    double f=.08/1.04;
    close(draw(0,0,{0,0,1}),float(f)*colors[4]+float(1-f)*colors[5]);
    close(draw(0,1,{0,0,1}),colors[5]*glm::vec3(std::exp(-.1),std::exp(-.2),std::exp(-.3)));
    glm::vec3 oblique{std::sqrt(.75f),0,.5};
    float Fs=dielectricFresnel(.5,1,1.5);
    float Ft=2*Fs/(1+Fs);
    close(draw(0,0,oblique),Ft*colors[1]+(1-Ft)*colors[1]);
    close(draw(1,2,oblique),Fs*colors[1]+(1-Fs)*colors[5]);
    close(draw(2,2,oblique),Fs*colors[1]+(1-Fs)*colors[5]);
    // A requested solid on the open plane must use the thin direction.
    close(draw(0,2,oblique),colors[1]);
    std::array one{Renderer::DrawItem{.objectIndex=0,.meshId=0,.materialId=0}};
    Renderer::SceneDrawList scene{.transparent=one,.allTransparent=one,.sky=false};
    l.localProbe={true,{-2,-2,-2},{2,2,2},{0,0,0}};
    r.captureLocalProbe(scene,l);
    float capture=RendererHdrTestAccess::probeEnergy(r);
    for(unsigned i=0;i<bake.cubeRgba.size();++i)if(i%4!=3)bake.cubeRgba[i]*=10;
    RendererHdrTestAccess::environment(r,bake);
    r.captureLocalProbe(scene,l);
    require(std::abs(RendererHdrTestAccess::probeEnergy(r)-capture)<1e-5,
            "Glass probe capture recursively sampled environment/probe lighting");
    std::cout<<"PASS raster glass: shared Fresnel/IOR/Beer, thin vs solid probe direction, "
                 "open fallback, legacy optical alpha and nonrecursive capture\n";
  }
  require(device.resourceLedger().snapshot().current==before,"Raster glass resources leaked");
}
