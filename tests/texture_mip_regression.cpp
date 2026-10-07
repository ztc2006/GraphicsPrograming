#include "texture_mip_regression.hpp"
#include "texture_cache.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, char const *message) {
  if (!ok)
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
          "Mip regression GPU submission did not finish");
}
void barrier(vk::CommandBuffer command, vk::Image image, vk::ImageLayout before,
             vk::ImageLayout after, vk::PipelineStageFlags2 dstStage,
             vk::AccessFlags2 dstAccess,
             vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor,
             unsigned mip = 0) {
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
      .subresourceRange = {aspect, mip, 1, 0, 1}};
  command.pipelineBarrier2(vk::DependencyInfo{
      .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &imageBarrier});
}
std::vector<std::uint32_t> shader(char const *path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  auto size = file ? file.tellg() : std::streampos{-1};
  if (size <= 0 || std::size_t(size) % 4)
    throw std::runtime_error(std::string("Invalid display shader: ") + path);
  std::vector<std::uint32_t> code(std::size_t(size) / 4);
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(code.data()), size))
    throw std::runtime_error(std::string("Incomplete display shader: ") + path);
  return code;
}
vk::raii::Pipeline probePipeline(Device const &device,
                                 vk::PipelineLayout layout) {
  auto displayFormat = vk::Format::eR32G32B32A32Sfloat;
  auto vertex = shader("shaders/display.vert.spv"),
       fragment = shader("shaders/texture_mip_probe.frag.spv");
  vk::raii::ShaderModule vs(
      device.logicalDevice(),
      vk::ShaderModuleCreateInfo{.codeSize = vertex.size() * 4,
                                 .pCode = vertex.data()});
  vk::raii::ShaderModule fs(
      device.logicalDevice(),
      vk::ShaderModuleCreateInfo{.codeSize = fragment.size() * 4,
                                 .pCode = fragment.data()});
  std::array stages{vk::PipelineShaderStageCreateInfo{
                        .stage = vk::ShaderStageFlagBits::eVertex,
                        .module = *vs,
                        .pName = "main"},
                    vk::PipelineShaderStageCreateInfo{
                        .stage = vk::ShaderStageFlagBits::eFragment,
                        .module = *fs,
                        .pName = "main"}};
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vk::PipelineInputAssemblyStateCreateInfo assembly{
      .topology = vk::PrimitiveTopology::eTriangleList};
  vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1,
                                               .scissorCount = 1};
  vk::PipelineRasterizationStateCreateInfo raster{
      .polygonMode = vk::PolygonMode::eFill,
      .cullMode = vk::CullModeFlagBits::eNone,
      .frontFace = vk::FrontFace::eCounterClockwise,
      .lineWidth = 1};
  vk::PipelineMultisampleStateCreateInfo samples{
      .rasterizationSamples = vk::SampleCountFlagBits::e1};
  vk::PipelineColorBlendAttachmentState attachment{
      .colorWriteMask =
          vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
  vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1,
                                              .pAttachments = &attachment};
  std::array dynamic{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
  vk::PipelineDynamicStateCreateInfo dynamicState{
      .dynamicStateCount = dynamic.size(), .pDynamicStates = dynamic.data()};
  vk::PipelineRenderingCreateInfo rendering{
      .colorAttachmentCount = 1, .pColorAttachmentFormats = &displayFormat};
  vk::GraphicsPipelineCreateInfo info{.pNext = &rendering,
                                      .stageCount = stages.size(),
                                      .pStages = stages.data(),
                                      .pVertexInputState = &vertexInput,
                                      .pInputAssemblyState = &assembly,
                                      .pViewportState = &viewport,
                                      .pRasterizationState = &raster,
                                      .pMultisampleState = &samples,
                                      .pColorBlendState = &blend,
                                      .pDynamicState = &dynamicState,
                                      .layout = layout};
  return vk::raii::Pipeline(device.logicalDevice(), nullptr, info);
}

void hostReadBarrier(vk::CommandBuffer command, vk::Buffer buffer,
                     std::size_t bytes) {
  vk::BufferMemoryBarrier2 host{
      .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
      .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
      .dstStageMask = vk::PipelineStageFlagBits2::eHost,
      .dstAccessMask = vk::AccessFlagBits2::eHostRead,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = buffer,
      .size = bytes};
  command.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1,
                                              .pBufferMemoryBarriers = &host});
}
std::vector<std::byte> readLevel(Device const &device,
                                 TextureResources const &texture,
                                 TextureMipLevel level, unsigned mip) {
  auto readback =
      device.createBuffer(level.size, vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible);
  record(device, [&](vk::CommandBuffer command) {
    barrier(command, texture.image(), vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::ImageLayout::eTransferSrcOptimal,
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferRead, vk::ImageAspectFlagBits::eColor,
            mip);
    command.copyImageToBuffer(
        texture.image(), vk::ImageLayout::eTransferSrcOptimal, *readback.buffer,
        {vk::BufferImageCopy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 1},
            .imageExtent = {level.width, level.height, 1}}});
    barrier(command, texture.image(), vk::ImageLayout::eTransferSrcOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eShaderSampledRead,
            vk::ImageAspectFlagBits::eColor, mip);
    hostReadBarrier(command, *readback.buffer, level.size);
  });
  std::vector<std::byte> result(level.size);
  readback.read(result);
  return result;
}
struct Probe {
  Device const &device;
  GpuImage output;
  vk::raii::ImageView view = nullptr;
  vk::raii::DescriptorSetLayout descriptorLayout = nullptr;
  vk::raii::DescriptorPool pool = nullptr;
  vk::raii::PipelineLayout layout = nullptr;
  vk::raii::Pipeline pipeline = nullptr;
  vk::DescriptorSet set;
  struct Push {
    float u, v, lod;
  };
  explicit Probe(Device const &d) : device(d) {
    output = device.createImage(
        vk::ImageCreateInfo{.imageType = vk::ImageType::e2D,
                            .format = vk::Format::eR32G32B32A32Sfloat,
                            .extent = {1, 1, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = vk::SampleCountFlagBits::e1,
                            .tiling = vk::ImageTiling::eOptimal,
                            .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                     vk::ImageUsageFlagBits::eTransferSrc,
                            .sharingMode = vk::SharingMode::eExclusive},
        16, vk::MemoryPropertyFlagBits::eDeviceLocal);
    view = vk::raii::ImageView(
        device.logicalDevice(),
        vk::ImageViewCreateInfo{
            .image = *output.image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
    vk::DescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment};
    descriptorLayout = vk::raii::DescriptorSetLayout(
        device.logicalDevice(), vk::DescriptorSetLayoutCreateInfo{
                                    .bindingCount = 1, .pBindings = &binding});
    vk::DescriptorPoolSize size{vk::DescriptorType::eCombinedImageSampler, 1};
    pool = vk::raii::DescriptorPool(
        device.logicalDevice(),
        vk::DescriptorPoolCreateInfo{
            .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size});
    auto rawLayout = *descriptorLayout;
    set = (*device.logicalDevice())
              .allocateDescriptorSets(
                  vk::DescriptorSetAllocateInfo{.descriptorPool = *pool,
                                                .descriptorSetCount = 1,
                                                .pSetLayouts = &rawLayout})
              .front();
    vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                               .size = sizeof(Push)};
    layout = vk::raii::PipelineLayout(
        device.logicalDevice(),
        vk::PipelineLayoutCreateInfo{.setLayoutCount = 1,
                                     .pSetLayouts = &rawLayout,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push});
    pipeline = probePipeline(device, *layout);
  }
  std::array<float, 4> sample(TextureResources const &texture, Push push) {
    vk::DescriptorImageInfo image{.sampler = texture.sampler(),
                                  .imageView = texture.imageView(),
                                  .imageLayout =
                                      vk::ImageLayout::eShaderReadOnlyOptimal};
    device.logicalDevice().updateDescriptorSets(
        {vk::WriteDescriptorSet{.dstSet = set,
                                .descriptorCount = 1,
                                .descriptorType =
                                    vk::DescriptorType::eCombinedImageSampler,
                                .pImageInfo = &image}},
        {});
    auto readback =
        device.createBuffer(16, vk::BufferUsageFlagBits::eTransferDst,
                            vk::MemoryPropertyFlagBits::eHostVisible);
    record(device, [&](vk::CommandBuffer command) {
      barrier(command, *output.image, vk::ImageLayout::eUndefined,
              vk::ImageLayout::eColorAttachmentOptimal,
              vk::PipelineStageFlagBits2::eColorAttachmentOutput,
              vk::AccessFlagBits2::eColorAttachmentWrite);
      vk::RenderingAttachmentInfo attachment{
          .imageView = *view,
          .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
          .loadOp = vk::AttachmentLoadOp::eDontCare,
          .storeOp = vk::AttachmentStoreOp::eStore};
      command.beginRendering(
          vk::RenderingInfo{.renderArea = {{0, 0}, {1, 1}},
                            .layerCount = 1,
                            .colorAttachmentCount = 1,
                            .pColorAttachments = &attachment});
      command.setViewport(0, {vk::Viewport{0, 0, 1, 1, 0, 1}});
      command.setScissor(0, {vk::Rect2D{{0, 0}, {1, 1}}});
      command.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
      command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0,
                                 {set}, {});
      command.pushConstants<Push>(*layout, vk::ShaderStageFlagBits::eFragment,
                                  0, push);
      command.draw(3, 1, 0, 0);
      command.endRendering();
      barrier(command, *output.image, vk::ImageLayout::eColorAttachmentOptimal,
              vk::ImageLayout::eTransferSrcOptimal,
              vk::PipelineStageFlagBits2::eTransfer,
              vk::AccessFlagBits2::eTransferRead);
      command.copyImageToBuffer(
          *output.image, vk::ImageLayout::eTransferSrcOptimal, *readback.buffer,
          {vk::BufferImageCopy{
              .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
              .imageExtent = {1, 1, 1}}});
      hostReadBarrier(command, *readback.buffer, 16);
    });
    std::array<float, 4> result;
    readback.read(std::as_writable_bytes(std::span(result)));
    return result;
  }
};
void close(float actual, float expected, char const *message) {
  if (std::abs(actual - expected) > .012f) {
    std::cerr << message << " actual=" << actual << " expected=" << expected
              << '\n';
    throw std::runtime_error(message);
  }
}
std::string ppm(unsigned width, unsigned height,
                std::span<unsigned char const> rgb) {
  return "P6\n" + std::to_string(width) + " " + std::to_string(height) +
         "\n255\n" +
         std::string(reinterpret_cast<char const *>(rgb.data()), rgb.size());
}
} // namespace
void exerciseTextureMips(Device const &device) {
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "Mip regression baseline not empty");
  {
    TextureCache cache(device);
    std::array<unsigned char, 12> rgb{0,   0,   0,   255, 255, 255,
                                      255, 255, 255, 0,   0,   0};
    auto source = ppm(2, 2, rgb);
    auto bytes = std::as_bytes(std::span(source));
    TextureSamplerDescription nearest{.mag = TextureFilter::Nearest,
                                      .min = TextureFilter::Nearest,
                                      .mip = TextureMipFilter::Nearest,
                                      .maxAnisotropy = 1};
    TextureSamplerDescription trilinear{.maxAnisotropy = 1};
    TextureSamplerDescription nonmip{.mag = TextureFilter::Nearest,
                                     .min = TextureFilter::Linear,
                                     .mip = TextureMipFilter::None};
    UploadBatch upload(device);
    auto linear = cache.encoded(bytes, "checker data",
                                TextureColorSpace::Linear, nearest, upload);
    auto color = cache.encoded(bytes, "checker color", TextureColorSpace::Srgb,
                               trilinear, upload);
    auto tri = cache.encoded(bytes, "checker tri", TextureColorSpace::Linear,
                             trilinear, upload);
    auto plain = cache.encoded(bytes, "checker plain",
                               TextureColorSpace::Linear, nonmip, upload);
    auto base = cache.encoded(bytes, "mask base", TextureColorSpace::Srgb,
                              trilinear, upload, TextureMipPolicy::BaseOnly);
    auto normal =
        cache.encoded(bytes, "same bytes, normal", TextureColorSpace::Linear,
                      nearest, upload, TextureMipPolicy::Normal);
    auto aniso = cache.encoded(bytes, "checker anisotropy",
                               TextureColorSpace::Linear, {}, upload);
    require(linear.image() == tri.image() && tri.image() == plain.image() &&
                tri.image() == aniso.image() &&
                linear.sampler() != tri.sampler() &&
                plain.sampler() != linear.sampler(),
            "Sampler state duplicated image or was lost");
    require(base.image() != color.image() && normal.image() != linear.image() &&
                base.mipLevels() == 1 && linear.mipLevels() == 2,
            "Typed mip policy missing from cache/view identity");
    close(aniso.maxAnisotropy(), std::min(8.f, device.maxSamplerAnisotropy()),
          "Anisotropy feature/limit clamp");
    require(linear.maxAnisotropy() == 1 && plain.maxAnisotropy() == 1,
            "Anisotropy changed nearest/non-mip semantics");
    auto stats = upload.finish();
    require(stats.imageCopies == 4 && stats.submissions == 1 &&
                stats.fenceWaits == 1,
            "Typed chains should upload four images, one batch");
    auto shared = device.resourceLedger().snapshot().at(
        ResourceLedger::Domain::SharedTextures);
    require(
        shared.images == 4 && shared.imageViews == 4 &&
            shared.payloadBytes == 76,
        "Texture ledger did not account for complete mip payload exactly once");
    std::array<unsigned char, 16> rgba{0,   0,   0,   255, 255, 255, 255, 255,
                                       255, 255, 255, 255, 0,   0,   0,   255};
    for (auto pair : {std::pair{linear, TextureColorSpace::Linear},
                      std::pair{color, TextureColorSpace::Srgb},
                      std::pair{normal, TextureColorSpace::Linear}}) {
      auto expected = generateTextureMips(
          std::as_bytes(std::span(rgba)), 2, 2, pair.second,
          pair.first.image() == normal.image() ? TextureMipPolicy::Normal
                                               : TextureMipPolicy::Average);
      for (unsigned mip = 0; mip < expected.levels.size(); ++mip) {
        auto level = expected.levels[mip];
        auto read = readLevel(device, pair.first, level, mip);
        require(std::equal(read.begin(), read.end(),
                           expected.pixels.begin() + level.offset),
                "Full-chain GPU copy differs from typed pixels");
      }
    }
    // NPOT copy offsets and all odd edge texels must survive actual GPU upload.
    std::array<unsigned char, 45> oddRgb{};
    for (unsigned y = 0; y < 5; ++y)
      for (unsigned c = 0; c < 3; ++c)
        oddRgb[(y * 3 + 2) * 3 + c] = 255;
    auto oddSource = ppm(3, 5, oddRgb);
    UploadBatch oddUpload(device);
    auto odd = cache.encoded(std::as_bytes(std::span(oddSource)), "odd edge",
                             TextureColorSpace::Linear, nearest, oddUpload);
    oddUpload.finish();
    std::array<unsigned char, 60> oddRgba{};
    for (unsigned i = 0; i < 15; ++i) {
      for (unsigned c = 0; c < 3; ++c)
        oddRgba[i * 4 + c] = oddRgb[i * 3 + c];
      oddRgba[i * 4 + 3] = 255;
    }
    auto oddChain = generateTextureMips(std::as_bytes(std::span(oddRgba)), 3, 5,
                                        TextureColorSpace::Linear);
    for (unsigned mip = 0; mip < oddChain.levels.size(); ++mip) {
      auto level = oddChain.levels[mip];
      auto read = readLevel(device, odd, level, mip);
      require(std::equal(read.begin(), read.end(),
                         oddChain.pixels.begin() + level.offset),
              "NPOT GPU copy dropped/misaligned mip texels");
    }
    Probe probe(device);
    // Independent normal-alpha oracle: black/white RGB decode to opposed
    // unit vectors. LOD1 is exactly cancelling, with +Z fallback and loss=1.
    close(probe.sample(normal, {.25f, .25f, 0})[3], 0,
          "Normal LOD0 variance must be zero");
    close(probe.sample(normal, {.25f, .25f, 1})[3], 1,
          "Normal opposing mean length loss");
    {
      UploadBatch batch(device);
      auto normalTri =
          cache.encoded(bytes, "normal trilinear", TextureColorSpace::Linear,
                        trilinear, batch, TextureMipPolicy::Normal);
      batch.finish();
      require(normalTri.image() == normal.image(),
              "Normal sampler duplicated variance image");
      close(probe.sample(normalTri, {.25f, .25f, .5f})[3], .5f,
            "Normal loss trilinear interpolation");
      std::array<unsigned char, 192> checker{}, stripes{};
      for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
          auto i = (y * 8 + x) * 3;
          checker[i] = x % 2 ? 51 : 204;
          stripes[i] = x < 4 ? 51 : 204;
          checker[i + 1] = stripes[i + 1] = 128;
          checker[i + 2] = stripes[i + 2] = 230;
        }
      auto a = ppm(8, 8, checker), b = ppm(8, 8, stripes);
      UploadBatch frequencyUpload(device);
      auto high = cache.encoded(std::as_bytes(std::span(a)), "high frequency",
                                TextureColorSpace::Linear, nearest,
                                frequencyUpload, TextureMipPolicy::Normal);
      auto low = cache.encoded(std::as_bytes(std::span(b)), "low frequency",
                               TextureColorSpace::Linear, nearest,
                               frequencyUpload, TextureMipPolicy::Normal);
      frequencyUpload.finish();
      double nx = 153. / 255, ny = 1. / 255, nz = 205. / 255;
      double length =
          std::sqrt((ny * ny + nz * nz) / (nx * nx + ny * ny + nz * nz));
      float loss = float(std::round((1 - length) * 255) / 255);
      for (unsigned level = 1; level <= 3; ++level) {
        close(probe.sample(high, {.0625f, .0625f, float(level)})[3], loss,
              "High-frequency normal variance mip");
        close(probe.sample(low, {.0625f, .0625f, float(level)})[3],
              level == 3 ? loss : 0, "Normal frequency footprint selection");
      }
    }
    close(probe.sample(linear, {.25f, .25f, 8})[0], 128.f / 255,
          "LOD/view still clamped to base");
    close(probe.sample(color, {.25f, .25f, 8})[0], .5f,
          "sRGB mip sampling energy");
    close(probe.sample(tri, {.25f, .25f, .5f})[0], .5f * 128 / 255,
          "Trilinear mip interpolation");
    close(probe.sample(linear, {.25f, .25f, .4f})[0], 0, "Nearest mip level 0");
    close(probe.sample(linear, {.25f, .25f, .6f})[0], 128.f / 255,
          "Nearest mip level 1");
    close(probe.sample(plain, {.4f, .25f, 8})[0], .3f,
          "Non-mip minFilter lost to magFilter");
    close(probe.sample(plain, {.4f, .25f, 0})[0], 0,
          "Non-mip magFilter ignored");
    close(probe.sample(base, {.25f, .25f, 8})[0], 0,
          "Mask BaseOnly acquired averaged alpha/color mip");
    for (auto min : {TextureFilter::Nearest, TextureFilter::Linear}) {
      for (auto mip : {TextureMipFilter::None, TextureMipFilter::Nearest,
                       TextureMipFilter::Linear}) {
        TextureSamplerDescription s{.mag = TextureFilter::Nearest,
                                    .min = min,
                                    .mip = mip,
                                    .maxAnisotropy = 1};
        UploadBatch binding(device);
        auto filtered = cache.encoded(bytes, "six min modes",
                                      TextureColorSpace::Linear, s, binding);
        binding.finish();
        float baseValue = min == TextureFilter::Linear ? .3f : 0.f;
        for (float lod : {.4f, .6f}) {
          float expected = mip == TextureMipFilter::None ? baseValue
                           : mip == TextureMipFilter::Nearest
                               ? (lod < .5f ? baseValue : 128.f / 255)
                               : baseValue * (1 - lod) + (128.f / 255) * lod;
          close(probe.sample(filtered, {.4f, .25f, lod})[0], expected,
                "Six glTF minification modes");
        }
        close(probe.sample(filtered, {.4f, .25f, 0})[0], 0,
              "magFilter for six minification modes");
      }
    }
    {
      TextureSamplerDescription limit{.maxAnisotropy = 255};
      UploadBatch binding(device);
      auto limited = cache.encoded(bytes, "device anisotropy limit",
                                   TextureColorSpace::Linear, limit, binding);
      binding.finish();
      close(limited.maxAnisotropy(), device.maxSamplerAnisotropy(),
            "Anisotropy exceeds enabled device limit");
      require(limited.image() == linear.image(),
              "Anisotropy request duplicated texture storage");
    }
    for (auto wrap : {TextureWrap::Repeat, TextureWrap::ClampToEdge,
                      TextureWrap::MirroredRepeat}) {
      auto s = nearest;
      s.mip = TextureMipFilter::None;
      s.u = wrap;
      UploadBatch binding(device);
      auto wrapped =
          cache.encoded(bytes, "wrap", TextureColorSpace::Linear, s, binding);
      binding.finish();
      close(probe.sample(wrapped, {1.25f, .25f, 0})[0],
            wrap == TextureWrap::Repeat ? 0.f : 1.f, "Wrap mapping");
    }
    {
      std::array<unsigned char, 192> maskRgb{};
      for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
          unsigned block = (y / 2) * 4 + x / 2;
          maskRgb[(y * 8 + x) * 3] = x % 2 ? block * 2 : 160 + block;
        }
      auto encoded = ppm(8, 8, maskRgb);
      auto maskBytes = std::as_bytes(std::span(encoded));
      UploadBatch coverageUpload(device);
      auto coverage = cache.encoded(
          maskBytes, "opacity R", TextureColorSpace::Linear, nearest,
          coverageUpload, TextureMipPolicy::AlphaCoverage,
          {.channel = TextureAlphaChannel::Red});
      auto repeated = cache.encoded(
          maskBytes, "same opacity R", TextureColorSpace::Linear, nearest,
          coverageUpload, TextureMipPolicy::AlphaCoverage,
          {.channel = TextureAlphaChannel::Red});
      auto changed = cache.encoded(
          maskBytes, "new threshold", TextureColorSpace::Linear, nearest,
          coverageUpload, TextureMipPolicy::AlphaCoverage,
          {.cutoff = .4f, .channel = TextureAlphaChannel::Red});
      auto alpha = cache.encoded(
          maskBytes, "alpha channel", TextureColorSpace::Linear, nearest,
          coverageUpload, TextureMipPolicy::AlphaCoverage);
      require(coverage.image() == repeated.image() &&
                  coverage.image() != changed.image() &&
                  coverage.image() != alpha.image(),
              "Coverage threshold/channel cache identity lost");
      auto beforeView = device.resourceLedger().snapshot().at(
          ResourceLedger::Domain::SharedTextures);
      auto source = cache.baseLevelBinding(coverage, nearest);
      auto sourceAgain = cache.baseLevelBinding(coverage, nearest);
      auto afterView = device.resourceLedger().snapshot().at(
          ResourceLedger::Domain::SharedTextures);
      require(
          source.image() == coverage.image() &&
              source.imageView() == sourceAgain.imageView() &&
              source.imageView() != coverage.imageView() &&
              source.mipLevels() == 1 && coverage.mipLevels() == 4 &&
              afterView.images == beforeView.images &&
              afterView.payloadBytes == beforeView.payloadBytes &&
              afterView.imageViews == beforeView.imageViews + 1,
          "Base-level fallback duplicated storage or failed to reuse its view");
      require(coverageUpload.finish().imageCopies == 3,
              "Coverage binding duplicated image copies");
      unsigned passing = 0;
      for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x)
          passing += probe.sample(coverage,
                                  {(x + .5f) / 4, (y + .5f) / 4, 1})[0] >= .5f;
      require(passing == 8, "Actual sampled coverage mip lost 8/16 target");
      close(probe.sample(source, {.125f, .125f, 8})[0], 0,
            "Base view failed to clamp LOD to original opacity");
      close(probe.sample(coverage, {.125f, .125f, 0})[3], 1,
            "Opacity alpha metadata or GPU channel contaminated");
      std::cout
          << "PASS GPU coverage cache: cutoff/channel identities, 8/16 sampled "
             "coverage, source view shares storage, no extra copies\n";
    }
    std::cout << "PASS GPU typed mips: full/NPOT chain readback, independent "
                 "image policies/samplers, normal variance/frequency/cache, "
                 "explicit LOD/trilinear/non-mip "
                 "min-mag/wrap, anisotropy limit="
              << aniso.maxAnisotropy() << '\n';
  }
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "Typed mip/upload/probe resources leaked");
}
