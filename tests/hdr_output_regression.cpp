#include "hdr_output_regression.hpp"
#include "hdr_output.hpp"
#include "renderer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <glm/gtc/packing.hpp>
#include <iostream>
#include <limits>

struct RendererHdrTestAccess {
  static HdrOutput &output(Renderer &renderer) { return *renderer.hdrOutput_; }
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
             vk::AccessFlags2 dstAccess) {
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
      .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
  command.pipelineBarrier2(vk::DependencyInfo{
      .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &imageBarrier});
}
std::vector<std::byte> pixel(Device const &device, vk::Image image,
                             vk::ImageLayout layout, vk::Offset3D position,
                             std::size_t bytes) {
  auto readback =
      device.createBuffer(bytes, vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible);
  record(device, [&](vk::CommandBuffer command) {
    barrier(command, image, layout, vk::ImageLayout::eTransferSrcOptimal,
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferRead);
    command.copyImageToBuffer(
        image, vk::ImageLayout::eTransferSrcOptimal, *readback.buffer,
        {vk::BufferImageCopy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageOffset = position,
            .imageExtent = {1, 1, 1}}});
    barrier(command, image, vk::ImageLayout::eTransferSrcOptimal, layout,
            vk::PipelineStageFlagBits2::eAllCommands,
            vk::AccessFlagBits2::eMemoryRead |
                vk::AccessFlagBits2::eMemoryWrite);
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
  auto data =
      pixel(device, output.sceneImage(), output.sceneLayout(), position, 8);
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
void beginColor(vk::CommandBuffer command, vk::ImageView view,
                vk::Extent2D extent, std::array<float, 4> color) {
  vk::RenderingAttachmentInfo attachment{
      .imageView = view,
      .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
      .loadOp = vk::AttachmentLoadOp::eClear,
      .storeOp = vk::AttachmentStoreOp::eStore,
      .clearValue = {.color = vk::ClearColorValue{color}}};
  command.beginRendering(vk::RenderingInfo{.renderArea = {{0, 0}, extent},
                                           .layerCount = 1,
                                           .colorAttachmentCount = 1,
                                           .pColorAttachments = &attachment});
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
    for (auto settings :
         {DisplaySettings{0.f, true}, DisplaySettings{2.f, true},
          DisplaySettings{-2.f, true}, DisplaySettings{0.f, false},
          DisplaySettings{1.f, false}}) {
      auto destination = target(device, output.extent(), format);
      record(device, [&](vk::CommandBuffer command) {
        output.prepareScene(command);
        beginColor(command, output.sceneView(), output.extent(), colors[0]);
        for (unsigned index = 1; index < colors.size(); ++index) {
          vk::ClearAttachment attachment{
              .aspectMask = vk::ImageAspectFlagBits::eColor,
              .colorAttachment = 0,
              .clearValue = {.color = vk::ClearColorValue{colors[index]}}};
          command.clearAttachments(
              {attachment},
              {vk::ClearRect{.rect = {{int(index % 2), int(index / 2)}, {1, 1}},
                             .baseArrayLayer = 0,
                             .layerCount = 1}});
        }
        command.endRendering();
        barrier(command, *destination.storage.image,
                vk::ImageLayout::eUndefined,
                vk::ImageLayout::eColorAttachmentOptimal,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                vk::AccessFlagBits2::eColorAttachmentWrite);
        output.drawDisplay(command, *destination.view, settings);
        command.endRendering();
      });
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
      record(device, [&](vk::CommandBuffer command) {
        barrier(command, *destination.storage.image,
                vk::ImageLayout::eUndefined,
                vk::ImageLayout::eColorAttachmentOptimal,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                vk::AccessFlagBits2::eColorAttachmentWrite);
        output.drawDisplay(command, *destination.view, {ev, true});
        vk::ClearAttachment ui{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .colorAttachment = 0,
            .clearValue = {.color = vk::ClearColorValue{
                               std::array<float, 4>{.25f, .5f, .75f, 1.f}}}};
        command.clearAttachments({ui}, {vk::ClearRect{.rect = {{0, 0}, {1, 1}},
                                                      .baseArrayLayer = 0,
                                                      .layerCount = 1}});
        command.endRendering();
      });
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
void exerciseHdrScene(Device const &device, SwapChain const &swapchain) {
  {
    Renderer renderer(device);
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
          output.sceneLayout() == vk::ImageLayout::eShaderReadOnlyOptimal,
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
      require(renderer.beginFrame(glm::mat4(1), {0, 0, 2}, lighting, false) ==
                  Renderer::FrameResult::eSuccess,
              "HDR scene frame failed");
      renderer.drawObject(0, 0, glm::mat4(1));
      renderer.drawObject(0, 1,
                          glm::translate(glm::mat4(1), {0.f, 0.f, -.25f}));
      renderer.endFrame();
      device.logicalDevice().waitIdle();
      renderer.collectCompletedWork();
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
      record(device, [&](vk::CommandBuffer command) {
        barrier(command, *destination.storage.image,
                vk::ImageLayout::eUndefined,
                vk::ImageLayout::eColorAttachmentOptimal,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                vk::AccessFlagBits2::eColorAttachmentWrite);
        output.drawDisplay(command, *destination.view, settings);
        command.endRendering();
      });
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
    bool resizeRejected = false;
    {
      auto original = std::filesystem::current_path();
      auto temporary = std::filesystem::temp_directory_path() /
                       ("vulkan-hdr-output-failure-" +
                        std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch().count()));
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
          std::filesystem::copy_file(
              entry.path(), temporary / "shaders" / entry.path().filename());
      std::filesystem::current_path(temporary);
      try {
        renderer.recreateForSwapChain(swapchain);
      } catch (std::runtime_error const &) {
        resizeRejected = true;
      }
    }
    require(resizeRejected &&
                RendererHdrTestAccess::output(renderer).sceneImage() ==
                    liveImage,
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
    renderer.setUiDrawCallback({});
    require(uiCalls == 8 && renderer.gpuTimings().valid &&
                renderer.gpuTimings().outputMs > 0,
            "HDR output/UI scheduling or separate timestamp query missing");
  }
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "HDR scene shutdown leaked resources");
  std::cout << "PASS HDR scene: actual opaque/alpha-blend emissive >1, linear "
               "compositing, global exposure after blending, sky intensity, "
               "debug bypass, rejected EV/nested frame, failed replacement, "
               "UI ordering, output timestamps, zero shutdown\n";
}
