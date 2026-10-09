#include "gtao.hpp"
#include "hdr_output_regression.hpp"
#include "renderer.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/packing.hpp>
#include <iostream>
#include <stdexcept>
struct RendererAoTestAccess {
  static bool ready(Renderer const &r) { return r.taa_->ready(); }
};
namespace {
void check(bool ok, char const *why) {
  if (!ok)
    throw std::runtime_error(why);
}
constexpr unsigned size = 64, pixels = size * size;
struct Input {
  GpuImage image;
  vk::raii::ImageView view = nullptr;
  RenderGraph::State state;
};
Input input(Device const &d, vk::Format format, bool depth) {
  Input t;
  auto aspect =
      depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
  auto usage = vk::ImageUsageFlagBits::eSampled |
               vk::ImageUsageFlagBits::eTransferDst |
               (depth ? vk::ImageUsageFlagBits::eDepthStencilAttachment
                      : vk::ImageUsageFlagBits::eColorAttachment);
  t.image = d.createImage({.imageType = vk::ImageType::e2D,
                           .format = format,
                           .extent = {size, size, 1},
                           .mipLevels = 1,
                           .arrayLayers = 1,
                           .samples = vk::SampleCountFlagBits::e1,
                           .tiling = vk::ImageTiling::eOptimal,
                           .usage = usage,
                           .sharingMode = vk::SharingMode::eExclusive},
                          pixels * (depth ? 4 : 8),
                          vk::MemoryPropertyFlagBits::eDeviceLocal);
  t.view = vk::raii::ImageView(d.logicalDevice(),
                               {.image = *t.image.image,
                                .viewType = vk::ImageViewType::e2D,
                                .format = format,
                                .subresourceRange = {aspect, 0, 1, 0, 1}});
  return t;
}
struct ReferenceBox {
  glm::vec3 lo{}, hi{};
  bool present = false;
};
float rayBox(glm::vec3 origin, glm::vec3 direction, ReferenceBox const &box) {
  if (!box.present)
    return INFINITY;
  float near = 0, far = INFINITY;
  for (unsigned axis = 0; axis < 3; ++axis) {
    if (std::abs(direction[axis]) < 1e-8f) {
      if (origin[axis] < box.lo[axis] || origin[axis] > box.hi[axis])
        return INFINITY;
    } else {
      float a = (box.lo[axis] - origin[axis]) / direction[axis],
            b = (box.hi[axis] - origin[axis]) / direction[axis];
      near = std::max(near, std::min(a, b));
      far = std::min(far, std::max(a, b));
    }
  }
  return near <= far && far > 1e-6f ? near : INFINITY;
}
struct ReferenceScene {
  std::string name;
  float scale = 1, tilt = 0, radius = .5f;
  glm::vec3 camera{};
  glm::vec2 jitterPixels{};
  ReferenceBox box;
};
struct ReferenceData {
  std::array<float, pixels> depth, reference;
  std::array<bool, pixels> top, receiver;
  glm::mat4 inverse;
};
ReferenceData referenceScene(ReferenceScene const &scene,
                             unsigned samples = 2048) {
  ReferenceData result;
  auto projection = glm::perspective(glm::radians(60.f), 1.f, .1f * scene.scale,
                                     20.f * scene.scale);
  projection[1][1] *= -1;
  auto raster = applyTemporalJitter(
      projection * glm::translate(glm::mat4(1), -scene.camera),
      scene.jitterPixels / float(size));
  result.inverse = glm::inverse(raster);
  glm::vec3 normal{std::sin(glm::radians(scene.tilt)), 0,
                   std::cos(glm::radians(scene.tilt))};
  glm::vec3 center{0, 0, -3 * scene.scale};
  auto box = scene.box;
  box.lo *= scene.scale;
  box.hi *= scene.scale;
  // Deterministic stratified cosine-weighted hemisphere directions; the ray
  // reference shares no horizon search/integration code with the shader.
  std::vector<glm::vec3> directions(samples);
  glm::vec3 tangent = glm::normalize(glm::cross(normal, glm::vec3{0, 1, 0}));
  glm::vec3 bitangent = glm::cross(normal, tangent);
  for (unsigned i = 0; i < directions.size(); ++i) {
    float r = std::sqrt((float(i) + .5f) / directions.size());
    float phi = glm::two_pi<float>() * std::fmod(float(i) * .61803398875f, 1.f);
    directions[i] = tangent * (r * std::cos(phi)) +
                    bitangent * (r * std::sin(phi)) +
                    normal * std::sqrt(1 - r * r);
  }
  for (unsigned y = 0; y < size; ++y)
    for (unsigned x = 0; x < size; ++x) {
      unsigned index = y * size + x;
      auto p =
          result.inverse * glm::vec4(2 * (float(x) + .5f) / size - 1,
                                     2 * (float(y) + .5f) / size - 1, 0, 1);
      auto ray = glm::normalize(glm::vec3(p) / p.w - scene.camera);
      float planeDistance =
          glm::dot(center - scene.camera, normal) / glm::dot(ray, normal);
      float boxDistance = rayBox(scene.camera, ray, box);
      bool boxHit = boxDistance > 0 && boxDistance < planeDistance;
      float distance = boxHit ? boxDistance : planeDistance;
      auto position = scene.camera + ray * distance;
      auto clip = raster * glm::vec4(position, 1);
      float depth = clip.z / clip.w;
      bool visible = distance > 0 && depth >= 0 && depth < 1;
      result.depth[index] = visible ? depth : 1;
      result.top[index] = visible && boxHit &&
                          std::abs(position.z - box.hi.z) < 1e-4f * scene.scale;
      result.receiver[index] = visible && !boxHit;
      // Forward-facing box front is unobstructed. Reference the receiver with
      // hemisphere rays; hidden box sides are deliberately part of this source.
      result.reference[index] = 1;
      if (result.receiver[index] && box.present) {
        float obscurance = 0, radius = scene.radius * scene.scale;
        for (auto direction : directions) {
          float hit =
              rayBox(position + normal * (scene.scale * 1e-5f), direction, box);
          if (hit < radius) {
            float t = std::clamp((hit / radius - .6f) / .4f, 0.f, 1.f);
            obscurance += 1 - t * t * (3 - 2 * t);
          }
        }
        result.reference[index] = 1 - obscurance / directions.size();
      }
    }
  return result;
}
void saveQuality(std::filesystem::path const &dir, std::string const &name,
                 ReferenceData const &data,
                 std::array<std::uint16_t, pixels * 6> const &gpu) {
  std::array<float, pixels * 4> values;
  for (unsigned i = 0; i < pixels; ++i) {
    values[4 * i] = data.reference[i];
    values[4 * i + 1] = glm::unpackHalf1x16(gpu[i]);
    values[4 * i + 2] = glm::unpackHalf1x16(gpu[pixels + i]);
    values[4 * i + 3] = data.depth[i];
  }
  std::ofstream f(dir / (name + ".reference_raw_filtered_depth.f32"),
                  std::ios::binary);
  f.write(reinterpret_cast<char const *>(values.data()), sizeof(values));
}

} // namespace
void exerciseGtao(Device const &d, SwapChain const &swapchain, unsigned slots) {
  for (float radius :
       {0.f, -1.f, 101.f, std::numeric_limits<float>::infinity()}) {
    bool rejected = false;
    try {
      validateAoSettings({.radius = radius});
    } catch (std::runtime_error const &) {
      rejected = true;
    } catch (std::invalid_argument const &) {
      rejected = true;
    }
    check(rejected, "Invalid AO radius accepted");
  }
  auto before = d.resourceLedger().snapshot().current;
  {
    auto hdr = input(d, Gtao::format(Gtao::Diffuse), false),
         depth = input(d, vk::Format::eD32Sfloat, true);
    Gtao effect(d, {size, size}, *hdr.view, *depth.view);
    auto upload =
        d.createBuffer(pixels * 4, vk::BufferUsageFlagBits::eTransferSrc,
                       vk::MemoryPropertyFlagBits::eHostVisible);
    auto download =
        d.createBuffer(pixels * 12, vk::BufferUsageFlagBits::eTransferDst,
                       vk::MemoryPropertyFlagBits::eHostVisible);
    vk::raii::CommandPool pool(
        d.logicalDevice(),
        {.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
         .queueFamilyIndex = d.graphicsQueueFamilyIndex()});
    vk::raii::CommandBuffers commands(
        d.logicalDevice(), {.commandPool = *pool,
                            .level = vk::CommandBufferLevel::ePrimary,
                            .commandBufferCount = 1});
    vk::raii::Fence fence(d.logicalDevice(), vk::FenceCreateInfo{});
    auto projection = glm::perspective(glm::radians(60.f), 1.f, .1f, 20.f);
    projection[1][1] *= -1;
    auto z = [&](float distance) {
      auto clip = projection * glm::vec4(0, 0, -distance, 1);
      return clip.z / clip.w;
    };
    GtaoPush push{.inverseRaster = glm::inverse(projection),
                  .camera = {0, 0, 0, 1},
                  .settings = {.5f, 1, 0, 128},
                  .screen = {size, size, 0, 0}};
    auto run = [&](bool blocker, bool sky, float radius, float strength,
                   unsigned debug, std::span<float const> depthOverride = {}) {
      std::array<float, pixels> values;
      values.fill(sky ? 1.f : z(3));
      if (blocker)
        for (unsigned y = 22; y < 42; ++y)
          for (unsigned x = 28; x < 38; ++x)
            values[y * size + x] = z(2.8f);
      if (!depthOverride.empty())
        std::copy(depthOverride.begin(), depthOverride.end(), values.begin());
      upload.write(std::as_bytes(std::span{values}));
      push.settings = {radius, strength, float(debug), 128};
      auto &cmd = commands[0];
      cmd.reset();
      cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
      vk::ImageMemoryBarrier2 b{
          .srcStageMask = depth.state.stages,
          .srcAccessMask = depth.state.access,
          .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
          .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
          .oldLayout = depth.state.layout,
          .newLayout = vk::ImageLayout::eTransferDstOptimal,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = *depth.image.image,
          .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}};
      cmd.pipelineBarrier2(
          {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
      cmd.copyBufferToImage(
          *upload.buffer, *depth.image.image,
          vk::ImageLayout::eTransferDstOptimal,
          {vk::BufferImageCopy{
              .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
              .imageExtent = {size, size, 1}}});
      depth.state = {vk::ImageLayout::eTransferDstOptimal,
                     vk::PipelineStageFlagBits2::eTransfer,
                     vk::AccessFlagBits2::eTransferWrite, true};
      using G = RenderGraph;
      G g;
      auto h = g.importImage({"Fixture HDR",
                              *hdr.image.image,
                              *hdr.view,
                              Gtao::format(Gtao::Diffuse),
                              {size, size},
                              vk::ImageAspectFlagBits::eColor,
                              vk::ImageUsageFlagBits::eColorAttachment |
                                  vk::ImageUsageFlagBits::eSampled,
                              false,
                              hdr.state});
      auto zimage =
          g.importImage({"Fixture depth",
                         *depth.image.image,
                         *depth.view,
                         vk::Format::eD32Sfloat,
                         {size, size},
                         vk::ImageAspectFlagBits::eDepth,
                         vk::ImageUsageFlagBits::eDepthStencilAttachment |
                             vk::ImageUsageFlagBits::eSampled |
                             vk::ImageUsageFlagBits::eTransferDst,
                         false,
                         depth.state});
      auto diffuse = effect.importDiffuse(g);
      g.addPass(
          "Fixture diffuse/direct/emissive",
          {{h, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore, false,
            vk::ClearValue{.color = vk::ClearColorValue{std::array<float, 4>{
                               1, 2, 3, 1}}}},
           {diffuse, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore, false,
            vk::ClearValue{.color = vk::ClearColorValue{
                               std::array<float, 4>{.25f, .5f, .75f, 1}}}}});
      auto f = effect.addPasses(g, h, zimage, diffuse);
      auto copy = g.addPass(
          "Read AO", {{f.images[Gtao::Raw], G::Usage::TransferSource},
                      {f.images[Gtao::Filtered], G::Usage::TransferSource},
                      {f.images[Gtao::Composite], G::Usage::TransferSource}});
      for (auto target : {Gtao::Raw, Gtao::Filtered, Gtao::Composite})
        g.exportImage(f.images[target], G::Usage::SampledColor);
      g.exportImage(zimage, G::Usage::SampledDepth);
      auto plan = g.compile();
      check(plan.dump().find("GTAO raw") != std::string::npos,
            "AO graph missing intermediate");
      plan.record(*cmd, [&](G::Pass const &pass, G::Event e) {
        if (e != G::Event::Draw)
          return;
        if (pass.id == f.horizon)
          effect.draw(*cmd, 0, push);
        else if (pass.id == f.filter)
          effect.draw(*cmd, 1, push);
        else if (pass.id == f.composite)
          effect.draw(*cmd, 2, push);
        else if (pass.id == copy) {
          std::uint64_t offset = 0;
          for (auto target : {Gtao::Raw, Gtao::Filtered, Gtao::Composite}) {
            cmd.copyImageToBuffer(
                effect.image(target), vk::ImageLayout::eTransferSrcOptimal,
                *download.buffer,
                {vk::BufferImageCopy{
                    .bufferOffset = offset,
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0,
                                         1},
                    .imageExtent = {size, size, 1}}});
            offset += pixels * (target == Gtao::Composite ? 8 : 2);
          }
          vk::BufferMemoryBarrier2 host{
              .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
              .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
              .dstStageMask = vk::PipelineStageFlagBits2::eHost,
              .dstAccessMask = vk::AccessFlagBits2::eHostRead,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .buffer = *download.buffer,
              .size = VK_WHOLE_SIZE};
          cmd.pipelineBarrier2(
              {.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host});
        }
      });
      cmd.end();
      d.logicalDevice().resetFences({*fence});
      auto raw = *cmd;
      d.graphicsQueue().submit(
          {vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &raw}},
          *fence);
      effect.submitted(plan, diffuse, &f);
      hdr.state = plan.finalState(h);
      depth.state = plan.finalState(zimage);
      check(d.logicalDevice().waitForFences({*fence}, true, UINT64_MAX) ==
                vk::Result::eSuccess,
            "AO fence failed");
      std::array<std::uint16_t, pixels * 6> result;
      download.read(std::as_writable_bytes(std::span{result}));
      return result;
    };
    auto flat = run(false, false, .5, 1, 0);
    float flatMin = 1;
    for (unsigned i = 0; i < pixels; ++i) {
      float a = glm::unpackHalf1x16(flat[i]);
      flatMin = std::min(flatMin, a);
      check(std::isfinite(a) && a > .94f, "Unoccluded plane self-darkened");
    }
    auto sky = run(false, true, .5, 1, 0);
    for (unsigned i = 0; i < pixels; ++i)
      check(sky[i] == glm::packHalf1x16(1), "Sky AO not white");
    auto contact = run(true, false, .5, 1, 0);
    float minAo = 1;
    for (unsigned i = 0; i < pixels; ++i) {
      float a = glm::unpackHalf1x16(contact[i]),
            f = glm::unpackHalf1x16(contact[pixels + i]);
      check(std::isfinite(a) && a >= 0 && a <= 1 && std::isfinite(f) &&
                f >= 0 && f <= 1,
            "AO outside finite unit range");
      minAo = std::min(minAo, a);
      for (unsigned c = 0; c < 3; ++c) {
        float actual = glm::unpackHalf1x16(contact[2 * pixels + 4 * i + c]);
        float expected = float(c + 1) * (1 - .25f * (1 - f));
        check(std::abs(actual - expected) < .003f,
              "AO darkened direct/emissive instead of isolated diffuse");
      }
    }
    check(minAo < .8f, "Contact fixture produces no occlusion");
    auto tiny = run(true, false, .001, 1, 0);
    for (unsigned i = 0; i < pixels; ++i)
      check(glm::unpackHalf1x16(tiny[i]) > .94f,
            "AO ignored finite world radius");
    auto zero = run(true, false, .5, 0, 0);
    for (unsigned i = 0; i < pixels; ++i)
      for (unsigned c = 0; c < 3; ++c)
        check(glm::unpackHalf1x16(zero[2 * pixels + 4 * i + c]) == float(c + 1),
              "Zero AO strength changes HDR");
    auto debug = run(true, false, .5, 1, 1);
    for (unsigned i = 0; i < pixels; ++i)
      check(debug[2 * pixels + 4 * i] == debug[i], "Raw AO debug mismatch");
    auto large = run(true, false, 100, 2, 2);
    for (unsigned i = 0; i < pixels; ++i)
      check(std::isfinite(glm::unpackHalf1x16(large[i])),
            "Large bounded radius produced NaN");
    std::vector<ReferenceScene> cases;
    for (float tilt : {0.f, 30.f, 60.f, 75.f})
      cases.push_back(
          {.name = "plane_" + std::to_string(int(tilt)), .tilt = tilt});
    ReferenceBox thick{{-.25f, -.5f, -3.f}, {.25f, .5f, -2.8f}, true};
    cases.push_back({.name = "box", .box = thick});
    auto thin = thick;
    thin.lo.x = -.045f;
    thin.hi.x = .045f;
    cases.push_back({.name = "thin", .box = thin});
    auto single = thin;
    single.lo.x = 0;
    single.hi.x = .05f;
    cases.push_back({.name = "thin_single", .box = single});
    auto edge = thick;
    edge.lo.x = 1.4f;
    edge.hi.x = 1.8f;
    cases.push_back({.name = "edge", .box = edge});
    auto outside = thick;
    outside.lo.x = 1.9f;
    outside.hi.x = 2.1f;
    cases.push_back({.name = "offscreen", .box = outside});
    for (float scale : {.1f, 10.f})
      cases.push_back({.name = "scale_" + std::to_string(scale),
                       .scale = scale,
                       .box = thick});
    std::filesystem::path output;
    if (auto dir = std::getenv("GTAO_QUALITY_OUTPUT")) {
      output = dir;
      std::filesystem::create_directories(output);
    }
    std::ofstream metrics;
    if (!output.empty()) {
      metrics.open(output / "metrics.csv");
      metrics << "case,raw_mae,filtered_mae,contact_mae,plane_max_error,top_"
                 "min,raw_min,receiver_pixels,contact_pixels\n";
    }
    float planeError = 0, topMin = 1, scaleError = 0, boxMae = 0,
          referenceDelta = 0;
    std::array<std::uint16_t, pixels * 6> scaleControl{};
    for (auto const &scene : cases) {
      auto reference = referenceScene(scene);
      push.inverseRaster = reference.inverse;
      push.camera = glm::vec4(scene.camera, 1);
      auto gpu =
          run(false, false, scene.radius * scene.scale, 1, 0, reference.depth);
      double rawError = 0, filteredError = 0, contactError = 0;
      unsigned receivers = 0, contacts = 0;
      float maximum = 0, top = 1, minimum = 1;
      for (unsigned i = 0; i < pixels; ++i) {
        float raw = glm::unpackHalf1x16(gpu[i]),
              filtered = glm::unpackHalf1x16(gpu[pixels + i]);
        check(std::isfinite(raw) && raw >= 0 && raw <= 1 &&
                  std::isfinite(filtered) && filtered >= 0 && filtered <= 1,
              "AO quality produced invalid value");
        minimum = std::min(minimum, raw);
        if (!scene.box.present)
          maximum =
              std::max({maximum, std::abs(raw - 1), std::abs(filtered - 1)});
        if (reference.top[i])
          top = std::min(top, filtered);
        if (reference.receiver[i]) {
          ++receivers;
          rawError += std::abs(raw - reference.reference[i]);
          filteredError += std::abs(filtered - reference.reference[i]);
          if (reference.reference[i] < .995f) {
            ++contacts;
            contactError += std::abs(filtered - reference.reference[i]);
          }
        }
      }
      rawError /= std::max(receivers, 1u);
      filteredError /= std::max(receivers, 1u);
      contactError /= std::max(contacts, 1u);
      planeError = std::max(planeError, maximum);
      topMin = std::min(topMin, top);
      if (scene.name == "box") {
        scaleControl = gpu;
        boxMae = float(filteredError);
      }
      if (scene.name.starts_with("scale_"))
        for (unsigned i = 0; i < pixels * 2; ++i)
          scaleError = std::max(scaleError,
                                std::abs(glm::unpackHalf1x16(gpu[i]) -
                                         glm::unpackHalf1x16(scaleControl[i])));
      std::cout << "AO quality " << scene.name << ": raw/filter MAE "
                << rawError << '/' << filteredError << ", contact "
                << contactError << ", plane error " << maximum << ", top min "
                << top << std::endl;
      if (scene.box.present && scene.scale == 1) {
        auto converged = referenceScene(scene, 8192);
        for (unsigned i = 0; i < pixels; ++i)
          referenceDelta =
              std::max(referenceDelta, std::abs(converged.reference[i] -
                                                reference.reference[i]));
      }
      if (metrics)
        metrics << scene.name << ',' << rawError << ',' << filteredError << ','
                << contactError << ',' << maximum << ',' << top << ','
                << minimum << ',' << receivers << ',' << contacts << '\n';
      if (!output.empty())
        saveQuality(output, scene.name, reference, gpu);
    }
    // Fixed 16-frame translation/jitter sequence on an unoccluded grazing
    // plane. This isolates AO's geometry sampling stability from any TAA
    // reconstruction.
    for (unsigned frame = 0; frame < 16; ++frame) {
      ReferenceScene scene{.name = "motion_plane_" + std::to_string(frame),
                           .tilt = 60};
      scene.camera.x = (frame < 8 ? .02f : .2f) *
                       std::sin(float(frame) * glm::pi<float>() * .25f);
      scene.jitterPixels = temporalJitterPixels(frame);
      auto reference = referenceScene(scene);
      push.inverseRaster = reference.inverse;
      push.camera = glm::vec4(scene.camera, 1);
      auto gpu = run(false, false, .5, 1, 0, reference.depth);
      for (unsigned i = 0; i < pixels * 2; ++i)
        planeError =
            std::max(planeError, std::abs(glm::unpackHalf1x16(gpu[i]) - 1));
      if (!output.empty())
        saveQuality(output, scene.name, reference, gpu);
    }
    std::cout << "AO quality 2048/8192-ray maximum reference delta="
              << referenceDelta
              << ", translated/jittered plane maximum error=" << planeError
              << std::endl;
    check(referenceDelta < .015f,
          "Hemisphere reference did not converge sufficiently");
    std::cout << "AO quality scale maximum delta=" << scaleError << std::endl;
    check(planeError <= .005f,
          "Unoccluded tilted plane differs from unit reference");
    check(topMin >= .95f, "Thin foreground AO contaminated by background");
    check(scaleError <= .006f, "AO not invariant under consistent world scale");
    check(boxMae <= .12f, "Visible box AO exceeds bounded reference error");
    std::cout << "GTAO flat min " << flatMin << ", contact min " << minAo
              << "; isolated diffuse composite/sky/range/debug pass (" << slots
              << "-slot harness)\n";
  }
  {
    Renderer r(d, slots);
    r.recreateForSwapChain(swapchain);
    AssetLibrary assets;
    Mesh quad;
    for (auto p : {glm::vec3{-1, -1, -3}, glm::vec3{1, -1, -3},
                   glm::vec3{1, 1, -3}, glm::vec3{-1, 1, -3}})
      quad.vertices.push_back(
          Vertex{.position = p, .color = {1, 1, 1}, .normal = {0, 0, 1}});
    quad.indices = {0, 1, 2, 0, 2, 3};
    assets.meshes.push_back(quad);
    Material material;
    material.doubleSided = true;
    assets.materials.push_back(material);
    auto prepared = r.prepareScene(assets);
    r.waitSceneUpload(prepared);
    check(r.commitScene(prepared), "AO renderer scene commit failed");
    auto projection = glm::perspective(
        glm::radians(60.f),
        float(swapchain.extent().width) / swapchain.extent().height, .1f, 20.f);
    projection[1][1] *= -1;
    std::array items{Renderer::DrawItem{.objectIndex = 1}};
    LightingSettings lighting;
    std::vector<GpuTimings> timings;
    r.setGpuTimingCallback([&](auto const &t) { timings.push_back(t); });
    auto draw = [&] {
      check(r.renderFrame({.opaque = items, .allOpaque = items, .sky = false},
                          projection, {0, 0, 0}, lighting,
                          false) == Renderer::FrameResult::eSuccess,
            "AO renderer frame failed");
    };
    r.setTaaEnabled(true);
    r.setAoSettings({.enabled = true});
    draw();
    check(RendererAoTestAccess::ready(r), "AO did not feed TAA");
    r.setAoSettings(r.aoSettings());
    check(RendererAoTestAccess::ready(r), "Unchanged AO reset history");
    r.setAoSettings({.enabled = true, .radius = .25});
    check(!RendererAoTestAccess::ready(r),
          "AO radius left stale colour history");
    draw();
    r.setAoSettings({.enabled = true, .debug = AoDebug::Raw});
    draw();
    check(!r.taaActive(), "AO debug accumulated TAA");
    r.setAoSettings({.enabled = false});
    draw();
    check(r.taaActive() && !r.aoActive(), "AO-off did not restore TAA");
    r.setAoSettings({.enabled = true});
    draw();
    draw();
    d.logicalDevice().waitIdle();
    r.collectCompletedWork();
    check(timings.size() == 6, "AO query delivery missing/duplicated");
    for (auto const &t : timings)
      check(t.valid && (t.frameId == 4 ? t.aoMs == 0 : t.aoMs > 0),
            "AO query state leaked across slots");
    r.recreateForSwapChain(swapchain);
    draw();
    d.logicalDevice().waitIdle();
    r.collectCompletedWork();
    check(r.aoActive() && RendererAoTestAccess::ready(r),
          "AO targets/history failed after recreation");
    std::cout << "PASS Renderer AO/TAA immutable alternate descriptors, "
                 "on/off/debug/parameter resets, "
              << slots << " slots, seven complete queries and recreation\n";
  }
  auto after = d.resourceLedger().snapshot().current;
  check(after == before, "GTAO fixture leaked resources");
}
