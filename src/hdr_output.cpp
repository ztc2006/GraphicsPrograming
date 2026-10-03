#include "hdr_output.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
struct DisplayPush {
  float exposure;
  std::uint32_t toneMap, encodeSrgb;
};
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
} // namespace
void validateDisplaySettings(DisplaySettings settings) {
  if (!std::isfinite(settings.exposureEv) || settings.exposureEv < -16 ||
      settings.exposureEv > 16)
    throw std::runtime_error(
        "Display exposure EV must be finite and in [-16,16]");
}
bool displayUsesHardwareSrgb(vk::Format format) {
  switch (format) {
  case vk::Format::eR8G8B8A8Srgb:
  case vk::Format::eB8G8R8A8Srgb:
    return true;
  case vk::Format::eR8G8B8A8Unorm:
  case vk::Format::eB8G8R8A8Unorm:
    return false;
  default:
    throw std::runtime_error("Unsupported SDR display format");
  }
}
HdrOutput::HdrOutput(Device const &device, vk::Extent2D extent,
                     vk::Format displayFormat)
    : device_(device), extent_(extent),
      hardwareSrgb_(displayUsesHardwareSrgb(displayFormat)) {
  if (!extent.width || !extent.height)
    throw std::runtime_error("HDR extent must be nonzero");
  auto required = vk::FormatFeatureFlagBits::eColorAttachment |
                  vk::FormatFeatureFlagBits::eColorAttachmentBlend |
                  vk::FormatFeatureFlagBits::eSampledImage |
                  vk::FormatFeatureFlagBits::eTransferSrc;
  if ((device.physicalDevice()
           .getFormatProperties(sceneFormat)
           .optimalTilingFeatures &
       required) != required)
    throw std::runtime_error(
        "RGBA16F HDR attachment/blending/sampling/readback is unsupported");
  auto scope =
      device.resourceLedger().scope(ResourceLedger::Domain::Persistent);
  scene_ = device.createImage(
      vk::ImageCreateInfo{.imageType = vk::ImageType::e2D,
                          .format = sceneFormat,
                          .extent = {extent.width, extent.height, 1},
                          .mipLevels = 1,
                          .arrayLayers = 1,
                          .samples = vk::SampleCountFlagBits::e1,
                          .tiling = vk::ImageTiling::eOptimal,
                          .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                   vk::ImageUsageFlagBits::eSampled |
                                   vk::ImageUsageFlagBits::eTransferSrc,
                          .sharingMode = vk::SharingMode::eExclusive},
      std::uint64_t(extent.width) * extent.height * 8,
      vk::MemoryPropertyFlagBits::eDeviceLocal, scope);
  view_ = vk::raii::ImageView(
      device.logicalDevice(),
      vk::ImageViewCreateInfo{
          .image = *scene_.image,
          .viewType = vk::ImageViewType::e2D,
          .format = sceneFormat,
          .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
  sampler_ = vk::raii::Sampler(
      device.logicalDevice(),
      vk::SamplerCreateInfo{
          .magFilter = vk::Filter::eNearest,
          .minFilter = vk::Filter::eNearest,
          .mipmapMode = vk::SamplerMipmapMode::eNearest,
          .addressModeU = vk::SamplerAddressMode::eClampToEdge,
          .addressModeV = vk::SamplerAddressMode::eClampToEdge,
          .addressModeW = vk::SamplerAddressMode::eClampToEdge,
          .maxLod = 0.0f});
  vk::DescriptorSetLayoutBinding binding{
      .binding = 0,
      .descriptorType = vk::DescriptorType::eCombinedImageSampler,
      .descriptorCount = 1,
      .stageFlags = vk::ShaderStageFlagBits::eFragment};
  descriptorLayout_ = vk::raii::DescriptorSetLayout(
      device.logicalDevice(), vk::DescriptorSetLayoutCreateInfo{
                                  .bindingCount = 1, .pBindings = &binding});
  vk::DescriptorPoolSize poolSize{vk::DescriptorType::eCombinedImageSampler, 1};
  descriptorPool_ = vk::raii::DescriptorPool(
      device.logicalDevice(),
      vk::DescriptorPoolCreateInfo{
          .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &poolSize});
  auto descriptorLayout = *descriptorLayout_;
  descriptor_ = (*device.logicalDevice())
                    .allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
                        .descriptorPool = *descriptorPool_,
                        .descriptorSetCount = 1,
                        .pSetLayouts = &descriptorLayout})
                    .front();
  vk::DescriptorImageInfo imageInfo{
      .sampler = *sampler_,
      .imageView = *view_,
      .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
  vk::WriteDescriptorSet write{.dstSet = descriptor_,
                               .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType =
                                   vk::DescriptorType::eCombinedImageSampler,
                               .pImageInfo = &imageInfo};
  device.logicalDevice().updateDescriptorSets({write}, {});
  vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                             .size = sizeof(DisplayPush)};
  pipelineLayout_ = vk::raii::PipelineLayout(
      device.logicalDevice(),
      vk::PipelineLayoutCreateInfo{.setLayoutCount = 1,
                                   .pSetLayouts = &descriptorLayout,
                                   .pushConstantRangeCount = 1,
                                   .pPushConstantRanges = &push});
  pipeline_ = createPipeline(displayFormat);
  accounting_ = scope.track({.imageViews = 1,
                             .samplers = 1,
                             .descriptorPools = 1,
                             .descriptorSets = 1});
  device.nameObject(*scene_.image, "Linear HDR scene RGBA16F");
  device.nameObject(*pipeline_, "Common display output");
}
vk::raii::Pipeline HdrOutput::createPipeline(vk::Format displayFormat) const {
  auto vertex = shader("shaders/display.vert.spv"),
       fragment = shader("shaders/display.frag.spv");
  vk::raii::ShaderModule vs(
      device_.logicalDevice(),
      vk::ShaderModuleCreateInfo{.codeSize = vertex.size() * 4,
                                 .pCode = vertex.data()});
  vk::raii::ShaderModule fs(
      device_.logicalDevice(),
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
                                      .layout = *pipelineLayout_};
  return vk::raii::Pipeline(device_.logicalDevice(), nullptr, info);
}
void HdrOutput::transition(vk::CommandBuffer command, vk::ImageLayout layout,
                           vk::PipelineStageFlags2 srcStage,
                           vk::AccessFlags2 srcAccess,
                           vk::PipelineStageFlags2 dstStage,
                           vk::AccessFlags2 dstAccess) {
  if (layout_ == vk::ImageLayout::eUndefined) {
    srcStage = {};
    srcAccess = {};
  }
  vk::ImageMemoryBarrier2 barrier{
      .srcStageMask = srcStage,
      .srcAccessMask = srcAccess,
      .dstStageMask = dstStage,
      .dstAccessMask = dstAccess,
      .oldLayout = layout_,
      .newLayout = layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = *scene_.image,
      .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
  command.pipelineBarrier2(vk::DependencyInfo{
      .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
  layout_ = layout;
}
void HdrOutput::prepareScene(vk::CommandBuffer command) {
  transition(command, vk::ImageLayout::eColorAttachmentOptimal,
             vk::PipelineStageFlagBits2::eFragmentShader,
             vk::AccessFlagBits2::eShaderSampledRead,
             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
             vk::AccessFlagBits2::eColorAttachmentWrite);
}
void HdrOutput::drawDisplay(vk::CommandBuffer command,
                            vk::ImageView destination,
                            DisplaySettings settings) {
  validateDisplaySettings(settings);
  transition(command, vk::ImageLayout::eShaderReadOnlyOptimal,
             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
             vk::AccessFlagBits2::eColorAttachmentWrite,
             vk::PipelineStageFlagBits2::eFragmentShader,
             vk::AccessFlagBits2::eShaderSampledRead);
  vk::RenderingAttachmentInfo attachment{
      .imageView = destination,
      .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
      .loadOp = vk::AttachmentLoadOp::eDontCare,
      .storeOp = vk::AttachmentStoreOp::eStore};
  vk::RenderingInfo rendering{.renderArea = {{0, 0}, extent_},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &attachment};
  command.beginRendering(rendering);
  command.setViewport(0, {vk::Viewport{0, 0, float(extent_.width),
                                       float(extent_.height), 0, 1}});
  command.setScissor(0, {vk::Rect2D{{0, 0}, extent_}});
  command.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_);
  command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipelineLayout_,
                             0, {descriptor_}, {});
  DisplayPush push{std::exp2(settings.exposureEv),
                   std::uint32_t(settings.toneMap),
                   std::uint32_t(!hardwareSrgb_)};
  command.pushConstants<DisplayPush>(
      *pipelineLayout_, vk::ShaderStageFlagBits::eFragment, 0, push);
  command.draw(3, 1, 0, 0);
}
