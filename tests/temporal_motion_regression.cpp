#include "hdr_output_regression.hpp"
#include "renderer.hpp"
#include <glm/gtc/packing.hpp>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, char const *why) {
  if (!ok)
    throw std::runtime_error(why);
}
} // namespace
struct RendererMotionTestAccess {
  static glm::vec4 pixel(Renderer &r, glm::uvec2 xy, bool color = false) {
    auto const &d = r.device_;
    auto bytes = d.createBuffer(8, vk::BufferUsageFlagBits::eTransferDst,
                                vk::MemoryPropertyFlagBits::eHostVisible);
    vk::raii::CommandPool pool(
        d.logicalDevice(),
        vk::CommandPoolCreateInfo{
            .flags = vk::CommandPoolCreateFlagBits::eTransient,
            .queueFamilyIndex = d.graphicsQueueFamilyIndex()});
    vk::raii::CommandBuffers commands(
        d.logicalDevice(),
        vk::CommandBufferAllocateInfo{.commandPool = *pool,
                                      .level = vk::CommandBufferLevel::ePrimary,
                                      .commandBufferCount = 1});
    auto &cmd = commands[0];
    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    auto source =
        color ? r.hdrOutput_->sceneImage() : *r.motionResources_.storage.image;
    auto view =
        color ? r.hdrOutput_->sceneView() : *r.motionResources_.imageView;
    auto &state = color ? r.imageStates_.hdr : r.imageStates_.motion;
    using G = RenderGraph;
    G graph;
    auto image = graph.importImage(
        {"Motion readback", source, view, Renderer::kMotionFormat,
         r.swapChain_->extent(), vk::ImageAspectFlagBits::eColor,
         vk::ImageUsageFlagBits::eColorAttachment |
             vk::ImageUsageFlagBits::eSampled |
             vk::ImageUsageFlagBits::eTransferSrc,
         false, state});
    graph.addPass("Copy motion", {{image, G::Usage::TransferSource}});
    graph.exportImage(image, G::Usage::SampledColor);
    auto plan = graph.compile();
    plan.record(*cmd, [&](auto const &, G::Event e) {
      if (e == G::Event::Draw) {
        cmd.copyImageToBuffer(
            source, vk::ImageLayout::eTransferSrcOptimal, *bytes.buffer,
            {vk::BufferImageCopy{
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageOffset = {int(xy.x), int(xy.y), 0},
                .imageExtent = {1, 1, 1}}});
        vk::BufferMemoryBarrier2 host{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *bytes.buffer,
            .size = 8};
        cmd.pipelineBarrier2(
            {.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host});
      }
    });
    cmd.end();
    vk::raii::Fence fence(d.logicalDevice(), vk::FenceCreateInfo{});
    vk::CommandBuffer raw = *cmd;
    d.graphicsQueue().submit(
        {vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &raw}},
        *fence);
    check(d.logicalDevice().waitForFences({*fence}, true, UINT64_MAX) ==
              vk::Result::eSuccess,
          "Motion readback failed");
    state = plan.finalState(image);
    std::array<std::uint16_t, 4> h{};
    bytes.read(std::as_writable_bytes(std::span{h}));
    return {glm::unpackHalf1x16(h[0]), glm::unpackHalf1x16(h[1]),
            glm::unpackHalf1x16(h[2]), glm::unpackHalf1x16(h[3])};
  }
  static bool taaReady(Renderer const &r) { return r.taa_->ready(); }
  static std::uint64_t sample(Renderer const &r) {
    return r.temporalHistory_.nextSample();
  }
};
void exerciseTemporalMotion(Device const &device, SwapChain const &swapchain,
                            unsigned slots) {
  {
    Renderer r(device, slots);
    r.recreateForSwapChain(swapchain);
    AssetLibrary assets;
    Mesh quad;
    for (auto p : {glm::vec3{-.3f, -.3f, .5f}, glm::vec3{.3f, -.3f, .5f},
                   glm::vec3{.3f, .3f, .5f}, glm::vec3{-.3f, .3f, .5f}})
      quad.vertices.push_back(
          Vertex{.position = p, .color = {1, 1, 1}, .normal = {0, 0, 1}});
    quad.indices = {0, 1, 2, 2, 3, 0};
    assets.meshes.push_back(quad);
    Material opaque;
    opaque.doubleSided = true;
    opaque.emissiveFactor = {.5f, .25f, .125f};
    auto mask = opaque;
    mask.alphaMode = AlphaMode::Mask;
    mask.tint.a = 0;
    auto blend = opaque;
    blend.alphaMode = AlphaMode::Blend;
    blend.tint.a = .5f;
    assets.materials = {opaque, mask, blend};
    auto candidate = r.prepareScene(assets);
    r.waitSceneUpload(candidate);
    check(r.commitScene(candidate), "Motion asset commit failed");
    auto size = swapchain.extent();
    glm::uvec2 left{size.width / 4, size.height / 2},
        right{size.width * 3 / 4, size.height / 2};
    std::array items{Renderer::DrawItem{.objectIndex = 7,
                                        .meshId = 0,
                                        .materialId = 0,
                                        .modelMatrix = glm::translate(
                                            glm::mat4(1), {-.5f, 0, 0})},
                     Renderer::DrawItem{.objectIndex = 9,
                                        .meshId = 0,
                                        .materialId = 0,
                                        .modelMatrix = glm::translate(
                                            glm::mat4(1), {.5f, 0, 0})}};
    LightingSettings light;
    light.sunEnabled = false;
    light.environmentIntensity = 0;
    light.toneMappingEnabled = false;
    auto draw = [&](Renderer::SceneDrawList scene, glm::mat4 vp = glm::mat4(1),
                    glm::vec3 cam = glm::vec3{0, 0, 2}, bool shadows = false) {
      check(r.renderFrame(scene, vp, cam, light, shadows) !=
                Renderer::FrameResult::eSwapChainOutOfDate,
            "Motion frame out of date");
      device.logicalDevice().waitIdle();
      r.collectCompletedWork();
    };
    auto scene = [&] {
      return Renderer::SceneDrawList{
          .opaque = items, .allOpaque = items, .sky = false};
    };
    auto read = [&](glm::uvec2 p) {
      return RendererMotionTestAccess::pixel(r, p);
    };
    auto close = [&](glm::vec4 v, glm::vec4 expected) {
      if (glm::length(v - expected) > .001f)
        std::cerr << "Motion actual " << v.x << ',' << v.y << ',' << v.z << ','
                  << v.w << " expected " << expected.x << ',' << expected.y
                  << ',' << expected.z << ',' << expected.w << '\n';
      check(glm::length(v - expected) < .001f,
            "Motion pixel differs from analytic reference");
    };
    draw(scene());
    close(read(left), {0, 0, -1, 0});
    draw(scene());
    close(read(left), {0, 0, 1, 1});
    close(read(right), {0, 0, 1, 1});
    items[0].modelMatrix[3].x += .1f;
    draw(scene());
    close(read(left), {.05f, 0, 1, 1});
    close(read(right), {0, 0, 1, 1});
    auto vp = glm::mat4(1);
    vp[3].y = .1f;
    draw(scene(), vp);
    close(read(left), {0, .05f, 1, 1});
    auto previousModel = items[0].modelMatrix;
    items[0].modelMatrix = glm::translate(glm::mat4(1), {-.4f, 0, 0}) *
                           glm::scale(glm::mat4(1), {-.8f, 1.2f, 1});
    draw(scene(), vp);
    glm::vec4 ndc{(float(left.x) + .5f) / size.width * 2 - 1,
                  (float(left.y) + .5f) / size.height * 2 - 1, .5f, 1};
    auto local = glm::inverse(items[0].modelMatrix) * glm::inverse(vp) * ndc;
    auto prev = vp * previousModel * local;
    close(read(left), {(ndc.x - prev.x / prev.w) * .5f,
                       (ndc.y - prev.y / prev.w) * .5f, 1, prev.w});
    // Preserve offscreen transforms in the complete set, but reject the prior
    // projection when the object enters the image.
    auto restoreModel = items[0].modelMatrix;
    items[0].modelMatrix[3].x = 3;
    draw({.opaque = std::span{items}.subspan(1),
          .allOpaque = items,
          .sky = false},
         vp);
    items[0].modelMatrix = restoreModel;
    draw(scene(), vp);
    close(read(left), {0, 0, -1, 0});
    draw(scene(), vp);
    close(read(left), {0, 0, 1, 1});
    r.invalidateTemporalHistory();
    draw(scene(), vp);
    close(read(left), {0, 0, -1, 0});
    draw(scene(), vp);
    auto id = r.submittedFrameId();
    auto sample = RendererMotionTestAccess::sample(r);
    auto duplicate = items;
    duplicate[1].objectIndex = 7;
    bool rejected = false;
    try {
      draw({.opaque = duplicate, .allOpaque = duplicate, .sky = false}, vp);
    } catch (std::runtime_error const &) {
      rejected = true;
    }
    check(rejected && r.submittedFrameId() == id &&
              RendererMotionTestAccess::sample(r) == sample,
          "Invalid identity acquired/advanced frame");
    auto background = items;
    auto covered = Renderer::DrawItem{
        .objectIndex = 20,
        .meshId = 0,
        .materialId = 1,
        .modelMatrix = glm::translate(glm::mat4(1), {-.5f, 0, -.1f})};
    std::array covers{covered};
    draw({.opaque = background,
          .mask = covers,
          .allOpaque = background,
          .allMask = covers,
          .sky = false},
         vp);
    close(read(left), {0, 0, 1, 1});
    r.setMaterialTint(1, {1, 1, 1, 1});
    draw({.opaque = background,
          .mask = covers,
          .allOpaque = background,
          .allMask = covers,
          .sky = false},
         vp);
    close(read(left), {0, 0, 1, 1});
    covers[0].materialId = 2;
    draw({.opaque = background,
          .transparent = covers,
          .allOpaque = background,
          .allTransparent = covers,
          .sky = false},
         vp);
    check(std::abs(read(left).z - .5f) < .001f,
          "BLEND coverage failed to reduce temporal validity");
    r.setTemporalJitterEnabled(true);
    draw(scene(), vp);
    close(read(left), {0, 0, -1, 0});
    draw(scene(), vp);
    close(read(left), {0, 0, 1, 1});
    check(glm::vec2(r.temporalCamera().jitterUv) !=
              glm::vec2(r.temporalCamera().jitterUv.z,
                        r.temporalCamera().jitterUv.w),
          "Jitter did not advance");
    r.setTemporalJitterEnabled(false);
    Camera camera;
    camera.position = {0, 0, 3};
    camera.target = {0, 0, 0};
    camera.farPlane = 100;
    light.sunEnabled = true;
    draw({.sky = true}, camera.viewProj(float(size.width) / size.height),
         camera.position, true);
    auto csm = r.sunCascades();
    r.setTemporalJitterEnabled(true);
    draw({.sky = true}, camera.viewProj(float(size.width) / size.height),
         camera.position, true);
    for (unsigned i = 0; i < 4; ++i)
      for (unsigned c = 0; c < 4; ++c)
        check(csm.viewProj[i][c] == r.sunCascades().viewProj[i][c],
              "Jitter moved CSM fit");
    light.sunEnabled = false;
    PunctualLight point;
    point.position = {0, 0, 1.5f};
    point.range = 4;
    point.intensity = 3.14159265f;
    light.punctualLights = {point};
    auto referenceClip = camera.viewProj(float(size.width) / size.height) *
                         items[0].modelMatrix * glm::vec4(0, 0, .5f, 1);
    auto referenceUv = glm::vec2(referenceClip) / referenceClip.w * .5f + .5f;
    glm::uvec2 shaded =
        glm::uvec2(referenceUv * glm::vec2(size.width, size.height));
    light.clusteredLights = false;
    r.setTemporalJitterEnabled(false);
    r.setTemporalJitterEnabled(true);
    draw(scene(), camera.viewProj(float(size.width) / size.height),
         camera.position);
    auto full = RendererMotionTestAccess::pixel(r, shaded, true);
    light.clusteredLights = true;
    r.setTemporalJitterEnabled(false);
    r.setTemporalJitterEnabled(true);
    draw(scene(), camera.viewProj(float(size.width) / size.height),
         camera.position);
    auto clustered = RendererMotionTestAccess::pixel(r, shaded, true);
    std::cout << "Jitter light HDR full/cluster: " << full.x << ',' << full.y
              << ',' << full.z << " / " << clustered.x << ',' << clustered.y
              << ',' << clustered.z << " at " << shaded.x << ',' << shaded.y
              << std::endl;
    check(full.x > .51f && glm::length(full - clustered) < .001f,
          "Jitter full/clustered HDR differs or light inactive");
    if (r.clusterSupported()) {
      check(r.clusterGrid().screen.z == 1,
            "Jitter unexpectedly disabled clusters");
      auto inverse = glm::inverse(r.temporalCamera().rasterViewProj);
      for (unsigned c = 0; c < 4; ++c)
        check(glm::length(r.clusterGrid().inverseViewProj[c] - inverse[c]) <
                  1e-6f,
              "Cluster rays disagree with jittered raster");
    }
    light.punctualLights.clear();
    r.setTemporalJitterEnabled(false);
    light.sunEnabled = false;
    auto sky = [&] {
      draw({.sky = true}, camera.viewProj(float(size.width) / size.height),
           camera.position);
    };
    sky();
    sky();
    glm::uvec2 center{size.width / 2, size.height / 2};
    close(read(center), {0, 0, 1, 0});
    camera.position.x += .5f;
    camera.target.x += .5f;
    sky();
    close(read(center), {0, 0, 1, 0});
    camera.target.x += .3f;
    sky();
    auto rotation = read(center);
    check(std::abs(rotation.x) > .01f && rotation.z == 1 && rotation.w == 0,
          "Sky rotation velocity missing");
    auto before = r.submittedFrameId();
    auto next = RendererMotionTestAccess::sample(r);
    light.localProbe.minimum = {-1, -1, -1};
    light.localProbe.maximum = {1, 1, 1};
    light.localProbe.position = {0, 0, 0};
    r.captureLocalProbe({.sky = true}, light);
    check(r.submittedFrameId() == before &&
              RendererMotionTestAccess::sample(r) == next,
          "Offline capture advanced temporal history");
    sky();
    close(read(center), {0, 0, 1, 0});
    candidate = r.prepareScene(assets);
    r.waitSceneUpload(candidate);
    check(r.commitScene(candidate), "Motion replacement commit failed");
    sky();
    close(read(center), {0, 0, -1, 0});
    r.recreateForSwapChain(swapchain);
    sky();
    close(read(center), {0, 0, -1, 0});
    r.setTaaEnabled(true);
    sky();
    check(RendererMotionTestAccess::taaReady(r) && r.gpuTimings().taaMs > 0,
          "Actual viewer-style TAA path did not publish history/query");
    r.setMaterialTint(0, {1, 1, 1, 1});
    check(RendererMotionTestAccess::taaReady(r),
          "Unchanged material reset history");
    r.setMaterialTint(0, {.8f, 1, 1, 1});
    check(!RendererMotionTestAccess::taaReady(r),
          "Changed material retained stale color history");
    sky();
    light.exposureEv = 2;
    check(r.beginFrame(camera.viewProj(float(size.width) / size.height),
                       camera.position, light,
                       false) == Renderer::FrameResult::eSuccess,
          "Exposure test begin failed");
    check(RendererMotionTestAccess::taaReady(r),
          "Exposure edit invalidated linear HDR history");
    r.drawEnvironment();
    r.endFrame();
    device.logicalDevice().waitIdle();
    r.collectCompletedWork();
    r.setTaaHistoryFilter(TaaHistoryFilter::Bilinear);
    check(!RendererMotionTestAccess::taaReady(r),
          "Changed history filter did not reset color");
    sky();
    r.setTaaHistoryFilter(TaaHistoryFilter::Bilinear);
    check(RendererMotionTestAccess::taaReady(r),
          "Unchanged history filter reset color");
    r.setSurfaceDebugEnabled(false, false);
    check(!RendererMotionTestAccess::taaReady(r),
          "Surface shading toggle retained color history");
    check(r.renderGraphDump().find("Previous instance transforms") !=
                  std::string::npos &&
              r.renderGraphDump().find("Scene motion") != std::string::npos,
          "Motion graph resources missing");
    std::cout
        << "PASS motion GPU: first/static, independent instances, +.05 UV X/Y, "
           "mirrored nonuniform transform, MASK coverage, BLEND validity=.5, "
           "jitter cancellation/CSM invariant, sky translation/rotation, "
           "invalid-before-acquire retry, probe/scene/resize resets; slots="
        << slots << '\n';
  }
  check(device.resourceLedger().snapshot().current ==
            ResourceLedger::Footprint{},
        "Motion test leaked GPU resources");
}
