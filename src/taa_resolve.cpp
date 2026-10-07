#include "taa_resolve.hpp"
#include <fstream>
#include <stdexcept>
namespace {
std::vector<std::uint32_t> shader(char const *p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  auto n = f ? f.tellg() : std::streampos(-1);
  if (n <= 0 || std::size_t(n) % 4)
    throw std::runtime_error("Invalid TAA shader");
  std::vector<std::uint32_t> s(std::size_t(n) / 4);
  f.seekg(0);
  if (!f.read(reinterpret_cast<char *>(s.data()), n))
    throw std::runtime_error("Incomplete TAA shader");
  return s;
}
} // namespace
TaaResolve::TaaResolve(Device const &d, vk::Extent2D e, vk::ImageView color,
                       vk::ImageView depth, vk::ImageView motion)
    : device_(d), extent_(e) {
  auto scope = d.resourceLedger().scope(ResourceLedger::Domain::Persistent);
  auto target = [&](GpuImage &image, vk::raii::ImageView &view,
                    vk::Format format, unsigned bytes) {
    auto required = vk::FormatFeatureFlagBits::eColorAttachment |
                    vk::FormatFeatureFlagBits::eSampledImage |
                    vk::FormatFeatureFlagBits::eTransferSrc;
    if (format == colorFormat)
      required |= vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
    if ((d.physicalDevice().getFormatProperties(format).optimalTilingFeatures &
         required) != required)
      throw std::runtime_error("TAA target format unsupported");
    image = d.createImage({.imageType = vk::ImageType::e2D,
                           .format = format,
                           .extent = {e.width, e.height, 1},
                           .mipLevels = 1,
                           .arrayLayers = 1,
                           .samples = vk::SampleCountFlagBits::e1,
                           .tiling = vk::ImageTiling::eOptimal,
                           .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                    vk::ImageUsageFlagBits::eSampled |
                                    vk::ImageUsageFlagBits::eTransferSrc,
                           .sharingMode = vk::SharingMode::eExclusive},
                          std::uint64_t(e.width) * e.height * bytes,
                          vk::MemoryPropertyFlagBits::eDeviceLocal, scope);
    view = vk::raii::ImageView(
        d.logicalDevice(),
        {.image = *image.image,
         .viewType = vk::ImageViewType::e2D,
         .format = format,
         .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
  };
  for (auto &t : targets_) {
    target(t.color, t.colorView, colorFormat, 8);
    target(t.depth, t.depthView, depthFormat, 4);
  }
  auto sampler = [&](vk::Filter filter) {
    return vk::raii::Sampler(
        d.logicalDevice(),
        {.magFilter = filter,
         .minFilter = filter,
         .mipmapMode = vk::SamplerMipmapMode::eNearest,
         .addressModeU = vk::SamplerAddressMode::eClampToEdge,
         .addressModeV = vk::SamplerAddressMode::eClampToEdge,
         .addressModeW = vk::SamplerAddressMode::eClampToEdge,
         .maxLod = 0});
  };
  nearest_ = sampler(vk::Filter::eNearest);
  linear_ = sampler(vk::Filter::eLinear);
  std::array<vk::DescriptorSetLayoutBinding, 5> bindings;
  for (unsigned i = 0; i < 5; ++i)
    bindings[i] = {.binding = i,
                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                   .descriptorCount = 1,
                   .stageFlags = vk::ShaderStageFlagBits::eFragment};
  setLayout_ = vk::raii::DescriptorSetLayout(
      d.logicalDevice(), {.bindingCount = 5, .pBindings = bindings.data()});
  vk::DescriptorPoolSize size{vk::DescriptorType::eCombinedImageSampler, 10};
  pool_ = vk::raii::DescriptorPool(
      d.logicalDevice(),
      {.maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &size});
  std::array layouts{*setLayout_, *setLayout_};
  auto allocated = (*d.logicalDevice())
                       .allocateDescriptorSets({.descriptorPool = *pool_,
                                                .descriptorSetCount = 2,
                                                .pSetLayouts = layouts.data()});
  for (unsigned w = 0; w < 2; ++w) {
    sets_[w] = allocated[w];
    unsigned prev = 1 - w;
    std::array infos{
        vk::DescriptorImageInfo{*nearest_, color,
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{*nearest_, depth,
                                vk::ImageLayout::eDepthReadOnlyOptimal},
        vk::DescriptorImageInfo{*nearest_, motion,
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{*linear_, *targets_[prev].colorView,
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{*nearest_, *targets_[prev].depthView,
                                vk::ImageLayout::eShaderReadOnlyOptimal}};
    std::array<vk::WriteDescriptorSet, 5> writes;
    for (unsigned i = 0; i < 5; ++i)
      writes[i] = {.dstSet = sets_[w],
                   .dstBinding = i,
                   .descriptorCount = 1,
                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                   .pImageInfo = &infos[i]};
    d.logicalDevice().updateDescriptorSets(writes, {});
  }
  vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                             .size = sizeof(TaaPush)};
  auto set = *setLayout_;
  layout_ = vk::raii::PipelineLayout(d.logicalDevice(),
                                     {.setLayoutCount = 1,
                                      .pSetLayouts = &set,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &push});
  auto v = shader("shaders/display.vert.spv"),
       f = shader("shaders/taa.frag.spv");
  vk::raii::ShaderModule vs(d.logicalDevice(),
                            {.codeSize = v.size() * 4, .pCode = v.data()}),
      fs(d.logicalDevice(), {.codeSize = f.size() * 4, .pCode = f.data()});
  std::array stages{vk::PipelineShaderStageCreateInfo{
                        .stage = vk::ShaderStageFlagBits::eVertex,
                        .module = *vs,
                        .pName = "main"},
                    vk::PipelineShaderStageCreateInfo{
                        .stage = vk::ShaderStageFlagBits::eFragment,
                        .module = *fs,
                        .pName = "main"}};
  vk::PipelineVertexInputStateCreateInfo input{};
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
  vk::PipelineColorBlendAttachmentState blend{
      .colorWriteMask =
          vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
  std::array blends{blend, blend};
  vk::PipelineColorBlendStateCreateInfo blending{.attachmentCount = 2,
                                                 .pAttachments = blends.data()};
  std::array dynamics{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
  vk::PipelineDynamicStateCreateInfo dynamic{.dynamicStateCount = 2,
                                             .pDynamicStates = dynamics.data()};
  std::array formats{colorFormat, depthFormat};
  vk::PipelineRenderingCreateInfo rendering{
      .colorAttachmentCount = 2, .pColorAttachmentFormats = formats.data()};
  pipeline_ = vk::raii::Pipeline(
      d.logicalDevice(), nullptr,
      vk::GraphicsPipelineCreateInfo{.pNext = &rendering,
                                     .stageCount = 2,
                                     .pStages = stages.data(),
                                     .pVertexInputState = &input,
                                     .pInputAssemblyState = &assembly,
                                     .pViewportState = &viewport,
                                     .pRasterizationState = &raster,
                                     .pMultisampleState = &samples,
                                     .pColorBlendState = &blending,
                                     .pDynamicState = &dynamic,
                                     .layout = *layout_});
  accounting_ = scope.track({.imageViews = 4,
                             .samplers = 2,
                             .descriptorPools = 1,
                             .descriptorSets = 2});
  d.nameObject(*pipeline_, "TAA linear HDR resolve");
}
TaaResolve::Frame TaaResolve::addPass(RenderGraph &g,
                                      RenderGraph::ImageId color,
                                      RenderGraph::ImageId depth,
                                      RenderGraph::ImageId motion) const {
  using G = RenderGraph;
  Frame f;
  f.write = write_;
  auto usage = vk::ImageUsageFlagBits::eColorAttachment |
               vk::ImageUsageFlagBits::eSampled |
               vk::ImageUsageFlagBits::eTransferSrc;
  for (unsigned i = 0; i < 2; ++i) {
    f.color[i] = g.importImage(
        {"TAA color " + std::to_string(i), *targets_[i].color.image,
         *targets_[i].colorView, colorFormat, extent_,
         vk::ImageAspectFlagBits::eColor, usage, false, colorStates_[i]});
    f.depth[i] = g.importImage(
        {"TAA linear depth " + std::to_string(i), *targets_[i].depth.image,
         *targets_[i].depthView, depthFormat, extent_,
         vk::ImageAspectFlagBits::eColor, usage, false, depthStates_[i]});
  }
  unsigned p = 1 - write_;
  vk::ClearValue zero{
      .color = vk::ClearColorValue{std::array<float, 4>{0, 0, 0, 0}}};
  if (!colorStates_[p].defined)
    g.addPass(
        "Initialize TAA history",
        {{f.color[p], G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
          vk::AttachmentStoreOp::eStore, false, zero},
         {f.depth[p], G::Usage::ColorAttachment, vk::AttachmentLoadOp::eClear,
          vk::AttachmentStoreOp::eStore, false, zero}});
  f.resolve = g.addPass(
      "TAA resolve (HDR + reprojection + rejection + clamp)",
      {{color, G::Usage::SampledColor},
       {depth, G::Usage::SampledDepth},
       {motion, G::Usage::SampledColor},
       {f.color[p], G::Usage::SampledColor},
       {f.depth[p], G::Usage::SampledColor},
       {f.color[write_], G::Usage::ColorAttachment,
        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore, true},
       {f.depth[write_], G::Usage::ColorAttachment,
        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore, true}});
  return f;
}
void TaaResolve::draw(vk::CommandBuffer cmd, TaaPush p) const {
  p.options.x *= ready_ ? 1.f : 0.f;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_);
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0,
                         {sets_[write_]}, {});
  cmd.pushConstants<TaaPush>(*layout_, vk::ShaderStageFlagBits::eFragment, 0,
                             p);
  cmd.draw(3, 1, 0, 0);
}
void TaaResolve::submitted(RenderGraph::Plan const &p, Frame const &f) {
  for (unsigned i = 0; i < 2; ++i) {
    colorStates_[i] = p.finalState(f.color[i]);
    depthStates_[i] = p.finalState(f.depth[i]);
  }
  write_ = 1 - f.write;
  ready_ = true;
}
