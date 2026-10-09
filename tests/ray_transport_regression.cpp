#include "hdr_output_regression.hpp"
#include "renderer.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
struct RendererTransportTestAccess {
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
};
namespace {
void require(bool value, char const *why) {
  if (!value)
    throw std::runtime_error(why);
}
Mesh quad(glm::vec3 center, glm::vec3 normal, float half) {
  Mesh m;
  auto t = glm::normalize(glm::cross(
      std::abs(normal.y) < .99 ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0),
      normal));
  auto b = glm::cross(normal, t);
  for (auto uv :
       {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1)})
    m.vertices.push_back(
        Vertex{.position = center + half * (t * uv.x + b * uv.y),
               .color = {1, 1, 1},
               .normal = normal,
               .uv = {.5, .5}});
  m.indices = {0, 1, 2, 0, 2, 3};
  return m;
}
struct Harness {
  Device const &d;
  RayTracingGeometry const &geometry;
  std::array<vk::DescriptorImageInfo, 2> textures;
  RayTracingRenderer rt;
  GpuBuffer download;
  vk::raii::CommandPool pool = nullptr;
  vk::raii::CommandBuffers commands = nullptr;
  RtPush push;
  unsigned sequence = 0;
  static constexpr unsigned extent = 32;
  Harness(Device const &device, Renderer const &scene, unsigned slots)
      : d(device), geometry(RendererTransportTestAccess::geometry(scene)),
        textures(RendererTransportTestAccess::textures(scene)),
        rt(device, {extent, extent}, slots) {
    download = d.createBuffer(extent * extent * 16,
                              vk::BufferUsageFlagBits::eTransferDst,
                              vk::MemoryPropertyFlagBits::eHostVisible);
    pool = vk::raii::CommandPool(
        d.logicalDevice(), {.queueFamilyIndex = d.graphicsQueueFamilyIndex()});
    commands = vk::raii::CommandBuffers(
        d.logicalDevice(), {.commandPool = *pool,
                            .level = vk::CommandBufferLevel::ePrimary,
                            .commandBufferCount = 1});
    auto view =
        glm::lookAt(glm::vec3(0, 0, 2), glm::vec3(0), glm::vec3(0, 1, 0));
    auto p = glm::ortho(-1.f, 1.f, -1.f, 1.f, .1f, 20.f);
    p[1][1] *= -1;
    push.inverseViewProjection = glm::inverse(p * view);
    push.options = {3, 2, 0, 0};
  }
  std::array<glm::vec4, extent * extent>
  run(std::span<RtInstanceInput const> inputs, RtLightingFrame frame,
      unsigned slots) {
    auto slot = sequence++ % slots;
    frame.transport = true;
    auto bytes = std::as_bytes(std::span{&push, 1});
    frame.historyKey.assign(bytes.begin(), bytes.end());
    frame.settings.sampling.w =
        1; // Exact pixel centers for independent numeric references.
    rt.prepare(slot, geometry, inputs, textures, frame);
    RenderGraph g;
    auto image = rt.importTarget(g);
    auto trace =
        g.addPass("transport", {{image,
                                 rt.accumulatedSamples()
                                     ? RenderGraph::Usage::RayTracingReadWrite
                                     : RenderGraph::Usage::RayTracingWrite,
                                 vk::AttachmentLoadOp::eDontCare,
                                 vk::AttachmentStoreOp::eStore, true}});
    auto copy =
        g.addPass("read", {{image, RenderGraph::Usage::TransferSource}});
    auto plan = g.compile();
    auto &c = commands.front();
    c.reset();
    c.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    rt.build(*c, slot);
    plan.record(*c, [&](auto const &pass, auto event) {
      if (event != RenderGraph::Event::Draw)
        return;
      if (pass.id == trace)
        rt.trace(*c, slot, push);
      if (pass.id == copy)
        c.copyImageToBuffer(
            rt.image(), vk::ImageLayout::eTransferSrcOptimal, *download.buffer,
            {vk::BufferImageCopy{
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {extent, extent, 1}}});
    });
    c.end();
    auto raw = *c;
    d.graphicsQueue().submit(
        {vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &raw}},
        nullptr);
    rt.submitted(plan, image);
    d.graphicsQueue().waitIdle();
    std::array<glm::vec4, extent * extent> result;
    download.read(std::as_writable_bytes(std::span(result)));
    for (auto v : result)
      for (unsigned i = 0; i < 4; ++i)
        require(std::isfinite(v[i]) && v[i] >= 0,
                "Path radiance nonfinite/negative");
    return result;
  }
};
void blockedLighting(Device const &d, Renderer const &scene,
                     RtInstanceInput plane, RtPush const &push) {
  RayTracingRenderer rt(d, {32, 32}, 2);
  auto const &geometry = RendererTransportTestAccess::geometry(scene);
  auto textures = RendererTransportTestAccess::textures(scene);
  std::array<GpuBuffer, 2> outputs;
  for (auto &b : outputs)
    b = d.createBuffer(32 * 32 * 16, vk::BufferUsageFlagBits::eTransferDst,
                       vk::MemoryPropertyFlagBits::eHostVisible);
  vk::raii::CommandPool pool(
      d.logicalDevice(), {.queueFamilyIndex = d.graphicsQueueFamilyIndex()});
  vk::raii::CommandBuffers commands(d.logicalDevice(),
                                    {.commandPool = *pool,
                                     .level = vk::CommandBufferLevel::ePrimary,
                                     .commandBufferCount = 2});
  vk::SemaphoreTypeCreateInfo type{
      .semaphoreType = vk::SemaphoreType::eTimeline, .initialValue = 0};
  vk::raii::Semaphore gate(d.logicalDevice(), {.pNext = &type});
  auto sem = *gate;
  std::uint64_t value = 1;
  vk::TimelineSemaphoreSubmitInfo timeline{.waitSemaphoreValueCount = 1,
                                           .pWaitSemaphoreValues = &value};
  auto stage = vk::PipelineStageFlags(vk::PipelineStageFlagBits::eAllCommands);
  d.graphicsQueue().submit({vk::SubmitInfo{.pNext = &timeline,
                                           .waitSemaphoreCount = 1,
                                           .pWaitSemaphores = &sem,
                                           .pWaitDstStageMask = &stage}},
                           nullptr);
  bool released = false;
  try {
    for (unsigned slot = 0; slot < 2; ++slot) {
      RtLightingFrame frame;
      frame.transport = true;
      frame.settings.counts.w = 1;
      frame.settings.sampling.w = 1;
      frame.settings.sunDirection = {0, 0, 1, 3.14159265359f};
      frame.settings.sunColor =
          slot == 0 ? glm::vec4(1, 0, 0, 1) : glm::vec4(0, 1, 0, 1);
      rt.prepare(slot, geometry, std::span{&plane, 1}, textures, frame);
      RenderGraph g;
      auto image = rt.importTarget(g);
      auto trace =
          g.addPass("trace", {{image,
                               rt.accumulatedSamples()
                                   ? RenderGraph::Usage::RayTracingReadWrite
                                   : RenderGraph::Usage::RayTracingWrite,
                               vk::AttachmentLoadOp::eDontCare,
                               vk::AttachmentStoreOp::eStore, true}});
      auto copy =
          g.addPass("read", {{image, RenderGraph::Usage::TransferSource}});
      auto plan = g.compile();
      auto &c = commands[slot];
      c.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
      rt.build(*c, slot);
      plan.record(*c, [&](auto const &pass, auto event) {
        if (event != RenderGraph::Event::Draw)
          return;
        if (pass.id == trace)
          rt.trace(*c, slot, push);
        if (pass.id == copy)
          c.copyImageToBuffer(
              rt.image(), vk::ImageLayout::eTransferSrcOptimal,
              *outputs[slot].buffer,
              {vk::BufferImageCopy{
                  .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0,
                                       1},
                  .imageExtent = {32, 32, 1}}});
      });
      c.end();
      auto raw = *c;
      d.graphicsQueue().submit(
          {vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &raw}},
          nullptr);
      rt.submitted(plan, image);
    }
    require(rt.accumulatedSamples() == 1,
            "Pending lighting change failed history invalidation");
    d.logicalDevice().signalSemaphore({.semaphore = sem, .value = 1});
    released = true;
    d.graphicsQueue().waitIdle();
  } catch (...) {
    if (!released)
      d.logicalDevice().signalSemaphore({.semaphore = sem, .value = 1});
    d.graphicsQueue().waitIdle();
    throw;
  }
  for (unsigned slot = 0; slot < 2; ++slot) {
    std::array<glm::vec4, 1024> data;
    outputs[slot].read(std::as_writable_bytes(std::span(data)));
    auto expected = slot == 0 ? glm::vec3(plane.tint.x, 0, 0)
                              : glm::vec3(0, plane.tint.y, 0);
    require(glm::length(glm::vec3(data[16 * 32 + 16]) - expected) < 1e-5,
            "Pending RT light/header descriptor snapshots alias");
  }
}
glm::vec3 mean(std::span<glm::vec4 const> image) {
  glm::dvec3 total(0);
  for (auto v : image)
    total += glm::dvec3(v);
  return glm::vec3(total / double(image.size()));
}
} // namespace
void exerciseRayTransport(Device const &device, SwapChain const &swapchain,
                          unsigned slots) {
  require(device.rayTracingSupported(), "SKIP RT: unavailable features");
  {
    Renderer scene(device, slots);
    scene.recreateForSwapChain(swapchain);
    AssetLibrary assets;
    assets.meshes = {quad({0, 0, 0}, {0, 0, 1}, 10),
                     quad({.6f, 0, 1}, {0, 0, 1}, .3f),
                     quad({0, 0, 0}, glm::normalize(glm::vec3(1, 0, 1)), 4),
                     quad({2, 0, 0}, {-1, 0, 0}, 5),
                     quad({1, 0, 0}, {-1, 0, 0}, 20),
                     quad({0, 0, 1}, {0, 0, -1}, 1)};
    assets.materials.push_back(Material{});
    auto candidate = scene.prepareScene(assets);
    scene.waitSceneUpload(candidate);
    require(scene.commitScene(candidate), "Transport geometry failed commit");
    Harness h(device, scene, slots);
    RtLightingFrame frame;
    frame.settings.counts.w = 1;
    frame.settings.environment = {0, 0, 1, 0};
    frame.settings.sunDirection = {0, 0, 1, 3.14159265359f};
    frame.settings.sunColor = {1, 1, 1, 1};
    RtInstanceInput plane;
    plane.tint = {.25f, .5f, .75f, 1};
    plane.specular.w = 0;
    std::vector<RtInstanceInput> inputs{plane};
    if (slots == 2)
      blockedLighting(device, scene, plane, h.push);
    auto image = h.run(inputs, frame, slots);
    auto middle = image[16 * 32 + 16];
    require(glm::length(glm::vec3(middle) - glm::vec3(.25, .5, .75)) < 1e-4,
            "Directional Lambert reference differs");
    auto direct = mean(image);
    frame.settings.sunDirection =
        glm::vec4(glm::normalize(glm::vec3(.6, 0, 1)), 3.14159265359f);
    image = h.run(inputs, frame, slots);
    auto lit = image[16 * 32 + 16];
    require(glm::length(glm::vec3(lit) -
                        glm::vec3(plane.tint) * frame.settings.sunDirection.z) <
                1e-4,
            "Directional angular reference differs");
    RtInstanceInput blocker;
    blocker.mesh = 1;
    blocker.primaryVisible = false;
    blocker.doubleSided = true;
    inputs.push_back(blocker);
    image = h.run(inputs, frame, slots);
    middle = image[16 * 32 + 16];
    require(glm::length(glm::vec3(middle)) < 1e-6,
            "Hidden offscreen caster failed real shadow ray");
    inputs.back().shadowCaster = false;
    image = h.run(inputs, frame, slots);
    require(glm::length(glm::vec3(image[16 * 32 + 16]) - glm::vec3(lit)) < 1e-4,
            "Caster flag did not restore direct illumination");
    inputs.resize(1);
    frame.settings.sunColor.w = 0;
    GpuPunctualLight point{
        {0, 0, 1, 0}, {0, 0, -1, 1}, {1, 1, 1, 3.14159265359f}, {1, 1, -1, 1}};
    frame.punctual = std::span{&point, 1};
    image = h.run(inputs, frame, slots);
    float coordinate = 1.f / 32;
    float d2 = 1 + 2 * coordinate * coordinate;
    float scale = 1 / (d2 * std::sqrt(d2));
    require(glm::length(glm::vec3(image[16 * 32 + 16]) -
                        glm::vec3(plane.tint) * scale) < 1e-4,
            "Point inverse-square/NoL reference differs");
    point.positionRange.w = .5f;
    image = h.run(inputs, frame, slots);
    require(glm::length(mean(image)) < 1e-6, "Point range failed cutoff");
    point.positionRange.w = 0;
    point.directionType.w = 2;
    point.cones = {std::cos(.2f), std::cos(.4f), -1, 0};
    image = h.run(inputs, frame, slots);
    require(glm::length(glm::vec3(image[16 * 32 + 16]) -
                        glm::vec3(plane.tint) * scale) < 1e-4,
            "Spot cone center differs");
    point.directionType = {1, 0, 0, 2};
    image = h.run(inputs, frame, slots);
    require(glm::length(mean(image)) < 1e-6,
            "Spot cone failed direction rejection");
    frame.punctual = {};
    // Constant environment and two-technique MIS: expected Lambert radiance
    // rho*L.
    std::array<float, 24> envPixels;
    envPixels.fill(.4f);
    UploadBatch upload(device);
    auto env = TextureLoader(device).createFromHdrCube(envPixels, 1, upload);
    upload.finish();
    frame.environment = {env.sampler(), env.imageView(),
                         vk::ImageLayout::eShaderReadOnlyOptimal};
    frame.settings.environment.x = 1;
    frame.settings.counts.w = 2;
    frame.settings.sampling.x = 64;
    h.run(inputs, frame, slots);
    image = h.run(inputs, frame, slots);
    auto environment = mean(image);
    std::cout << "env observed " << environment.x << ' ' << environment.y << ' '
              << environment.z << " samples=" << h.rt.accumulatedSamples()
              << '\n';
    require(glm::length(environment - glm::vec3(plane.tint) * .4f) < .004f,
            "Environment MIS is biased/double-counted");
    require(h.rt.accumulatedSamples() == 128,
            "Static scene did not accumulate across frame slots");
    frame.settings.sampling.z = 0;
    frame.settings.sampling.y = 123;
    auto rawA = h.run(inputs, frame, slots);
    frame.settings.sampling.y = 456;
    auto rawB = h.run(inputs, frame, slots);
    frame.settings.sampling.z = 1;
    frame.settings.sampling.y = 123;
    h.rt.resetAccumulation();
    auto averageA = h.run(inputs, frame, slots);
    frame.settings.sampling.y = 456;
    auto averageB = h.run(inputs, frame, slots);
    for (unsigned i = 0; i < rawA.size(); ++i)
      require(glm::length(glm::vec3(averageB[i]) -
                          .5f * glm::vec3(rawA[i] + rawB[i])) < 2e-6f,
              "GPU running mean differs from independent arithmetic average");
    require(h.rt.accumulatedSamples() == 128,
            "Explicit sequence changed physical-history key");
    frame.settings.sampling.y = 0;
    frame.settings.environment.x = 2;
    image = h.run(inputs, frame, slots);
    require(h.rt.accumulatedSamples() == 64,
            "Lighting edit retained stale accumulation");
    require(glm::length(mean(image) - glm::vec3(plane.tint) * .8f) < .012f,
            "Environment intensity did not scale radiance");
    h.push.inverseViewProjection[3].x += .01f;
    h.run(inputs, frame, slots);
    require(h.rt.accumulatedSamples() == 64,
            "Camera edit retained stale accumulation");
    h.rt.resetAccumulation();
    h.run(inputs, frame, slots);
    require(h.rt.accumulatedSamples() == 64, "Explicit history reset failed");
    frame.settings.environment.x = 0;
    frame.settings.sunColor.w = 0;
    frame.settings.sampling.x = 64;
    frame.settings.counts.w = 1;
    RtInstanceInput mirror;
    mirror.mesh = 2;
    mirror.tint = {.8, .8, .8, 1};
    mirror.pbr = {1, .04, 1, 0};
    RtInstanceInput emitter;
    emitter.mesh = 3;
    emitter.primaryVisible = false;
    emitter.shadowCaster = false;
    emitter.doubleSided = true;
    emitter.emission = {1, .2, .1, 0};
    emitter.sourceAreaLight = true;
    inputs = {mirror, emitter};
    image = h.run(inputs, frame, slots);
    require(glm::length(mean(image)) < 1e-6,
            "Single-event mirror received invented reflected light");
    frame.settings.counts.w = 2;
    image = h.run(inputs, frame, slots);
    require(image[16 * 32 + 16].r > .5f && image[16 * 32 + 16].g > .08f,
            "Glossy reflection did not hit offscreen emissive geometry");
    // An ordinary finite area lamp, including emitter-hit MIS and endpoint
    // visibility.
    RtInstanceInput lamp;
    lamp.mesh = 5;
    lamp.primaryVisible = false;
    lamp.shadowCaster = true;
    lamp.emission = {1, 1, 1, 0};
    lamp.sourceAreaLight = true;
    lamp.maps.z = 0;
    lamp.maps.w = 2;
    inputs = {plane, lamp};
    std::array<RtAreaLight, 2> areas;
    for (unsigned primitive = 0; primitive < 2; ++primitive) {
      auto tri = h.geometry.triangle(5, primitive);
      areas[primitive] = {glm::vec4(tri[0], 2),
                          glm::vec4(tri[1], .5f),
                          glm::vec4(tri[2], .5f * (primitive + 1)),
                          {0, 0, -1, 0},
                          {1, primitive, 0, 0}};
    }
    frame.areas = areas;
    frame.settings.counts.w = 2;
    frame.settings.sunColor.w = 0;
    h.run(inputs, frame, slots);
    image = h.run(inputs, frame, slots);
    glm::dvec3 observed(0), expected(0);
    double cell = 2.0 / 128.0;
    for (unsigned y = 12; y < 20; ++y)
      for (unsigned x = 12; x < 20; ++x) {
        observed += glm::dvec3(image[y * 32 + x]);
        double px = -1 + (x + .5) * 2 / 32, py = -1 + (y + .5) * 2 / 32;
        double integral = 0;
        // Include the small camera translation exercised above.
        px += .01;
        for (unsigned j = 0; j < 128; ++j)
          for (unsigned i = 0; i < 128; ++i) {
            double dx = -1 + (i + .5) * cell - px,
                   dy = -1 + (j + .5) * cell - py;
            double r2 = 1 + dx * dx + dy * dy;
            integral += cell * cell / (r2 * r2);
          }
        expected += glm::dvec3(plane.tint) * integral / 3.141592653589793;
      }
    observed /= 64;
    expected /= 64;
    std::cout << "area observed=" << observed.x << " reference=" << expected.x
              << '\n';
    require(glm::length(observed - expected) < .012,
            "Area sampling/MIS/endpoint visibility differs from quadrature");
    frame.areas = {};
    // Diffuse interreflection from a red wall that alone receives lateral sun.
    plane.tint = {.5, .5, .5, 1};
    RtInstanceInput wall;
    wall.mesh = 4;
    wall.tint = {.8, 0, 0, 1};
    wall.specular.w = 0;
    wall.doubleSided = true;
    inputs = {plane, wall};
    frame.settings.sunDirection = {-1, 0, 0, 3.14159265359f};
    frame.settings.sunColor = {1, 1, 1, 1};
    frame.settings.environment.z = 0;
    frame.settings.counts.w = 1;
    image = h.run(inputs, frame, slots);
    require(glm::length(mean(image)) < 1e-6,
            "Direct-only receiver should be dark");
    frame.settings.counts.w = 2;
    h.run(inputs, frame, slots);
    image = h.run(inputs, frame, slots);
    auto bounce2 = mean(image);
    require(bounce2.r > .1f && bounce2.g < 1e-6,
            "Diffuse secondary hit failed color bleeding");
    frame.settings.counts.w = 4;
    h.run(inputs, frame, slots);
    image = h.run(inputs, frame, slots);
    auto bounce4 = mean(image);
    std::cout << "diffuse bounce2=" << bounce2.r << " bounce4=" << bounce4.r
              << '\n';
    require(bounce4.r > bounce2.r + .005f,
            "Additional diffuse bounce cycles did not transport energy");
  }
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "Transport resources leaked");
  std::cout << "PASS RT-B "
               "direct/occlusion/point/spot/environmentMIS/reflection/"
               "multibounce/accumulation/reset/lifetime\n";
}
namespace {
Mesh glassBox(float thickness, float half = 2) {
  Mesh m;
  for (auto p : {glm::vec3{-half, -half, -thickness / 2},
                 glm::vec3{half, -half, -thickness / 2},
                 glm::vec3{half, half, -thickness / 2},
                 glm::vec3{-half, half, -thickness / 2},
                 glm::vec3{-half, -half, thickness / 2},
                 glm::vec3{half, -half, thickness / 2},
                 glm::vec3{half, half, thickness / 2},
                 glm::vec3{-half, half, thickness / 2}})
    m.vertices.push_back(Vertex{.position = p,
                                .color = {1, 1, 1},
                                .normal = glm::normalize(p),
                                .uv = {.5, .5}});
  m.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
               1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
  return m;
}
} // namespace
void exerciseDielectricRT(Device const &device, SwapChain const &swapchain,
                          unsigned slots) {
  require(device.rayTracingSupported(), "SKIP RT: unavailable features");
  {
    Renderer scene(device, slots);
    scene.recreateForSwapChain(swapchain);
    AssetLibrary assets;
    assets.meshes = {glassBox(.001f),
                     quad({0, 0, -1}, {0, 0, 1}, 10),
                     quad({0, 0, 0}, {0, 0, 1}, 10),
                     glassBox(.5f),
                     quad({2, 0, -1}, {0, 0, 1}, 2),
                     quad({-2, 0, -1}, {0, 0, 1}, 2),
                     quad({0, 0, 1}, {0, 0, -1}, 10),
                     quad({2.3f, 0, 0}, {-1, 0, 0}, 10)};
    assets.materials.push_back(Material{});
    auto candidate = scene.prepareScene(assets);
    scene.waitSceneUpload(candidate);
    require(scene.commitScene(candidate), "Glass geometry failed commit");
    Harness h(device, scene, slots);
    RtLightingFrame frame;
    frame.settings.counts.w = 16;
    frame.settings.sampling.x = 64;
    frame.settings.environment = {0, 0, 0, 0};
    RtInstanceInput glass;
    glass.mesh = 0;
    glass.doubleSided = true;
    glass.optical = {1.5f, 1, 1, 1};
    glass.alpha = {2, .5, 0, 0};
    glass.tint.a = .35f;
    RtInstanceInput emitter;
    emitter.mesh = 1;
    emitter.sourceAreaLight = true;
    emitter.emission = {1, 1, 1, 0};
    emitter.shadowCaster = false;
    std::vector<RtInstanceInput> input{glass, emitter};
    h.run(input, frame, slots);
    auto image = h.run(input, frame, slots);
    auto observed = mean(image);
    double f = .04, trans = (1 - f) / (1 + f);
    std::cout << "slab T=" << observed.x << " reference=" << trans << '\n';
    require(glm::length(observed - glm::vec3(trans)) < .012,
            "Normal slab Fresnel/enter-exit/radiance eta weights incorrect");
    input[0].absorptionThickness = {1000, 2000, 0, 0};
    h.run(input, frame, slots);
    image = h.run(input, frame, slots);
    observed = mean(image);
    glm::vec3 expected;
    for (unsigned c = 0; c < 3; ++c) {
      double a = std::exp(-double(input[0].absorptionThickness[c]) * .001);
      expected[c] = float((1 - f) * (1 - f) * a / (1 - f * f * a * a));
    }
    std::cout << "millimeter absorption " << observed.x << ' ' << observed.y
              << ' ' << observed.z << " expected " << expected.x << ' '
              << expected.y << ' ' << expected.z << '\n';
    require(glm::length(observed - expected) < .015,
            "Millimeter Beer distance/offset compensation incorrect");
    input[0].optical.x = 1;
    h.run(input, frame, slots);
    image = h.run(input, frame, slots);
    observed = mean(image);
    require(glm::length(observed - glm::vec3(std::exp(-1), std::exp(-2), 1)) <
                1e-4,
            "Matched-IOR slab should equal analytic Beer law");
    input[0].transform=glm::scale(glm::mat4(1),glm::vec3(-1,1,1));
    h.run(input,frame,slots);
    image=h.run(input,frame,slots);
    require(glm::length(mean(image)-observed)<1e-4,
            "Mirrored solid changed outward medium orientation/Beer distance");
    input[0].transform=glm::mat4(1);
    input[0].mesh = 2;
    input[0].optical = {1.5, 1, 0, 1};
    input[0].absorptionThickness = {0, 0, 0, 0};
    h.run(input, frame, slots);
    image = h.run(input, frame, slots);
    require(glm::length(mean(image) - glm::vec3(trans)) < .012,
            "Thin sheet two-interface transmission incorrect");
    // Optical alpha is not a collection of alpha=.35 holes.
    input[0].tint.a = 1;
    h.run(input, frame, slots);
    image = h.run(input, frame, slots);
    require(glm::length(mean(image) - glm::vec3(trans)) < .012,
            "Converted alpha changed physical optical throughput");
    // Oblique finite slab changes the target half-plane according to Snell.
    RtInstanceInput red = emitter, blue = emitter;
    red.mesh = 4;
    red.emission = {1, 0, 0, 0};
    blue.mesh = 5;
    blue.emission = {0, 0, 1, 0};
    auto view = glm::lookAt(glm::vec3(-2.9f, 0, 2), glm::vec3(.1f, 0, -1),
                            glm::vec3(0, 1, 0));
    auto projection = glm::ortho(-.2f, .2f, -.2f, .2f, .1f, 20.f);
    projection[1][1] *= -1;
    h.push.inverseViewProjection = glm::inverse(projection * view);
    input = {red, blue};
    image = h.run(input, frame, slots);
    require(image[16 * 32 + 16].r > .9,
            "Oblique control did not hit red half plane");
    glass.mesh = 3;
    glass.optical = {1.5, 1, 1, 1};
    glass.tint = {1, 1, 1, 1};
    input = {glass, red, blue};
    h.run(input, frame, slots);
    image = h.run(input, frame, slots);
    require(image[16 * 32 + 16].b > .6 && image[16 * 32 + 16].r < .2,
            "Snell displacement did not change background hit");
    // Camera starts inside: 60-degree top incidence must internally reflect to
    // the side.
    RtInstanceInput top = emitter, side = emitter;
    top.mesh = 6;
    top.emission = {1, 0, 0, 0};
    side.mesh = 7;
    side.emission = {0, 1, 0, 0};
    input = {glass, top, side};
    view = glm::lookAt(glm::vec3(0),
                       glm::vec3(std::sin(1.04719755), 0, std::cos(1.04719755)),
                       glm::vec3(0, 1, 0));
    projection = glm::ortho(-.01f, .01f, -.01f, .01f, .001f, 20.f);
    projection[1][1] *= -1;
    h.push.inverseViewProjection = glm::inverse(projection * view);
    h.run(input, frame, slots);
    image = h.run(input, frame, slots);
    auto middle = image[16 * 32 + 16];
    std::cout << "inside TIR target " << middle.r << ' ' << middle.g << ' '
              << middle.b << '\n';
    require(middle.g > 1 && middle.r < .1,
            "TIR/camera-inside medium handling failed");
    // Straight colored shadow approximation: hidden pane must attenuate instead
    // of fully block.
    view = glm::lookAt(glm::vec3(0, 0, 2), glm::vec3(0), glm::vec3(0, 1, 0));
    projection = glm::ortho(-1.f, 1.f, -1.f, 1.f, .1f, 20.f);
    projection[1][1] *= -1;
    h.push.inverseViewProjection = glm::inverse(projection * view);
    RtInstanceInput receiver;
    receiver.mesh = 1;
    receiver.specular.w = 0;
    receiver.tint = {1, 1, 1, 1};
    glass.mesh = 0;
    glass.primaryVisible = false;
    glass.shadowCaster = true;
    glass.optical = {1, 1, 1, 1};
    glass.absorptionThickness = {1000, 2000, 0, 0};
    input = {receiver, glass};
    frame.settings.counts.w = 1;
    frame.settings.environment.z = 1;
    frame.settings.sunDirection = {0, 0, 1, 3.14159265359f};
    frame.settings.sunColor = {1, 1, 1, 1};
    image = h.run(input, frame, slots);
    require(glm::length(mean(image) -
                        glm::vec3(std::exp(-1), std::exp(-2), 1)) < 1e-4,
            "Colored closest-hit glass shadow approximation lost absorption");
  }
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "Dielectric RT resources leaked");
  std::cout << "PASS RT-C "
               "solid/thin/alpha/Beer/millimeter/Snell/TIR/camera-inside/"
               "colored shadow/retirement\n";
}
