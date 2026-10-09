#include "gltf_loader.hpp"
#include "hdr_output_regression.hpp"
#include "renderer.hpp"
#include "viewer_scene_prepare.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
struct RendererRtTestAccess {
  static RayTracingGeometry const &geometry(Renderer const &r) {
    return *r.sceneAssets_->rtGeometry_;
  }
  static std::array<vk::DescriptorImageInfo, 2> textures(Renderer const &r) {
    auto const &m = r.materials().material(0);
    return {vk::DescriptorImageInfo{m.baseAlbedoTexture.sampler(),
                                    m.baseAlbedoTexture.imageView(),
                                    vk::ImageLayout::eShaderReadOnlyOptimal},
            vk::DescriptorImageInfo{m.baseAlphaTexture.sampler(),
                                    m.baseAlphaTexture.imageView(),
                                    vk::ImageLayout::eShaderReadOnlyOptimal}};
  }
  static vk::Image image(Renderer const &r) { return r.rayTracing_->image(); }
  static void debug(Renderer &r, unsigned mode) { r.rtPush_.options.x = mode; }
  static unsigned instances(Renderer const &r, unsigned slot) {
    return r.rayTracing_->instanceCount(slot);
  }
};
namespace {
void require(bool ok, char const *message) {
  if (!ok)
    throw std::runtime_error(message);
}
std::vector<glm::vec4> read(Device const &device, Renderer &renderer,
                            vk::Extent2D extent) {
  device.graphicsQueue().waitIdle();
  renderer.collectCompletedWork();
  auto output =
      device.createBuffer(std::uint64_t(extent.width) * extent.height * 16,
                          vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible);
  vk::raii::CommandPool pool(
      device.logicalDevice(),
      {.queueFamilyIndex = device.graphicsQueueFamilyIndex()});
  vk::raii::CommandBuffers commands(device.logicalDevice(),
                                    {.commandPool = *pool,
                                     .level = vk::CommandBufferLevel::ePrimary,
                                     .commandBufferCount = 1});
  auto &c = commands.front();
  c.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
  vk::ImageMemoryBarrier2 barrier{
      .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
      .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
      .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
      .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
      .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
      .newLayout = vk::ImageLayout::eTransferSrcOptimal,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = RendererRtTestAccess::image(renderer),
      .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
  c.pipelineBarrier2(
      {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
  c.copyImageToBuffer(
      barrier.image, vk::ImageLayout::eTransferSrcOptimal, *output.buffer,
      {vk::BufferImageCopy{
          .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
          .imageExtent = {extent.width, extent.height, 1}}});
  barrier.srcStageMask = vk::PipelineStageFlagBits2::eCopy;
  barrier.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
  barrier.dstStageMask = vk::PipelineStageFlagBits2::eRayTracingShaderKHR |
                         vk::PipelineStageFlagBits2::eFragmentShader;
  barrier.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite |
                          vk::AccessFlagBits2::eShaderSampledRead;
  std::swap(barrier.oldLayout, barrier.newLayout);
  c.pipelineBarrier2(
      {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
  c.end();
  auto command = *c;
  device.graphicsQueue().submit(
      {vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &command}},
      nullptr);
  device.graphicsQueue().waitIdle();
  std::vector<glm::vec4> result(extent.width * extent.height);
  output.read(std::as_writable_bytes(std::span(result)));
  return result;
}
void pendingSlots(Device const &device, Renderer const &renderer,
                  glm::mat4 const &inverse) {
  require(device.timelineSemaphoreSupported(),
          "RT pending-slot check requires timeline capability");
  constexpr unsigned width = 16;
  RayTracingRenderer rt(device, {width, width}, 2);
  auto textures = RendererRtTestAccess::textures(renderer);
  auto const &geometry = RendererRtTestAccess::geometry(renderer);
  std::array<GpuBuffer, 2> downloads;
  for (auto &b : downloads)
    b = device.createBuffer(width * width * 16,
                            vk::BufferUsageFlagBits::eTransferDst,
                            vk::MemoryPropertyFlagBits::eHostVisible);
  vk::raii::CommandPool pool(
      device.logicalDevice(),
      {.queueFamilyIndex = device.graphicsQueueFamilyIndex()});
  vk::raii::CommandBuffers commands(device.logicalDevice(),
                                    {.commandPool = *pool,
                                     .level = vk::CommandBufferLevel::ePrimary,
                                     .commandBufferCount = 2});
  vk::SemaphoreTypeCreateInfo type{
      .semaphoreType = vk::SemaphoreType::eTimeline, .initialValue = 0};
  vk::raii::Semaphore gate(device.logicalDevice(), {.pNext = &type});
  auto semaphore = *gate;
  std::uint64_t value = 1;
  auto waitStage =
      vk::PipelineStageFlags(vk::PipelineStageFlagBits::eAllCommands);
  vk::TimelineSemaphoreSubmitInfo timeline{.waitSemaphoreValueCount = 1,
                                           .pWaitSemaphoreValues = &value};
  device.graphicsQueue().submit(
      {vk::SubmitInfo{.pNext = &timeline,
                      .waitSemaphoreCount = 1,
                      .pWaitSemaphores = &semaphore,
                      .pWaitDstStageMask = &waitStage}},
      nullptr);
  bool released = false;
  try {
    for (unsigned slot = 0; slot < 2; ++slot) {
      RtInstanceInput instance;
      instance.tint =
          slot == 0 ? glm::vec4(.2f, .4f, .6f, 1) : glm::vec4(.7f, .3f, .1f, 1);
      rt.prepare(slot, geometry, std::span{&instance, 1}, textures);
      RenderGraph graph;
      auto image = rt.importTarget(graph);
      auto trace = graph.addPass("blocked primary",
                                 {{image, RenderGraph::Usage::RayTracingWrite,
                                   vk::AttachmentLoadOp::eDontCare,
                                   vk::AttachmentStoreOp::eStore, true}});
      auto copy = graph.addPass("blocked readback",
                                {{image, RenderGraph::Usage::TransferSource}});
      auto plan = graph.compile();
      auto &command = commands[slot];
      command.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
      rt.build(*command, slot);
      plan.record(*command, [&](auto const &pass, auto event) {
        if (event != RenderGraph::Event::Draw)
          return;
        if (pass.id == trace)
          rt.trace(*command, slot, RtPush{.inverseViewProjection = inverse});
        if (pass.id == copy)
          command.copyImageToBuffer(
              rt.image(), vk::ImageLayout::eTransferSrcOptimal,
              *downloads[slot].buffer,
              {vk::BufferImageCopy{
                  .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0,
                                       1},
                  .imageExtent = {width, width, 1}}});
      });
      command.end();
      auto raw = *command;
      device.graphicsQueue().submit(
          {vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &raw}},
          nullptr);
      rt.submitted(plan, image);
    }
    device.logicalDevice().signalSemaphore(
        {.semaphore = semaphore, .value = 1});
    released = true;
    device.graphicsQueue().waitIdle();
  } catch (...) {
    if (!released)
      device.logicalDevice().signalSemaphore(
          {.semaphore = semaphore, .value = 1});
    device.graphicsQueue().waitIdle();
    throw;
  }
  for (unsigned slot = 0; slot < 2; ++slot) {
    std::array<glm::vec4, width * width> image;
    downloads[slot].read(std::as_writable_bytes(std::span(image)));
    auto expected =
        slot == 0 ? glm::vec3(.2f, .4f, .6f) : glm::vec3(.7f, .3f, .1f);
    require(glm::length(glm::vec3(image[width / 2 * width + width / 2]) -
                        expected) < 1e-5f,
            "Pending RT slot table/descriptor was overwritten");
  }
}
} // namespace
void exerciseRayTracing(Device const &device, SwapChain const &swapchain,
                        unsigned slots) {
  require(device.rayTracingSupported(),
          "SKIP RT: required device features unavailable");
  {
    Renderer renderer(device, slots);
    renderer.recreateForSwapChain(swapchain);
    require(renderer.rayTracingAvailable(),
            renderer.rayTracingUnavailableReason().c_str());
    AssetLibrary assets;
    Mesh m;
    for (auto position :
         {glm::vec3{-3, -3, 0}, glm::vec3{3, -3, 0}, glm::vec3{0, 3, 0}})
      m.vertices.push_back(Vertex{.position = position,
                                  .color = {1, 1, 1},
                                  .normal = {0, 0, 1},
                                  .uv = {.5f, .5f}});
    m.indices = {0, 1, 2};
    assets.meshes.push_back(m);
    Material red;
    red.tint = {.25f, .5f, .75f, 1};
    red.doubleSided = false;
    Material mask = red;
    mask.alphaMode = AlphaMode::Mask;
    mask.tint.a = .1f;
    assets.materials = {red, mask};
    auto candidate = renderer.prepareScene(assets);
    renderer.waitSceneUpload(candidate);
    require(renderer.commitScene(candidate), "RT scene commit failed");
    renderer.setRenderMethod(Renderer::RenderMethod::RayTracing);
    glm::mat4 view =
        glm::lookAt(glm::vec3(0, 0, 2), glm::vec3(0), glm::vec3(0, 1, 0));
    auto proj = glm::perspective(
        glm::radians(60.f),
        float(swapchain.extent().width) / swapchain.extent().height, .1f, 10.f);
    proj[1][1] *= -1;
    std::vector<Renderer::DrawItem> items{
        {.objectIndex = 1, .meshId = 0, .materialId = 0}};
    LightingSettings lighting;
    lighting.sunEnabled = false;
    lighting.clusteredLights = false;
    lighting.toneMappingEnabled = false;
    auto render = [&](unsigned mode = 0) {
      RendererRtTestAccess::debug(renderer, mode);
      Renderer::SceneDrawList scene{
          .opaque = items, .allOpaque = items, .sky = false};
      require(renderer.renderFrame(scene, proj * view, {0, 0, 2}, lighting,
                                   false) == Renderer::FrameResult::eSuccess,
              "RT render failed");
      require(!renderer.aoActive() && !renderer.taaActive(),
              "RT accidentally executed raster post effects");
      auto pixels = read(device, renderer, swapchain.extent());
      for (auto v : pixels)
        for (unsigned i = 0; i < 4; ++i)
          require(std::isfinite(v[i]), "RT image contains nonfinite values");
      return pixels[swapchain.extent().height / 2 * swapchain.extent().width +
                    swapchain.extent().width / 2];
    };
    auto color = render();
    require(glm::length(glm::vec3(color) - glm::vec3(.25f, .5f, .75f)) < 1e-5f,
            "Primary hit material differs from expected color");
    auto normal = render(1);
    require(glm::length(glm::vec3(normal) - glm::vec3(.5f, .5f, 1.f)) < 1e-5f,
            "Primary normal or 112B Vertex layout incorrect");
    auto distance = render(2);
    auto uv = glm::vec2(
        (swapchain.extent().width / 2 + .5f) / swapchain.extent().width,
        (swapchain.extent().height / 2 + .5f) / swapchain.extent().height);
    auto inv = glm::inverse(proj * view);
    auto near = inv * glm::vec4(uv * 2.f - 1.f, 0, 1),
         far = inv * glm::vec4(uv * 2.f - 1.f, 1, 1);
    auto origin = glm::vec3(near) / near.w,
         direction = glm::normalize(glm::vec3(far) / far.w - origin);
    require(std::abs(distance.r - (-origin.z / direction.z)) < 1e-4f,
            "RT distance differs from analytic ray/plane reference");
    // Complete scene must drive primary rays even when the raster visibility
    // list is empty.
    Renderer::SceneDrawList complete{.allOpaque = items, .sky = false};
    RendererRtTestAccess::debug(renderer, 0);
    require(renderer.renderFrame(complete, proj * view, {0, 0, 2}, lighting,
                                 false) == Renderer::FrameResult::eSuccess,
            "Complete RT scene failed");
    color = read(device, renderer,
                 swapchain.extent())[swapchain.extent().height / 2 *
                                         swapchain.extent().width +
                                     swapchain.extent().width / 2];
    require(std::abs(color.r - .25f) < 1e-5f,
            "RT used camera-culled raster geometry");
    items[0].materialId = 1;
    color = render();
    require(std::abs(color.r - .05f) < 1e-5f,
            "MASK alpha failed any-hit rejection");
    items[0].materialId = 0;
    items[0].primaryVisible = false;
    color = render();
    require(std::abs(color.r - .05f) < 1e-5f,
            "Hidden light card participated in primary RT");
    items[0].primaryVisible = true;
    items[0].modelMatrix = glm::translate(glm::mat4(1), {20, 0, 0});
    color = render();
    require(std::abs(color.r - .05f) < 1e-5f,
            "TLAS transform update retained old instance");
    items[0].modelMatrix = glm::scale(glm::mat4(1), {-1.f, 1.f, 1.f});
    color = render();
    require(std::abs(color.r - .25f) < 1e-5f,
            "Mirrored single-sided instance facing incorrect");
    auto debug = renderer.rasterizerDebugSettings();
    debug.cullMode = vk::CullModeFlagBits::eFront;
    renderer.setRasterizerDebugSettings(debug);
    color = render();
    require(std::abs(color.r - .05f) < 1e-5f,
            "RT front culling ignored raster setting");
    debug.cullMode = vk::CullModeFlagBits::eBack;
    renderer.setRasterizerDebugSettings(debug);
    items[0].modelMatrix = glm::mat4(1);
    // Two pending submissions exercise immutable slot descriptors/tables, with
    // different transforms.
    for (unsigned i = 0; i < slots; ++i) {
      items[0].modelMatrix =
          glm::translate(glm::mat4(1), {float(i) * .1f, 0, 0});
      Renderer::SceneDrawList scene{
          .opaque = items, .allOpaque = items, .sky = false};
      require(renderer.renderFrame(scene, proj * view, {0, 0, 2}, lighting,
                                   false) == Renderer::FrameResult::eSuccess,
              "Pending RT slot failed");
    }
    device.graphicsQueue().waitIdle();
    renderer.collectCompletedWork();
    require(renderer.gpuTimings().valid && renderer.gpuTimings().rayTracing,
            "RT submission query/tag mismatch");
    auto hashBefore = renderer.submittedFrameId();
    renderer.setRenderMethod(Renderer::RenderMethod::Raster);
    Renderer::SceneDrawList scene{
        .opaque = items, .allOpaque = items, .sky = false};
    require(renderer.renderFrame(scene, proj * view, {0, 0, 2}, lighting,
                                 false) == Renderer::FrameResult::eSuccess,
            "RT-to-raster switch failed");
    device.graphicsQueue().waitIdle();
    renderer.collectCompletedWork();
    require(!renderer.gpuTimings().rayTracing &&
                renderer.submittedFrameId() == hashBefore + 1,
            "Mode switch submission identity incorrect");
    renderer.setRenderMethod(Renderer::RenderMethod::RayTracing);
    color = render();
    require(std::abs(color.r - .25f) < 1e-5f, "Raster-to-RT switch failed");
    if (slots == 2)
      pendingSlots(device, renderer, glm::inverse(proj * view));
    // Candidate failure leaves the committed RT scene usable.
    auto invalid = assets;
    invalid.meshes[0].indices[0] = 999;
    bool rejected = false;
    try {
      renderer.prepareScene(invalid);
    } catch (std::runtime_error const &) {
      rejected = true;
    }
    require(rejected, "Invalid RT candidate was accepted");
    color = render();
    require(std::abs(color.r - .25f) < 1e-5f,
            "Failed candidate lost live RT scene");
    candidate = renderer.prepareScene(assets);
    renderer.waitSceneUpload(candidate);
    require(renderer.commitScene(candidate), "Second RT commit failed");
    color = render();
    require(std::abs(color.r - .25f) < 1e-5f,
            "RT scene replacement changed visibility");
    device.graphicsQueue().waitIdle();
    renderer.collectCompletedWork();
    renderer.recreateForSwapChain(swapchain);
    color = render();
    require(std::abs(color.r - .25f) < 1e-5f, "RT resource recreation failed");
    device.graphicsQueue().waitIdle();
    renderer.collectCompletedWork();
    require(renderer.renderGraphDump().find("RT path tracing") !=
                std::string::npos,
            "RT pass absent from graph dump");
    auto manyMaterials = assets;
    manyMaterials.materials.resize(RayTracingRenderer::textureCapacity / 2 + 1);
    candidate = renderer.prepareScene(manyMaterials);
    renderer.waitSceneUpload(candidate);
    require(renderer.commitScene(candidate),
            "RT material limit incorrectly rejected raster scene");
    require(renderer.renderMethod() == Renderer::RenderMethod::Raster &&
                !renderer.rayTracingAvailable(),
            "Oversized RT table did not safely select raster");
    bool modeRejected = false;
    try {
      renderer.setRenderMethod(Renderer::RenderMethod::RayTracing);
    } catch (std::runtime_error const &) {
      modeRejected = true;
    }
    require(modeRejected && renderer.rayTracingUnavailableReason().find(
                                "256 materials") != std::string::npos,
            "RT table limit missing mode diagnostic");
    require(renderer.renderFrame(scene, proj * view, {0, 0, 2}, lighting,
                                 false) == Renderer::FrameResult::eSuccess,
            "Oversized RT table broke raster rendering");
    device.graphicsQueue().waitIdle();
    renderer.collectCompletedWork();
    candidate = renderer.prepareScene(assets);
    renderer.waitSceneUpload(candidate);
    require(renderer.commitScene(candidate) && renderer.rayTracingAvailable(),
            "Smaller scene failed to restore RT eligibility");
    renderer.setRenderMethod(Renderer::RenderMethod::RayTracing);
    color = render();
    require(std::abs(color.r - .25f) < 1e-5f,
            "RT did not recover after compatible reload");
  }
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "RT resources leaked after renderer destruction");
  std::cout << "PASS RT-A: analytic primary color/normal/distance, "
               "mask/visibility/transform/mirror/cull, "
               "slots/switch/commit/failure/recreate/retirement\n";
}

void exerciseRayTracingKitchen(Device const &device, SwapChain const &swapchain,
                               unsigned slots,
                               std::filesystem::path const &asset,
                               std::filesystem::path const &directory) {
  require(device.rayTracingSupported(), "SKIP RT: device features unavailable");
  auto imported = loadStaticGltfScene(asset, "");
  auto repairs = prepareImportedSceneForViewer(imported);
  {
    AssetLibrary assets{imported.meshes, imported.materials};
    Renderer renderer(device, slots);
    renderer.recreateForSwapChain(swapchain);
    auto candidate = renderer.prepareScene(assets);
    renderer.waitSceneUpload(candidate);
    require(renderer.commitScene(candidate), "Kitchen RT commit failed");
    renderer.setRenderMethod(Renderer::RenderMethod::RayTracing);
    std::vector<Renderer::DrawItem> opaque, mask, blend;
    for (std::size_t i = 0; i < imported.objects.size(); ++i) {
      auto const &o = imported.objects[i];
      Renderer::DrawItem item{.objectIndex = i,
                              .meshId = o.meshId,
                              .materialId = o.materialId,
                              .modelMatrix = o.transform.matrix(),
                              .worldBounds = o.worldBounds,
                              .primaryVisible = o.primaryVisible,
                              .shadowCaster = o.shadowCaster};
      auto alpha = imported.materials[o.materialId].alphaMode;
      (alpha == AlphaMode::Opaque ? opaque
       : alpha == AlphaMode::Mask ? mask
                                  : blend)
          .push_back(item);
    }
    Camera camera;
    camera.position = {1.5f, 1.7f, 2.3f};
    camera.target = {-1.5f, 1.3f, -2.f};
    camera.farPlane = 30.f;
    LightingSettings lighting;
    lighting.sunEnabled = false;
    lighting.toneMappingEnabled = false;
    Renderer::SceneDrawList scene{.opaque = opaque,
                                  .mask = mask,
                                  .transparent = blend,
                                  .allOpaque = opaque,
                                  .allMask = mask,
                                  .allTransparent = blend};
    for (unsigned i = 0; i < slots + 2; ++i)
      require(
          renderer.renderFrame(scene,
                               camera.viewProj(float(swapchain.extent().width) /
                                               swapchain.extent().height),
                               camera.position, lighting,
                               false) == Renderer::FrameResult::eSuccess,
          "Kitchen RT frame failed");
    auto image = read(device, renderer, swapchain.extent());
    for (auto v : image)
      for (unsigned i = 0; i < 4; ++i)
        require(std::isfinite(v[i]) && v[i] >= 0,
                "Kitchen RT pixels nonfinite/negative");
    unsigned colored = 0;
    for (auto v : image)
      if (glm::length(glm::vec3(v) - glm::vec3(.05, .07, .1)) > .01)
        ++colored;
    require(colored > image.size() / 4,
            "Kitchen RT did not shade substantial scene geometry");
    std::filesystem::create_directories(directory);
    std::ofstream raw(directory / "kitchen-primary.rgba32f", std::ios::binary);
    raw.write(reinterpret_cast<char const *>(image.data()),
              image.size() * sizeof(glm::vec4));
    std::ofstream report(directory / "kitchen-primary.json");
    report
        << "{\"width\":" << swapchain.extent().width
        << ",\"height\":" << swapchain.extent().height
        << ",\"objects\":" << imported.objects.size()
        << ",\"hidden_light_cards\":" << repairs.lightCards
        << ",\"colored_pixels\":" << colored
        << ",\"stage\":\"RT-B path tracing\",\"scene_pass_ms\":"
        << renderer.gpuTimings().mainMs << "}\n";
    require(renderer.gpuTimings().valid && renderer.gpuTimings().rayTracing,
            "Kitchen RT query mode mismatch");
  }
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "Kitchen RT resources leaked");
  std::cout << "PASS kitchen RT primary visibility, complete geometry, "
               "material textures, hidden cards, HDR finite, retirement\n";
}

void exerciseRayTracingDisabled(Device const &device,
                                SwapChain const &swapchain, unsigned slots) {
  require(!device.rayTracingSupported(),
          "Disabled RT profile still enabled features");
  {
    Renderer renderer(device, slots);
    renderer.recreateForSwapChain(swapchain);
    require(!renderer.rayTracingAvailable() &&
                !renderer.rayTracingUnavailableReason().empty(),
            "Disabled RT profile has no diagnostic");
    bool rejected = false;
    try {
      renderer.setRenderMethod(Renderer::RenderMethod::RayTracing);
    } catch (std::runtime_error const &) {
      rejected = true;
    }
    require(rejected &&
                renderer.renderMethod() == Renderer::RenderMethod::Raster,
            "Unavailable RT mode changed selected renderer");
    AssetLibrary assets;
    Mesh mesh;
    for (auto p :
         {glm::vec3{-1, -1, 0}, glm::vec3{1, -1, 0}, glm::vec3{0, 1, 0}})
      mesh.vertices.push_back(
          Vertex{.position = p, .color = {1, 1, 1}, .normal = {0, 0, 1}});
    mesh.indices = {0, 1, 2};
    assets.meshes.push_back(mesh);
    assets.materials.push_back(Material{});
    auto candidate = renderer.prepareScene(assets);
    renderer.waitSceneUpload(candidate);
    require(renderer.commitScene(candidate),
            "Raster-only profile scene failed");
    Renderer::DrawItem item{.objectIndex = 1};
    Renderer::SceneDrawList scene{.opaque = std::span{&item, 1},
                                  .allOpaque = std::span{&item, 1}};
    Camera camera;
    camera.position = {0, 0, 2};
    camera.target = {0, 0, 0};
    for (unsigned i = 0; i < slots + 1; ++i)
      require(
          renderer.renderFrame(scene,
                               camera.viewProj(float(swapchain.extent().width) /
                                               swapchain.extent().height),
                               camera.position, {},
                               false) == Renderer::FrameResult::eSuccess,
          "Disabled RT profile broke raster rendering");
    device.graphicsQueue().waitIdle();
    renderer.collectCompletedWork();
    require(renderer.gpuTimings().valid && !renderer.gpuTimings().rayTracing,
            "Raster-only profile query mismatch");
  }
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "Raster-only profile leaked resources");
  std::cout << "PASS disabled RT device profile: no RT resources, clear "
               "rejection, raster still renders, zero teardown\n";
}
