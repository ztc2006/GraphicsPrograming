#include "gtao.hpp"
#include <cmath>
#include <fstream>
#include <stdexcept>
namespace {
std::vector<std::uint32_t> shader(char const *path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  auto n = f ? f.tellg() : std::streampos(-1);
  if (n <= 0 || std::size_t(n) % 4)
    throw std::runtime_error("Invalid GTAO shader");
  std::vector<std::uint32_t> s(std::size_t(n) / 4);
  f.seekg(0);
  if (!f.read(reinterpret_cast<char *>(s.data()), n))
    throw std::runtime_error("Incomplete GTAO shader");
  return s;
}
constexpr auto usage = vk::ImageUsageFlagBits::eColorAttachment |
                       vk::ImageUsageFlagBits::eSampled |
                       vk::ImageUsageFlagBits::eTransferSrc;
char const *names[]{"Indirect diffuse HDR", "GTAO raw", "GTAO filtered",
                    "AO composite HDR"};
} // namespace
void validateAoSettings(AoSettings const &s) {
  if (!std::isfinite(s.radius) || s.radius < .001f || s.radius > 100.f ||
      !std::isfinite(s.strength) || s.strength < 0 || s.strength > 2 ||
      unsigned(s.debug) > unsigned(AoDebug::Filtered))
    throw std::invalid_argument(
        "AO radius must be .001..100, strength 0..2, and debug valid");
}
Gtao::Gtao(Device const &d, vk::Extent2D e, vk::ImageView hdr,
           vk::ImageView depth)
    : device_(d), extent_(e) {
  if (!e.width || !e.height)
    throw std::invalid_argument("AO extent must be nonzero");
  auto scope = d.resourceLedger().scope(ResourceLedger::Domain::Persistent);
  for (unsigned i = 0; i < Count; ++i) {
    auto fmt = format(Target(i));
    auto required = vk::FormatFeatureFlagBits::eColorAttachment |
                    vk::FormatFeatureFlagBits::eSampledImage |
                    vk::FormatFeatureFlagBits::eTransferSrc;
    if (i == Diffuse)
      required |= vk::FormatFeatureFlagBits::eColorAttachmentBlend;
    if ((d.physicalDevice().getFormatProperties(fmt).optimalTilingFeatures &
         required) != required)
      throw std::runtime_error("AO target format unsupported");
    targets_[i].image = d.createImage(
        {.imageType = vk::ImageType::e2D,
         .format = fmt,
         .extent = {e.width, e.height, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = vk::SampleCountFlagBits::e1,
         .tiling = vk::ImageTiling::eOptimal,
         .usage = usage,
         .sharingMode = vk::SharingMode::eExclusive},
        std::uint64_t(e.width) * e.height * (i == Raw || i == Filtered ? 2 : 8),
        vk::MemoryPropertyFlagBits::eDeviceLocal, scope);
    targets_[i].view = vk::raii::ImageView(
        d.logicalDevice(),
        {.image = *targets_[i].image.image,
         .viewType = vk::ImageViewType::e2D,
         .format = fmt,
         .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
    d.nameObject(image(Target(i)), names[i]);
  }
  sampler_ = vk::raii::Sampler(
      d.logicalDevice(), {.magFilter = vk::Filter::eNearest,
                          .minFilter = vk::Filter::eNearest,
                          .mipmapMode = vk::SamplerMipmapMode::eNearest,
                          .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                          .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                          .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                          .maxLod = 0});
  std::array<vk::DescriptorSetLayoutBinding, 5> bindings;
  for (unsigned i = 0; i < 5; ++i)
    bindings[i] = {.binding = i,
                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                   .descriptorCount = 1,
                   .stageFlags = vk::ShaderStageFlagBits::eFragment};
  setLayout_ = vk::raii::DescriptorSetLayout(
      d.logicalDevice(), {.bindingCount = 5, .pBindings = bindings.data()});
  vk::DescriptorPoolSize size{vk::DescriptorType::eCombinedImageSampler, 5};
  pool_ = vk::raii::DescriptorPool(
      d.logicalDevice(),
      {.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size});
  auto sl = *setLayout_;
  set_ = (*d.logicalDevice())
             .allocateDescriptorSets({.descriptorPool = *pool_,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &sl})
             .front();
  std::array infos{
      vk::DescriptorImageInfo{*sampler_, depth,
                              vk::ImageLayout::eDepthReadOnlyOptimal},
      vk::DescriptorImageInfo{*sampler_, view(Raw),
                              vk::ImageLayout::eShaderReadOnlyOptimal},
      vk::DescriptorImageInfo{*sampler_, view(Filtered),
                              vk::ImageLayout::eShaderReadOnlyOptimal},
      vk::DescriptorImageInfo{*sampler_, hdr,
                              vk::ImageLayout::eShaderReadOnlyOptimal},
      vk::DescriptorImageInfo{*sampler_, view(Diffuse),
                              vk::ImageLayout::eShaderReadOnlyOptimal}};
  std::array<vk::WriteDescriptorSet, 5> writes;
  for (unsigned i = 0; i < 5; ++i)
    writes[i] = {.dstSet = set_,
                 .dstBinding = i,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                 .pImageInfo = &infos[i]};
  d.logicalDevice().updateDescriptorSets(writes, {});
  vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                             .size = sizeof(GtaoPush)};
  layout_ = vk::raii::PipelineLayout(d.logicalDevice(),
                                     {.setLayoutCount = 1,
                                      .pSetLayouts = &sl,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &push});
  auto vertex = shader("shaders/display.vert.spv");
  vk::raii::ShaderModule vs(d.logicalDevice(), {.codeSize = vertex.size() * 4,
                                                .pCode = vertex.data()});
  char const *files[]{"shaders/gtao.frag.spv", "shaders/gtao_filter.frag.spv",
                      "shaders/gtao_composite.frag.spv"};
  for (unsigned i = 0; i < 3; ++i) {
    auto code = shader(files[i]);
    vk::raii::ShaderModule fs(
        d.logicalDevice(), {.codeSize = code.size() * 4, .pCode = code.data()});
    std::array stages{vk::PipelineShaderStageCreateInfo{
                          .stage = vk::ShaderStageFlagBits::eVertex,
                          .module = *vs,
                          .pName = "main"},
                      vk::PipelineShaderStageCreateInfo{
                          .stage = vk::ShaderStageFlagBits::eFragment,
                          .module = *fs,
                          .pName = "main"}};
    vk::PipelineVertexInputStateCreateInfo vi{};
    vk::PipelineInputAssemblyStateCreateInfo ia{
        .topology = vk::PrimitiveTopology::eTriangleList};
    vk::PipelineViewportStateCreateInfo vp{.viewportCount = 1,
                                           .scissorCount = 1};
    vk::PipelineRasterizationStateCreateInfo rs{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .lineWidth = 1};
    vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples =
                                                  vk::SampleCountFlagBits::e1};
    vk::PipelineColorBlendAttachmentState ba{
        .colorWriteMask =
            vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    vk::PipelineColorBlendStateCreateInfo bs{.attachmentCount = 1,
                                             .pAttachments = &ba};
    std::array dyn{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo ds{.dynamicStateCount = 2,
                                          .pDynamicStates = dyn.data()};
    auto fmt = format(Target(i + 1));
    vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1,
                                              .pColorAttachmentFormats = &fmt};
    pipelines_[i] = vk::raii::Pipeline(
        d.logicalDevice(), nullptr,
        vk::GraphicsPipelineCreateInfo{.pNext = &rendering,
                                       .stageCount = 2,
                                       .pStages = stages.data(),
                                       .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia,
                                       .pViewportState = &vp,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pColorBlendState = &bs,
                                       .pDynamicState = &ds,
                                       .layout = *layout_});
    d.nameObject(*pipelines_[i], names[i + 1]);
  }
  accounting_ = scope.track({.imageViews = 4,
                             .samplers = 1,
                             .descriptorPools = 1,
                             .descriptorSets = 1});
}
RenderGraph::ImageId Gtao::importDiffuse(RenderGraph &g) const {
  return g.importImage(
      {names[Diffuse], image(Diffuse), view(Diffuse), format(Diffuse), extent_,
       vk::ImageAspectFlagBits::eColor, usage, false, states_[Diffuse]});
}
Gtao::Frame Gtao::addPasses(RenderGraph &g, RenderGraph::ImageId hdr,
                            RenderGraph::ImageId depth,
                            RenderGraph::ImageId diffuse) const {
  using G = RenderGraph;
  Frame f;
  f.images[Diffuse] = diffuse;
  for (unsigned i = Raw; i < Count; ++i)
    f.images[i] = g.importImage(
        {names[i], image(Target(i)), view(Target(i)), format(Target(i)),
         extent_, vk::ImageAspectFlagBits::eColor, usage, false, states_[i]});
  auto output = [&](Target i) {
    return G::Use{f.images[i], G::Usage::ColorAttachment,
                  vk::AttachmentLoadOp::eDontCare,
                  vk::AttachmentStoreOp::eStore, true};
  };
  f.horizon = g.addPass("GTAO horizons",
                        {{depth, G::Usage::SampledDepth}, output(Raw)});
  f.filter =
      g.addPass("GTAO spatial filter", {{depth, G::Usage::SampledDepth},
                                        {f.images[Raw], G::Usage::SampledColor},
                                        output(Filtered)});
  f.composite = g.addPass("AO indirect diffuse composite",
                          {{depth, G::Usage::SampledDepth},
                           {hdr, G::Usage::SampledColor},
                           {diffuse, G::Usage::SampledColor},
                           {f.images[Raw], G::Usage::SampledColor},
                           {f.images[Filtered], G::Usage::SampledColor},
                           output(Composite)});
  return f;
}
void Gtao::draw(vk::CommandBuffer c, unsigned pass, GtaoPush const &p) const {
  c.setViewport(0, {vk::Viewport{0, 0, float(extent_.width),
                                 float(extent_.height), 0, 1}});
  c.setScissor(0, {vk::Rect2D{{0, 0}, extent_}});
  c.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipelines_.at(pass));
  c.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, {set_},
                       {});
  c.pushConstants<GtaoPush>(*layout_, vk::ShaderStageFlagBits::eFragment, 0, p);
  c.draw(3, 1, 0, 0);
}
void Gtao::submitted(RenderGraph::Plan const &p, RenderGraph::ImageId diffuse,
                     Frame const *f) {
  states_[Diffuse] = p.finalState(diffuse);
  if (f)
    for (unsigned i = Raw; i < Count; ++i)
      states_[i] = p.finalState(f->images[i]);
}
