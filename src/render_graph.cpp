#include "render_graph.hpp"
#include <algorithm>
#include <optional>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
using G = RenderGraph;
bool attachment(G::Usage usage) {
  return usage == G::Usage::ColorAttachment || usage == G::Usage::DepthAttachment;
}
bool writes(vk::AccessFlags2 access) {
  return bool(access & (vk::AccessFlagBits2::eColorAttachmentWrite |
                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderStorageWrite |
                        vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eHostWrite |
                        vk::AccessFlagBits2::eMemoryWrite));
}
G::State desired(G::Usage usage) {
  using S = vk::PipelineStageFlagBits2;
  using A = vk::AccessFlagBits2;
  switch (usage) {
  case G::Usage::ColorAttachment:
    return {vk::ImageLayout::eColorAttachmentOptimal, S::eColorAttachmentOutput,
            A::eColorAttachmentRead | A::eColorAttachmentWrite};
  case G::Usage::DepthAttachment:
    return {vk::ImageLayout::eDepthAttachmentOptimal,
            S::eEarlyFragmentTests | S::eLateFragmentTests,
            A::eDepthStencilAttachmentRead | A::eDepthStencilAttachmentWrite};
  case G::Usage::SampledColor:
    return {vk::ImageLayout::eShaderReadOnlyOptimal, S::eFragmentShader, A::eShaderSampledRead};
  case G::Usage::SampledDepth:
    return {vk::ImageLayout::eDepthReadOnlyOptimal, S::eFragmentShader, A::eShaderSampledRead};
  case G::Usage::TransferSource:
    return {vk::ImageLayout::eTransferSrcOptimal, S::eCopy, A::eTransferRead};
  case G::Usage::RayTracingReadWrite:
    return {vk::ImageLayout::eGeneral, S::eRayTracingShaderKHR, A::eShaderStorageRead | A::eShaderStorageWrite};
  case G::Usage::RayTracingWrite:
    return {vk::ImageLayout::eGeneral, S::eRayTracingShaderKHR, A::eShaderStorageWrite};
  case G::Usage::Present:
    return {vk::ImageLayout::ePresentSrcKHR, {}, {}};
  }
  throw std::runtime_error("Unknown graph usage");
}
void validate(G::Image const &image, G::Usage usage) {
  vk::ImageUsageFlags required;
  auto aspect = vk::ImageAspectFlagBits::eColor;
  switch (usage) {
  case G::Usage::ColorAttachment: required = vk::ImageUsageFlagBits::eColorAttachment; break;
  case G::Usage::DepthAttachment:
    required = vk::ImageUsageFlagBits::eDepthStencilAttachment;
    aspect = vk::ImageAspectFlagBits::eDepth; break;
  case G::Usage::SampledDepth: aspect = vk::ImageAspectFlagBits::eDepth; [[fallthrough]];
  case G::Usage::SampledColor: required = vk::ImageUsageFlagBits::eSampled; break;
  case G::Usage::TransferSource:
    required = vk::ImageUsageFlagBits::eTransferSrc;
    aspect = image.aspect == vk::ImageAspectFlagBits::eDepth
                 ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
    break;
  case G::Usage::RayTracingReadWrite:
  case G::Usage::RayTracingWrite: required = vk::ImageUsageFlagBits::eStorage; break;
  case G::Usage::Present:
    required = vk::ImageUsageFlagBits::eColorAttachment;
    if (!image.presentable) throw std::runtime_error("Graph image is not presentable: " + image.name);
    break;
  }
  if (image.aspect != aspect || (image.usage & required) != required)
    throw std::runtime_error("Graph image usage/aspect mismatch: " + image.name);
  if (attachment(usage) && !image.view)
    throw std::runtime_error("Graph attachment requires a view: " + image.name);
}
void transition(G::Image const &image, G::State &state, G::Usage usage,
                std::vector<vk::ImageMemoryBarrier2> &barriers) {
  auto next = desired(usage);
  next.defined = state.defined;
  if (state.layout != next.layout || writes(state.access) || writes(next.access)) {
    barriers.push_back(vk::ImageMemoryBarrier2{
        .srcStageMask = state.stages, .srcAccessMask = state.access,
        .dstStageMask = next.stages, .dstAccessMask = next.access,
        .oldLayout = state.layout, .newLayout = next.layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = {image.aspect, 0, 1, 0, 1}});
  } else {
    next.stages |= state.stages;
    next.access |= state.access;
  }
  state = next;
}
void applyUse(G::Image const &image, G::State &state, G::Use const &use,
              std::vector<vk::ImageMemoryBarrier2> &barriers) {
  validate(image, use.usage);
  if (attachment(use.usage)) {
    if (use.load != vk::AttachmentLoadOp::eClear && use.load != vk::AttachmentLoadOp::eLoad &&
        use.load != vk::AttachmentLoadOp::eDontCare)
      throw std::runtime_error("Unsupported graph load operation: " + image.name);
    if (use.store != vk::AttachmentStoreOp::eStore && use.store != vk::AttachmentStoreOp::eDontCare)
      throw std::runtime_error("Unsupported graph store operation: " + image.name);
    if (use.load == vk::AttachmentLoadOp::eLoad && !state.defined)
      throw std::runtime_error("Graph LOAD of undefined contents: " + image.name);
  } else if (use.usage != G::Usage::RayTracingWrite && !state.defined) {
    throw std::runtime_error("Graph read of undefined contents: " + image.name);
  }
  transition(image, state, use.usage, barriers);
  if ((use.usage == G::Usage::RayTracingWrite || use.usage == G::Usage::RayTracingReadWrite))
    state.defined = use.fullOverwrite;
  if (attachment(use.usage))
    state.defined = use.store == vk::AttachmentStoreOp::eStore &&
                    (use.load == vk::AttachmentLoadOp::eClear ||
                     use.load == vk::AttachmentLoadOp::eLoad || use.fullOverwrite);
}
G::BufferState desired(G::BufferUsage usage) {
  using S = vk::PipelineStageFlagBits2;
  using A = vk::AccessFlagBits2;
  switch (usage) {
  case G::BufferUsage::ComputeRead: return {S::eComputeShader, A::eShaderStorageRead};
  case G::BufferUsage::ComputeWrite: return {S::eComputeShader, A::eShaderStorageWrite};
  case G::BufferUsage::VertexRead: return {S::eVertexShader, A::eShaderStorageRead};
  case G::BufferUsage::FragmentRead: return {S::eFragmentShader, A::eShaderStorageRead};
  }
  throw std::runtime_error("Unknown graph buffer usage");
}
void applyBuffer(G::Buffer const &buffer, G::BufferState &state, G::BufferUse use,
                 std::vector<vk::BufferMemoryBarrier2> &barriers) {
  if (!(buffer.usage & vk::BufferUsageFlagBits::eStorageBuffer))
    throw std::runtime_error("Graph buffer lacks storage usage: " + buffer.name);
  auto next = desired(use.usage);
  if (!writes(next.access) && !state.defined)
    throw std::runtime_error("Graph read of undefined buffer: " + buffer.name);
  if (use.fullOverwrite && !writes(next.access))
    throw std::runtime_error("Overwrite on read-only buffer use");
  next.defined = state.defined || (writes(next.access) && use.fullOverwrite);
  bool newReadStage = !writes(next.access) && bool(state.writerAccess) &&
      (state.stages & next.stages) != next.stages;
  if (writes(state.access) || writes(next.access) || newReadStage)
    barriers.push_back(vk::BufferMemoryBarrier2{
        .srcStageMask = state.stages | state.writerStages,
        .srcAccessMask = state.access | state.writerAccess,
        .dstStageMask = next.stages, .dstAccessMask = next.access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer.buffer, .offset = buffer.offset, .size = buffer.size});
  if (writes(next.access)) {
    next.writerStages = next.stages; next.writerAccess = next.access;
  } else {
    next.writerStages = writes(state.access) ? state.stages : state.writerStages;
    next.writerAccess = writes(state.access) ? state.access : state.writerAccess;
    if (!writes(state.access)) { next.stages |= state.stages; next.access |= state.access; }
  }
  state = next;
}
void emit(vk::CommandBuffer command, std::vector<vk::ImageMemoryBarrier2> const &barriers) {
  if (!barriers.empty())
    command.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = std::uint32_t(barriers.size()),
        .pImageMemoryBarriers = barriers.data()});
}
} // namespace

RenderGraph::ImageId RenderGraph::importImage(Image image) {
  if (image.name.empty() || !image.image || !image.extent.width || !image.extent.height ||
      image.format == vk::Format::eUndefined ||
      (image.aspect != vk::ImageAspectFlagBits::eColor && image.aspect != vk::ImageAspectFlagBits::eDepth))
    throw std::runtime_error("Invalid graph image description");
  if (image.initial.layout == vk::ImageLayout::eUndefined &&
      (image.initial.defined || image.initial.stages || image.initial.access))
    throw std::runtime_error("Undefined graph image has content/access state: " + image.name);
  for (auto const &existing : images_)
    if (existing.image == image.image || existing.name == image.name)
      throw std::runtime_error("Duplicate graph image/identity: " + image.name);
  ImageId id{std::uint32_t(images_.size())};
  images_.push_back(std::move(image));
  return id;
}
RenderGraph::BufferId RenderGraph::importBuffer(Buffer buffer) {
  if (buffer.name.empty() || !buffer.buffer || !buffer.size || buffer.size == VK_WHOLE_SIZE ||
      buffer.offset > std::numeric_limits<vk::DeviceSize>::max() - buffer.size)
    throw std::runtime_error("Invalid graph buffer description");
  for (auto const &existing : buffers_)
    if (existing.buffer == buffer.buffer || existing.name == buffer.name)
      throw std::runtime_error("Duplicate graph buffer/identity: " + buffer.name);
  BufferId id{std::uint32_t(buffers_.size())};
  buffers_.push_back(std::move(buffer));
  return id;
}
RenderGraph::PassId RenderGraph::addPass(std::string name, std::vector<Use> uses,
                                       std::vector<BufferUse> buffers) {
  if (name.empty()) throw std::runtime_error("Graph pass requires a name");
  for (auto const &p : passes_)
    if (p.name == name) throw std::runtime_error("Duplicate graph pass name: " + name);
  PassId id{std::uint32_t(passes_.size())};
  passes_.push_back({id, std::move(name), std::move(uses), {}, {}, std::move(buffers), {}});
  return id;
}
void RenderGraph::dependsOn(PassId pass, PassId predecessor) {
  if (pass.value >= passes_.size() || predecessor.value >= passes_.size())
    throw std::runtime_error("Invalid graph dependency ID");
  passes_[pass.value].dependencies.push_back(predecessor);
}
void RenderGraph::exportImage(ImageId image, Usage usage) {
  if (image.value >= images_.size() || attachment(usage))
    throw std::runtime_error("Invalid graph export");
  for (auto const &item : exports_)
    if (item.first == image) throw std::runtime_error("Duplicate graph export");
  exports_.emplace_back(image, usage);
}
RenderGraph::Plan RenderGraph::compile() const {
  Plan plan;
  plan.images_ = images_;
  plan.buffers_ = buffers_;
  std::vector<std::vector<bool>> edges(passes_.size(), std::vector<bool>(passes_.size()));
  std::vector<std::optional<PassId>> writer(images_.size());
  std::vector<std::vector<PassId>> readers(images_.size());
  std::vector<std::optional<PassId>> bufferWriter(buffers_.size());
  std::vector<std::vector<PassId>> bufferReaders(buffers_.size());
  for (auto const &pass : passes_) {
    unsigned colors = 0, depths = 0;
    std::optional<vk::Extent2D> extent;
    std::vector<bool> seen(images_.size());
    for (auto pred : pass.dependencies) edges[pred.value][pass.id.value] = true;
    for (auto const &use : pass.uses) {
      if (use.image.value >= images_.size()) throw std::runtime_error("Invalid graph image ID");
      auto index = use.image.value;
      if (seen[index]) throw std::runtime_error("Graph feedback/duplicate use: " + pass.name);
      seen[index] = true;
      auto const &image = images_[index];
      validate(image, use.usage);
      if (use.usage == Usage::Present) throw std::runtime_error("Present must be a graph export");
      if (attachment(use.usage)) {
        if (use.usage == Usage::ColorAttachment) ++colors; else ++depths;
        if (extent && *extent != image.extent) throw std::runtime_error("Graph attachment extents differ");
        extent = image.extent;
      } else if ((use.usage == Usage::RayTracingWrite || use.usage == Usage::RayTracingReadWrite)) {
        if (!use.fullOverwrite || use.load != vk::AttachmentLoadOp::eDontCare || use.store != vk::AttachmentStoreOp::eStore)
          throw std::runtime_error("RT image write must fully overwrite contents");
      } else if (use.load != vk::AttachmentLoadOp::eDontCare ||
                 use.store != vk::AttachmentStoreOp::eStore || use.fullOverwrite) {
        throw std::runtime_error("Load/store contract on non-attachment use");
      }
      if (writer[index]) edges[writer[index]->value][pass.id.value] = true;
      if (attachment(use.usage) || (use.usage == Usage::RayTracingWrite || use.usage == Usage::RayTracingReadWrite)) {
        for (auto reader : readers[index]) edges[reader.value][pass.id.value] = true;
        readers[index].clear();
        writer[index] = pass.id;
      } else readers[index].push_back(pass.id);
    }
    std::vector<bool> seenBuffers(buffers_.size());
    for (auto const &use : pass.bufferUses) {
      if (use.buffer.value >= buffers_.size()) throw std::runtime_error("Invalid graph buffer ID");
      auto index = use.buffer.value;
      if (seenBuffers[index]) throw std::runtime_error("Duplicate graph buffer use");
      seenBuffers[index] = true;
      if (bufferWriter[index]) edges[bufferWriter[index]->value][pass.id.value] = true;
      if (writes(desired(use.usage).access)) {
        for (auto reader : bufferReaders[index]) edges[reader.value][pass.id.value] = true;
        bufferReaders[index].clear(); bufferWriter[index] = pass.id;
      } else bufferReaders[index].push_back(pass.id);
    }
    if (colors > 4 || depths > 1) throw std::runtime_error("Minimal graph supports up to four colors and one depth attachment");
  }
  std::vector<State> states;
  for (auto const &image : images_) states.push_back(image.initial);
  std::vector<BufferState> bufferStates;
  for (auto const &buffer : buffers_) bufferStates.push_back(buffer.initial);
  std::vector<bool> emitted(passes_.size());
  for (std::size_t count = 0; count < passes_.size(); ++count) {
    std::optional<std::size_t> ready;
    for (std::size_t i = 0; i < passes_.size(); ++i) {
      if (emitted[i]) continue;
      bool blocked = false;
      for (std::size_t j = 0; j < passes_.size(); ++j)
        blocked = blocked || (edges[j][i] && !emitted[j]);
      if (!blocked) { ready = i; break; }
    }
    if (!ready) throw std::runtime_error("Render graph dependency cycle");
    auto pass = passes_[*ready];
    pass.dependencies.clear();
    for (std::size_t j = 0; j < passes_.size(); ++j)
      if (edges[j][*ready]) pass.dependencies.push_back(PassId{std::uint32_t(j)});
    for (auto const &use : pass.uses)
      applyUse(images_[use.image.value], states[use.image.value], use, pass.barriers);
    for (auto const &use : pass.bufferUses)
      applyBuffer(buffers_[use.buffer.value], bufferStates[use.buffer.value], use, pass.bufferBarriers);
    plan.passes_.push_back(std::move(pass));
    emitted[*ready] = true;
  }
  for (auto const &[image, usage] : exports_) {
    validate(images_[image.value], usage);
    if (!states[image.value].defined) throw std::runtime_error("Export of undefined graph contents");
    transition(images_[image.value], states[image.value], usage, plan.exports_);
  }
  plan.final_ = states;
  plan.bufferFinal_ = bufferStates;
  for (auto const &image : images_) plan.recorded_.push_back(image.initial);
  return plan;
}
RenderGraph::State const &RenderGraph::Plan::finalState(ImageId id) const {
  if (id.value >= final_.size()) throw std::runtime_error("Invalid graph state ID");
  return final_[id.value];
}
RenderGraph::State const &RenderGraph::Plan::recordedState(ImageId id) const {
  if (id.value >= recorded_.size()) throw std::runtime_error("Invalid graph state ID");
  return recorded_[id.value];
}
RenderGraph::BufferState const &RenderGraph::Plan::finalBufferState(BufferId id) const {
  if (id.value >= bufferFinal_.size()) throw std::runtime_error("Invalid graph buffer state ID");
  return bufferFinal_[id.value];
}
void RenderGraph::Plan::record(vk::CommandBuffer command,
                             std::function<void(Pass const &, Event)> const &callback) {
  if (!command || started_) throw std::runtime_error("Invalid/reused graph recording");
  started_ = true;
  for (auto const &pass : passes_) {
    emit(command, pass.barriers);
    if (!pass.bufferBarriers.empty())
      command.pipelineBarrier2(vk::DependencyInfo{
          .bufferMemoryBarrierCount = std::uint32_t(pass.bufferBarriers.size()),
          .pBufferMemoryBarriers = pass.bufferBarriers.data()});
    for (auto const &use : pass.uses) {
      auto next = desired(use.usage);
      next.defined = recorded_[use.image.value].defined;
      if (next.layout == recorded_[use.image.value].layout && !writes(next.access) &&
          !writes(recorded_[use.image.value].access)) {
        next.stages |= recorded_[use.image.value].stages;
        next.access |= recorded_[use.image.value].access;
      }
      recorded_[use.image.value] = next;
    }
    callback(pass, Event::Begin);
    std::vector<vk::RenderingAttachmentInfo> colors;
    vk::RenderingAttachmentInfo depth{};
    bool hasDepth = false;
    vk::Extent2D extent{};
    for (auto const &use : pass.uses) {
      if (!attachment(use.usage)) continue;
      auto const &image = images_[use.image.value];
      vk::RenderingAttachmentInfo info{
          .imageView = image.view, .imageLayout = desired(use.usage).layout,
          .loadOp = use.load, .storeOp = use.store, .clearValue = use.clear};
      extent = image.extent;
      if (use.usage == Usage::ColorAttachment) colors.push_back(info);
      else { depth = info; hasDepth = true; }
    }
    if (!colors.empty() || hasDepth) {
      command.beginRendering(vk::RenderingInfo{
          .renderArea = {{0, 0}, extent}, .layerCount = 1,
          .colorAttachmentCount = std::uint32_t(colors.size()),
          .pColorAttachments = colors.data(),
          .pDepthAttachment = hasDepth ? &depth : nullptr});
      command.setViewport(0, {vk::Viewport{0, 0, float(extent.width), float(extent.height), 0, 1}});
      command.setScissor(0, {vk::Rect2D{{0, 0}, extent}});
    }
    callback(pass, Event::Draw);
    if (!colors.empty() || hasDepth) command.endRendering();
    for (auto const &use : pass.uses)
      if (attachment(use.usage) || (use.usage == Usage::RayTracingWrite || use.usage == Usage::RayTracingReadWrite))
        recorded_[use.image.value].defined = use.store == vk::AttachmentStoreOp::eStore &&
            (use.load == vk::AttachmentLoadOp::eClear || use.load == vk::AttachmentLoadOp::eLoad || use.fullOverwrite);
    callback(pass, Event::End);
  }
  emit(command, exports_);
  recorded_ = final_;
}
std::string RenderGraph::Plan::dump() const {
  std::ostringstream out;
  out << "Render Graph: single graphics queue, whole images, no aliasing\n";
  for (std::size_t i = 0; i < images_.size(); ++i) {
    auto const &image = images_[i];
    out << "image " << i << ' ' << image.name << ' ' << vk::to_string(image.format) << ' '
        << image.extent.width << 'x' << image.extent.height << " initial=" << vk::to_string(image.initial.layout)
        << " defined=" << image.initial.defined << " final=" << vk::to_string(final_[i].layout)
        << " defined=" << final_[i].defined << '\n';
  }
  auto barriers = [&](auto const &items) {
    for (auto const &b : items) {
      auto image = std::find_if(images_.begin(), images_.end(), [&](auto const &item) { return item.image == b.image; });
      out << "  barrier " << image->name << ": " << vk::to_string(b.oldLayout) << " -> " << vk::to_string(b.newLayout)
          << " src=" << vk::to_string(b.srcStageMask) << '/' << vk::to_string(b.srcAccessMask)
          << " dst=" << vk::to_string(b.dstStageMask) << '/' << vk::to_string(b.dstAccessMask) << '\n';
    }
  };
  for (auto const &pass : passes_) {
    out << "pass " << pass.id.value << ' ' << pass.name << " dependencies=";
    for (auto id : pass.dependencies) out << id.value << ' ';
    out << '\n';
    for (auto const &use : pass.uses) {
      out << "  " << images_[use.image.value].name << ' ' << vk::to_string(desired(use.usage).layout);
      if (attachment(use.usage)) out << " load=" << vk::to_string(use.load) << " store=" << vk::to_string(use.store)
                                   << " full=" << use.fullOverwrite;
      out << '\n';
    }
    barriers(pass.barriers);
    for (auto const &use : pass.bufferUses)
      out << "  buffer " << buffers_[use.buffer.value].name << " usage=" << int(use.usage)
          << " full=" << use.fullOverwrite << '\n';
    for (auto const &barrier : pass.bufferBarriers)
      out << "  buffer barrier src=" << vk::to_string(barrier.srcStageMask) << '/'
          << vk::to_string(barrier.srcAccessMask) << " dst=" << vk::to_string(barrier.dstStageMask)
          << '/' << vk::to_string(barrier.dstAccessMask) << " bytes=" << barrier.size << '\n';
  }
  out << "exports\n";
  barriers(exports_);
  return out.str();
}
