#include "hdr_output_regression.hpp"
#include "taa_resolve.hpp"
#include "temporal_motion.hpp"
#include <cmath>
#include <cstring>
#include <glm/gtc/packing.hpp>
#include <iostream>
#include <stdexcept>
namespace {
void check(bool v, char const *s) {
  if (!v)
    throw std::runtime_error(s);
}
struct Image {
  GpuImage storage;
  vk::raii::ImageView view = nullptr;
};
Image image(Device const &d, vk::Format format, bool depth) {
  Image t;
  unsigned bytes = depth ? 4 : 8;
  t.storage =
      d.createImage({.imageType = vk::ImageType::e2D,
                     .format = format,
                     .extent = {8, 8, 1},
                     .mipLevels = 1,
                     .arrayLayers = 1,
                     .samples = vk::SampleCountFlagBits::e1,
                     .tiling = vk::ImageTiling::eOptimal,
                     .usage = vk::ImageUsageFlagBits::eSampled |
                              vk::ImageUsageFlagBits::eTransferDst,
                     .sharingMode = vk::SharingMode::eExclusive},
                    8 * 8 * bytes, vk::MemoryPropertyFlagBits::eDeviceLocal);
  t.view = vk::raii::ImageView(
      d.logicalDevice(),
      {.image = *t.storage.image,
       .viewType = vk::ImageViewType::e2D,
       .format = format,
       .subresourceRange = {depth ? vk::ImageAspectFlagBits::eDepth
                                  : vk::ImageAspectFlagBits::eColor,
                            0, 1, 0, 1}});
  return t;
}
void transition(vk::CommandBuffer c, vk::Image i, vk::ImageLayout old,
                vk::ImageLayout next, bool depth = false) {
  vk::ImageMemoryBarrier2 b{
      .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
      .srcAccessMask =
          vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
      .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
      .dstAccessMask =
          vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
      .oldLayout = old,
      .newLayout = next,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = i,
      .subresourceRange = {depth ? vk::ImageAspectFlagBits::eDepth
                                 : vk::ImageAspectFlagBits::eColor,
                           0, 1, 0, 1}};
  if (old == vk::ImageLayout::eUndefined) {
    b.srcStageMask = {};
    b.srcAccessMask = {};
  }
  c.pipelineBarrier2(
      {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
}
} // namespace
void exerciseTaaResolve(Device const &d, SwapChain const &, unsigned) {
  {
    auto color = image(d, TaaResolve::colorFormat, false),
         depth = image(d, vk::Format::eD32Sfloat, true),
         motion = image(d, TaaResolve::colorFormat, false);
    TaaResolve taa(d, {8, 8}, *color.view, *depth.view, *motion.view);
    auto staging = d.createBuffer(512, vk::BufferUsageFlagBits::eTransferSrc,
                                  vk::MemoryPropertyFlagBits::eHostVisible);
    auto download = d.createBuffer(8, vk::BufferUsageFlagBits::eTransferDst,
                                   vk::MemoryPropertyFlagBits::eHostVisible);
    auto stagingB = d.createBuffer(512, vk::BufferUsageFlagBits::eTransferSrc,
                                   vk::MemoryPropertyFlagBits::eHostVisible);
    auto downloadB = d.createBuffer(8, vk::BufferUsageFlagBits::eTransferDst,
                                    vk::MemoryPropertyFlagBits::eHostVisible);
    auto &stagingA = staging;
    auto &downloadA = download;
    bool deferred = false;
    unsigned deferredSlot = 0;
    vk::raii::CommandPool pool(
        d.logicalDevice(),
        {.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
         .queueFamilyIndex = d.graphicsQueueFamilyIndex()});
    vk::raii::CommandBuffers cmds(d.logicalDevice(),
                                  {.commandPool = *pool,
                                   .level = vk::CommandBufferLevel::ePrimary,
                                   .commandBufferCount = 2});
    vk::raii::Fence fence(d.logicalDevice(), vk::FenceCreateInfo{});
    vk::raii::Fence fenceB(d.logicalDevice(), vk::FenceCreateInfo{});
    auto &fenceA = fence;
    bool initialized = false;
    TaaPush params{.depthRow = {0, 0, 0, 1},
                   .jitterWeight = {0, 0, .1f, 0},
                   .options = {1, .002f, 8, 1.5f}};
    auto frame = [&](unsigned phase, glm::vec4 mv = glm::vec4(0, 0, 1, 1),
                     bool uniform = false, float value = 0.f) {
      unsigned slot = deferred ? deferredSlot++ : 0;
      auto &staging = slot ? stagingB : stagingA;
      auto &download = slot ? downloadB : downloadA;
      auto &fence = slot ? fenceB : fenceA;
      std::array<std::uint16_t, 256> pixels{};
      for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
          float v =
              uniform
                  ? (value == -3
                         ? .5f +
                               .25f * std::cos((float(x) -
                                                temporalJitterPixels(phase).x) *
                                               1.57079632679f)
                     : value == -1 ? float(x) / 7.f
                     : value == -2 ? float(x ? x - 1 : 0) / 7.f
                                   : value)
                  : float((x + y + phase) % 2);
          if (uniform && value == -4)
            v = .5f + .25f * std::cos((float(x) + float(phase) * .5f) *
                                      1.57079632679f);
          if (uniform && value == -5)
            v = .5f + (((x + y + phase) % 2) ? .1f : -.1f);
          for (unsigned k = 0; k < 3; ++k)
            pixels[(y * 8 + x) * 4 + k] = glm::packHalf1x16(v);
          pixels[(y * 8 + x) * 4 + 3] = glm::packHalf1x16(1);
        }
      staging.write(std::as_bytes(std::span{pixels}));
      auto &cmd = cmds[slot];
      cmd.reset();
      cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
      for (auto pair : {std::pair{&color, false}, std::pair{&depth, true},
                        std::pair{&motion, false}})
        transition(*cmd, *pair.first->storage.image,
                   initialized
                       ? (pair.second ? vk::ImageLayout::eDepthReadOnlyOptimal
                                      : vk::ImageLayout::eShaderReadOnlyOptimal)
                       : vk::ImageLayout::eUndefined,
                   vk::ImageLayout::eTransferDstOptimal, pair.second);
      cmd.copyBufferToImage(
          *staging.buffer, *color.storage.image,
          vk::ImageLayout::eTransferDstOptimal,
          {vk::BufferImageCopy{
              .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
              .imageExtent = {8, 8, 1}}});
      cmd.clearDepthStencilImage(
          *depth.storage.image, vk::ImageLayout::eTransferDstOptimal, {.5f, 0},
          {{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}});
      cmd.clearColorImage(
          *motion.storage.image, vk::ImageLayout::eTransferDstOptimal,
          vk::ClearColorValue{std::array<float, 4>{mv.x, mv.y, mv.z, mv.w}},
          {{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
      transition(*cmd, *color.storage.image,
                 vk::ImageLayout::eTransferDstOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal);
      transition(*cmd, *depth.storage.image,
                 vk::ImageLayout::eTransferDstOptimal,
                 vk::ImageLayout::eDepthReadOnlyOptimal, true);
      transition(*cmd, *motion.storage.image,
                 vk::ImageLayout::eTransferDstOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal);
      using G = RenderGraph;
      G g;
      G::State read{vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eFragmentShader,
                    vk::AccessFlagBits2::eShaderSampledRead, true};
      auto usage = vk::ImageUsageFlagBits::eSampled |
                   vk::ImageUsageFlagBits::eTransferDst;
      auto c = g.importImage({"current",
                              *color.storage.image,
                              *color.view,
                              TaaResolve::colorFormat,
                              {8, 8},
                              vk::ImageAspectFlagBits::eColor,
                              usage,
                              false,
                              read});
      auto ds = read;
      ds.layout = vk::ImageLayout::eDepthReadOnlyOptimal;
      auto z = g.importImage({"depth",
                              *depth.storage.image,
                              *depth.view,
                              vk::Format::eD32Sfloat,
                              {8, 8},
                              vk::ImageAspectFlagBits::eDepth,
                              usage,
                              false,
                              ds});
      auto m = g.importImage({"motion",
                              *motion.storage.image,
                              *motion.view,
                              TaaResolve::colorFormat,
                              {8, 8},
                              vk::ImageAspectFlagBits::eColor,
                              usage,
                              false,
                              read});
      auto f = taa.addPass(g, c, z, m);
      auto copy = g.addPass("read resolved",
                            {{f.color[f.write], G::Usage::TransferSource}});
      g.exportImage(f.color[f.write], G::Usage::SampledColor);
      auto plan = g.compile();
      plan.record(*cmd, [&](auto const &pass, G::Event e) {
        if (e != G::Event::Draw)
          return;
        if (pass.id == f.resolve)
          taa.draw(*cmd, params);
        else if (pass.id == copy) {
          cmd.copyImageToBuffer(
              taa.colorImage(f.write), vk::ImageLayout::eTransferSrcOptimal,
              *download.buffer,
              {vk::BufferImageCopy{
                  .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0,
                                       1},
                  .imageOffset = {4, 4, 0},
                  .imageExtent = {1, 1, 1}}});
          vk::BufferMemoryBarrier2 b{
              .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
              .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
              .dstStageMask = vk::PipelineStageFlagBits2::eHost,
              .dstAccessMask = vk::AccessFlagBits2::eHostRead,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .buffer = *download.buffer,
              .size = 8};
          cmd.pipelineBarrier2(
              {.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b});
        }
      });
      cmd.end();
      vk::CommandBuffer raw = *cmd;
      d.logicalDevice().resetFences({*fence});
      d.graphicsQueue().submit(
          {vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &raw}},
          *fence);
      taa.submitted(plan, f);
      initialized = true;
      if (deferred)
        return 0.f;
      check(d.logicalDevice().waitForFences({*fence}, true, UINT64_MAX) ==
                vk::Result::eSuccess,
            "TAA fixture submit failed");

      std::array<std::uint16_t, 4> result;
      download.read(std::as_writable_bytes(std::span{result}));
      return glm::unpackHalf1x16(result[0]);
    };
    check(frame(0) == 0, "First resolve read undefined history");
    check(std::abs(frame(1) - .1f) < .001f,
          "Static history accumulation incorrect");
    check(std::abs(frame(0) - .09f) < .001f, "Ping pong previous frame lost");
    float v = 0;
    for (unsigned i = 0; i < 64; ++i)
      v = frame(i % 2);
    std::cout << "TAA convergence=" << v << std::endl;
    check(v > .45f && v < .56f,
          "Alternating subpixel samples did not converge");
    check(frame(0, {0, 0, 0, 1}) == 0, "Invalid motion accumulated");
    check(frame(1, {2, 0, 1, 1}) == 1, "Out of image history accumulated");
    check(frame(0, {0, 0, 1, 2}) == 0, "Disoccluded previous depth accepted");
    check(frame(1, {0, 0, .5f, 1}) == 1,
          "Transparent coverage history accepted");
    // A transparent pixel cannot seed the next exposed opaque/background
    // history.
    bool transparentHistoryRejected = frame(0, {0, 0, 1, 1}) == 0;
    taa.reset();
    float squareError = 0;
    float reference = 0;
    for (unsigned k = 0; k < 64; ++k)
      reference += .5f + .25f * std::cos((4.f + (float(k) + .5f) / 64.f - .5f) *
                                         1.57079632679f);
    reference /= 64;
    for (unsigned n = 0; n < 64; ++n) {
      auto previous = temporalJitterPixels(n ? n - 1 : 0);
      auto current = temporalJitterPixels(n);
      params.jitterWeight.x = (previous.x - current.x) / 8.f;
      params.jitterWeight.w = 1;
      float value = frame(n, {0, 0, 1, 1}, true, -3);
      if (n >= 56)
        squareError += (value - reference) * (value - reference);
    }
    float rms = std::sqrt(squareError / 8);
    std::cout << "Static Halton sine RMS=" << rms << " reference=" << reference
              << std::endl;
    check(transparentHistoryRejected,
          "Transparent history leaked after uncovering same-depth background");
    check(rms < .025f, "Static history lattice lost fine detail relative to "
                       "64-sample reference");
    params.jitterWeight.x = 0;
    params.jitterWeight.w = 0;
    taa.reset();
    frame(0, {0, 0, 1, 1}, true, -1);
    params.jitterWeight.x = .125f;
    check(std::abs(frame(0, {.125f, 0, 1, 1}, true, -2) - .42857143f) < .002f,
          "History output lattice incorrectly followed sampling jitter");
    params.jitterWeight.x = 0;
    taa.reset();
    frame(0, {0, 0, 1, 1}, true, -1);
    check(std::abs(frame(0, {.125f, 0, 1, 1}, true, -2) - .42857143f) < .002f,
          "Motion reprojection failed without jitter");
    check(std::abs(frame(0, {0, 0, 1, 1}, true, 4) - 4) < .005f,
          "HDR/clamp did not reject stale dark history");
    taa.reset();
    check(frame(0) == 0, "Reset retained history");
    // Compare fractional moving reconstruction with a continuous signal, not a
    // screenshot's apparent sharpness. Clamp and both filters see identical
    // data.
    params.jitterWeight.w = 0;
    taa.reset();
    frame(0, {0, 0, 1, 1}, true, -4);
    float bilinear = frame(1, {-.0625f, 0, 1, 1}, true, -4);
    params.jitterWeight.w = 1;
    taa.reset();
    frame(0, {0, 0, 1, 1}, true, -4);
    float cubic = frame(1, {-.0625f, 0, 1, 1}, true, -4);
    float movingReference = .5f + .25f * std::cos(4.5f * 1.57079632679f);
    std::cout << "Moving sine bilinear=" << bilinear << " CR=" << cubic
              << " reference=" << movingReference << std::endl;
    check(std::abs(cubic - movingReference) <
              std::abs(bilinear - movingReference) * .5f,
          "Catmull-Rom did not improve moving detail reference error");
    // Outer cubic taps cross a different surface; only a matching bilinear
    // center contribution is allowed, without mixing the adjacent black texel.
    taa.reset();
    params.depthRow = {.5f, 0, 0, 1};
    frame(1);
    params.depthRow = {0, 0, 0, 1};
    float edge = frame(0, {-.0625f, 0, 1, 1.0625f});
    check(std::abs(edge - .88125f) < .003f,
          "History footprint blended across a depth boundary");
    // No previous correspondence is different from an ineligible composite.
    taa.reset();
    check(frame(1, {0, 0, -1, 0}) == 1, "No-previous frame read history");
    check(std::abs(frame(0) - .9f) < .002f,
          "Opaque first frame did not seed eligible history");
    // A contradicted radiance sample must react faster than normal
    // accumulation.
    taa.reset();
    frame(0, {0, 0, 1, 1}, true, 4);
    float response = frame(0, {0, 0, 1, 1}, true, -5);
    check(response < .46f && response >= .39f,
          "Changed shading retained contradicted bright history");
    // Reconstruction must retain HDR energy even off pixel centers.
    taa.reset();
    frame(0, {0, 0, 1, 1}, true, 4);
    check(std::abs(frame(0, {-.04f, .03f, 1, 1}, true, 4) - 4) < .005f,
          "Cubic reconstruction changed a constant HDR field");
    params.jitterWeight.w = 0;
    if (d.timelineSemaphoreSupported()) {
      vk::SemaphoreTypeCreateInfo type{.semaphoreType =
                                           vk::SemaphoreType::eTimeline};
      vk::raii::Semaphore gate(d.logicalDevice(),
                               vk::SemaphoreCreateInfo{.pNext = &type});
      std::uint64_t value = 1;
      vk::Semaphore raw = *gate;
      vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands;
      vk::TimelineSemaphoreSubmitInfo timeline{.waitSemaphoreValueCount = 1,
                                               .pWaitSemaphoreValues = &value};
      d.graphicsQueue().submit({vk::SubmitInfo{.pNext = &timeline,
                                               .waitSemaphoreCount = 1,
                                               .pWaitSemaphores = &raw,
                                               .pWaitDstStageMask = &stage}},
                               nullptr);
      bool released = false;
      auto release = [&] {
        if (!released) {
          d.logicalDevice().signalSemaphore({.semaphore = raw, .value = 1});
          released = true;
        }
        d.logicalDevice().waitIdle();
      };
      try {
        taa.reset();
        deferred = true;
        frame(0);
        frame(1);
        check(fenceA.getStatus() == vk::Result::eNotReady &&
                  fenceB.getStatus() == vk::Result::eNotReady,
              "TAA gate did not hold both submissions");
        release();
        deferred = false;
        std::array<std::uint16_t, 4> a{}, b{};
        downloadA.read(std::as_writable_bytes(std::span{a}));
        downloadB.read(std::as_writable_bytes(std::span{b}));
        check(
            glm::unpackHalf1x16(a[0]) == 0 &&
                std::abs(glm::unpackHalf1x16(b[0]) - .1f) < .001f,
            "Two pending TAA histories aliased or parity advanced incorrectly");
      } catch (...) {
        release();
        throw;
      }
      std::cout
          << "PASS two timeline-blocked TAA submissions: independent "
             "staging/push snapshots, parity and RAW/WAR history, HDR0/.1\n";
    }
    std::cout << "PASS TAA: first frame, ping pong .1/.09, 64-frame "
                 "convergence, invalid/bounds/depth/transparent rejection, "
                 "jitter compensation, HDR clamp, reset\n";
  }
  check(d.resourceLedger().snapshot().current == ResourceLedger::Footprint{},
        "TAA fixture leaked resources");
}
