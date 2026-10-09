#include "render_graph.hpp"
#include <iostream>
#include <algorithm>
#include <stdexcept>

namespace {
using G = RenderGraph;
void require(bool condition, char const *message) {
  if (!condition) throw std::runtime_error(message);
}
template <class F> void rejects(F operation, char const *message) {
  bool rejected = false;
  try { operation(); } catch (std::runtime_error const &) { rejected = true; }
  require(rejected, message);
}
// Opaque identities for the pure compiler. No Vulkan instance/device is created.
G::Image image(unsigned id, bool depth = false) {
  return {"Image " + std::to_string(id),
      vk::Image(reinterpret_cast<VkImage>(std::uintptr_t(id))),
      vk::ImageView(reinterpret_cast<VkImageView>(std::uintptr_t(id))),
      depth ? vk::Format::eD32Sfloat : vk::Format::eR16G16B16A16Sfloat,
      {64, 64}, depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
      (depth ? vk::ImageUsageFlagBits::eDepthStencilAttachment : vk::ImageUsageFlagBits::eColorAttachment) |
          vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc};
}
G::Use color(G::ImageId id, vk::AttachmentLoadOp load = vk::AttachmentLoadOp::eClear,
             vk::AttachmentStoreOp store = vk::AttachmentStoreOp::eStore) {
  return {id, G::Usage::ColorAttachment, load, store};
}
void sceneGraph() {
  G graph;
  auto shadow = graph.importImage(image(1, true));
  auto depth = graph.importImage(image(2, true));
  auto hdr = graph.importImage(image(3));
  auto outputDescription = image(4);
  outputDescription.presentable = true;
  auto output = graph.importImage(outputDescription);
  auto shadowPass = graph.addPass("Shadow", {{shadow, G::Usage::DepthAttachment,
      vk::AttachmentLoadOp::eClear}});
  auto main = graph.addPass("Main", {{shadow, G::Usage::SampledDepth}, color(hdr),
      {depth, G::Usage::DepthAttachment, vk::AttachmentLoadOp::eClear}});
  auto display = graph.addPass("Output", {{hdr, G::Usage::SampledColor},
      {output, G::Usage::ColorAttachment, vk::AttachmentLoadOp::eDontCare,
          vk::AttachmentStoreOp::eStore, true}});
  auto ui = graph.addPass("UI", {color(output, vk::AttachmentLoadOp::eLoad)});
  graph.exportImage(depth, G::Usage::SampledDepth);
  graph.exportImage(output, G::Usage::Present);
  auto plan = graph.compile();
  auto const &passes = plan.passes();
  require(passes.size() == 4 && passes[0].id == shadowPass && passes[1].id == main &&
      passes[2].id == display && passes[3].id == ui, "Scene order changed");
  require(passes[1].dependencies == std::vector{shadowPass} &&
      passes[2].dependencies == std::vector{main} &&
      passes[3].dependencies == std::vector{display}, "Missing RAW/WAW dependencies");
  auto const &shadowRead = passes[1].barriers[0];
  require(shadowRead.srcStageMask == (vk::PipelineStageFlagBits2::eEarlyFragmentTests |
      vk::PipelineStageFlagBits2::eLateFragmentTests) &&
      bool(shadowRead.srcAccessMask & vk::AccessFlagBits2::eDepthStencilAttachmentWrite) &&
      shadowRead.newLayout == vk::ImageLayout::eDepthReadOnlyOptimal,
      "Depth dependency does not include early and late tests");
  auto const &uiBarrier = passes[3].barriers;
  require(uiBarrier.size() == 1 && uiBarrier[0].oldLayout == uiBarrier[0].newLayout &&
      bool(uiBarrier[0].srcAccessMask & vk::AccessFlagBits2::eColorAttachmentWrite) &&
      bool(uiBarrier[0].dstAccessMask & vk::AccessFlagBits2::eColorAttachmentRead),
      "UI LOAD lacks same-layout write/read dependency");
  require(plan.exportBarriers().size() == 2 && plan.finalState(depth).defined &&
      plan.finalState(depth).layout == vk::ImageLayout::eDepthReadOnlyOptimal &&
      plan.finalState(output).layout == vk::ImageLayout::ePresentSrcKHR &&
      !plan.finalState(output).stages && !plan.finalState(output).access,
      "Depth/presentation export mismatch");
  require(plan.recordedState(hdr) == G::State{} && graph.compile().recordedState(hdr) == G::State{},
      "Compile mutated imported/recorded state");
  require(plan.dump().find("barrier Image 4: ColorAttachmentOptimal -> ColorAttachmentOptimal") !=
      std::string::npos, "Inspectable UI barrier is absent from dump");
}
void dependencies() {
  G graph;
  auto input = image(1);
  input.initial = {vk::ImageLayout::eShaderReadOnlyOptimal,
      vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, true};
  auto id = graph.importImage(input);
  auto readA = graph.addPass("Read A", {{id, G::Usage::SampledColor}});
  auto readB = graph.addPass("Read B", {{id, G::Usage::SampledColor}});
  auto write = graph.addPass("Rewrite", {color(id)});
  auto readC = graph.addPass("Read C", {{id, G::Usage::SampledColor}});
  auto writeAgain = graph.addPass("Rewrite again", {color(id)});
  auto plan = graph.compile();
  require(plan.passes()[0].barriers.empty() && plan.passes()[1].barriers.empty(),
      "Read-only same-layout uses added unnecessary barriers");
  require(plan.passes()[2].dependencies == std::vector{readA, readB} &&
      plan.passes()[3].dependencies == std::vector{write} &&
      plan.passes()[4].dependencies == std::vector{write, readC},
      "WAR/RAW/WAW ordering is incomplete");
  graph.dependsOn(readA, write);
  rejects([&] { graph.compile(); }, "Hazard/explicit edge cycle accepted");
  G independent;
  auto a = independent.addPass("A", {});
  auto b = independent.addPass("B", {});
  auto c = independent.addPass("C", {});
  independent.dependsOn(a, c);
  auto order = independent.compile().passes();
  require(order[0].id == b && order[1].id == c && order[2].id == a,
      "Explicit dependency or stable ready ordering ignored");
  G imported;
  input.initial.access = vk::AccessFlagBits2::eMemoryWrite;
  auto previousWrite = imported.importImage(input);
  imported.addPass("Read external write", {{previousWrite, G::Usage::SampledColor}});
  require(imported.compile().passes()[0].barriers.size() == 1,
      "Imported write scope did not produce a memory dependency");
}
void contentContracts() {
  for (auto load : {vk::AttachmentLoadOp::eLoad, vk::AttachmentLoadOp::eDontCare}) {
    G graph;
    auto id = graph.importImage(image(1));
    graph.addPass("Unknown content", {color(id, load)});
    graph.addPass("Read", {{id, G::Usage::SampledColor}});
    rejects([&] { graph.compile(); }, "Undefined LOAD/partial content read accepted");
  }
  G discarded;
  auto id = discarded.importImage(image(1));
  discarded.addPass("Discard", {color(id, vk::AttachmentLoadOp::eClear,
      vk::AttachmentStoreOp::eDontCare)});
  discarded.exportImage(id, G::Usage::TransferSource);
  rejects([&] { discarded.compile(); }, "Discarded store exported as defined");
  G cleared;
  auto clearId = cleared.importImage(image(1));
  cleared.addPass("Clear only", {color(clearId)});
  cleared.addPass("Load", {color(clearId, vk::AttachmentLoadOp::eLoad)});
  cleared.exportImage(clearId, G::Usage::TransferSource);
  require(cleared.compile().finalState(clearId).defined, "Clear-only contents were not preserved");
  G full;
  auto fullId = full.importImage(image(1));
  full.addPass("Full coverage", {{fullId, G::Usage::ColorAttachment,
      vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore, true}});
  full.exportImage(fullId, G::Usage::SampledColor);
  require(full.compile().finalState(fullId).defined, "Guaranteed full overwrite is not defined");
}
void validation() {
  G duplicate;
  duplicate.importImage(image(1));
  rejects([&] { duplicate.importImage(image(1)); }, "Duplicate identity imported");
  G invalid;
  auto undefined = image(1); undefined.initial.defined = true;
  rejects([&] { invalid.importImage(undefined); }, "Undefined layout marked defined");
  auto id = invalid.importImage(image(1));
  rejects([&] { invalid.exportImage(id, G::Usage::ColorAttachment); }, "Attachment exported");
  rejects([&] { invalid.dependsOn({0}, {1}); }, "Invalid pass dependency accepted");
  rejects([&] { invalid.compile().finalState({99}); }, "Invalid state ID accepted");
  G feedback;
  auto feedbackId = feedback.importImage(image(1));
  feedback.addPass("Feedback", {color(feedbackId), {feedbackId, G::Usage::SampledColor}});
  rejects([&] { feedback.compile(); }, "Attachment sampling feedback accepted");
  G usage;
  auto unsupported = image(1); unsupported.usage = vk::ImageUsageFlagBits::eSampled;
  auto unsupportedId = usage.importImage(unsupported);
  usage.addPass("Unsupported attachment", {color(unsupportedId)});
  rejects([&] { usage.compile(); }, "Missing attachment usage accepted");
  G extent;
  auto colorId = extent.importImage(image(1));
  auto other = image(2, true); other.extent.width = 32;
  auto depthId = extent.importImage(other);
  extent.addPass("Extent mismatch", {color(colorId), {depthId, G::Usage::DepthAttachment,
      vk::AttachmentLoadOp::eClear}});
  rejects([&] { extent.compile(); }, "Different attachment extents accepted");
  G present;
  auto presentId = present.importImage(image(1));
  present.addPass("Clear", {color(presentId)});
  present.exportImage(presentId, G::Usage::Present);
  rejects([&] { present.compile(); }, "Offscreen image presented");
}
void bufferContracts() {
  auto description = G::Buffer{"lights", vk::Buffer(reinterpret_cast<VkBuffer>(std::uintptr_t(12))),
      0, 128, vk::BufferUsageFlagBits::eStorageBuffer,
      {vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite, true}};
  G g;
  auto lights = g.importBuffer(description);
  description.name = "indices"; description.buffer = vk::Buffer(reinterpret_cast<VkBuffer>(std::uintptr_t(13)));
  description.initial = {};
  auto indices = g.importBuffer(description);
  auto cull = g.addPass("cull", {}, {{lights, G::BufferUsage::ComputeRead},
                                    {indices, G::BufferUsage::ComputeWrite, true}});
  auto draw = g.addPass("draw", {}, {{lights, G::BufferUsage::FragmentRead},
                                    {indices, G::BufferUsage::FragmentRead}});
  auto plan = g.compile();
  require(plan.passes()[1].dependencies == std::vector{cull}, "Compute RAW edge missing");
  auto barrier = plan.passes()[1].bufferBarriers.back();
  require(barrier.srcStageMask == vk::PipelineStageFlagBits2::eComputeShader &&
          barrier.dstStageMask == vk::PipelineStageFlagBits2::eFragmentShader &&
          barrier.srcAccessMask == vk::AccessFlagBits2::eShaderStorageWrite &&
          barrier.dstAccessMask == vk::AccessFlagBits2::eShaderStorageRead && barrier.size == 128,
          "Compute-to-fragment storage barrier mismatch");
  require(std::any_of(plan.passes()[1].bufferBarriers.begin(), plan.passes()[1].bufferBarriers.end(),
      [&](auto const &b) { return b.buffer == g.compile().passes()[0].bufferBarriers[0].buffer &&
          bool(b.srcAccessMask & vk::AccessFlagBits2::eHostWrite) &&
          bool(b.dstStageMask & vk::PipelineStageFlagBits2::eFragmentShader); }),
      "Read-to-read stage change lost original writer visibility");
  require(plan.finalBufferState(indices).defined, "Cluster buffer remains undefined");
  auto rewrite = g.addPass("rewrite", {}, {{indices, G::BufferUsage::ComputeWrite, true}});
  auto again = g.compile();
  require(again.passes().back().dependencies == std::vector{cull, draw}, "Buffer WAR/WAW missing");
  g.dependsOn(cull, rewrite);
  rejects([&] { g.compile(); }, "Buffer hazard cycle accepted");
  G vertexGraph;
  auto hostDescription=description;
  hostDescription.initial={vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite, true};
  auto shadowConfig=vertexGraph.importBuffer(hostDescription);
  vertexGraph.addPass("shadow vertex",{},{{shadowConfig,G::BufferUsage::VertexRead}});
  vertexGraph.addPass("main fragment",{},{{shadowConfig,G::BufferUsage::FragmentRead}});
  auto vertexPlan=vertexGraph.compile();
  for (unsigned pass=0;pass<2;++pass) {
    auto b=vertexPlan.passes()[pass].bufferBarriers.at(0);
    require(bool(b.srcStageMask & vk::PipelineStageFlagBits2::eHost) &&
      bool(b.srcAccessMask & vk::AccessFlagBits2::eHostWrite) &&
      b.dstStageMask==(pass==0?vk::PipelineStageFlagBits2::eVertexShader:vk::PipelineStageFlagBits2::eFragmentShader),
      "Host config write not made visible to both shadow vertex and main fragment");
  }
  G bad; auto unknown = bad.importBuffer(description);
  bad.addPass("read", {}, {{unknown, G::BufferUsage::FragmentRead}});
  rejects([&] { bad.compile(); }, "Undefined buffer read accepted");
  rejects([&] { bad.importBuffer(description); }, "Duplicate buffer identity accepted");
  G partial; auto partialId = partial.importBuffer(description);
  partial.addPass("partial", {}, {{partialId, G::BufferUsage::ComputeWrite}});
  partial.addPass("read", {}, {{partialId, G::BufferUsage::ComputeRead}});
  rejects([&] { partial.compile(); }, "Undefined partial buffer contents accepted");
  G same; auto sameId = same.importBuffer(description);
  same.addPass("duplicate", {}, {{sameId, G::BufferUsage::ComputeWrite, true},
                                 {sameId, G::BufferUsage::FragmentRead}});
  rejects([&] { same.compile(); }, "Buffer feedback accepted");
}
void motionMrt() {
  G g; auto hdr=g.importImage(image(1)), velocity=g.importImage(image(2));
  auto main=g.addPass("MRT",{color(hdr),color(velocity)});
  g.addPass("Read HDR and motion",{{hdr,G::Usage::SampledColor},{velocity,G::Usage::SampledColor}});
  auto plan=g.compile();
  require(plan.passes()[1].dependencies==std::vector{main} && plan.passes()[1].barriers.size()==2,
      "MRT targets did not each receive dependency/barrier");
  require(plan.finalState(hdr).defined && plan.finalState(velocity).defined,
      "MRT contents were not both preserved");
  G limit; std::vector<G::Use> uses;
  for(unsigned i=1;i<=5;++i)uses.push_back(color(limit.importImage(image(i))));
  limit.addPass("Too many targets",std::move(uses));
  rejects([&]{limit.compile();},"Graph exceeded guaranteed four color targets");
}
void rayTracingStorage() {
  G g;auto description=image(1);description.usage|=vk::ImageUsageFlagBits::eStorage;
  auto target=g.importImage(description);
  auto trace=g.addPass("RT write",{{target,G::Usage::RayTracingWrite,vk::AttachmentLoadOp::eDontCare,vk::AttachmentStoreOp::eStore,true}});
  g.addPass("RT display",{{target,G::Usage::SampledColor}});
  auto plan=g.compile();auto const &read=plan.passes()[1];
  require(read.dependencies==std::vector{trace} && read.barriers.size()==1,"RT storage write did not order sampled read");
  auto const &barrier=read.barriers[0];
  require(barrier.srcStageMask==vk::PipelineStageFlagBits2::eRayTracingShaderKHR && barrier.srcAccessMask==vk::AccessFlagBits2::eShaderStorageWrite &&
      barrier.oldLayout==vk::ImageLayout::eGeneral && barrier.newLayout==vk::ImageLayout::eShaderReadOnlyOptimal,"RT image barrier scopes incorrect");
  require(plan.finalState(target).defined,"RT full write did not define image");
  G partial;auto p=partial.importImage(description);partial.addPass("bad",{{p,G::Usage::RayTracingWrite}});
  rejects([&]{partial.compile();},"Partial undefined RT storage write accepted");
}
void rayReadWrite() {
  G g;auto d=image(7);d.usage|=vk::ImageUsageFlagBits::eStorage;
  d.initial={vk::ImageLayout::eShaderReadOnlyOptimal,vk::PipelineStageFlagBits2::eRayTracingShaderKHR|vk::PipelineStageFlagBits2::eFragmentShader,
      vk::AccessFlagBits2::eShaderStorageWrite|vk::AccessFlagBits2::eShaderSampledRead,true};
  auto id=g.importImage(d);g.addPass("mean",{{id,G::Usage::RayTracingReadWrite,vk::AttachmentLoadOp::eDontCare,vk::AttachmentStoreOp::eStore,true}});
  auto plan=g.compile();auto const &barrier=plan.passes()[0].barriers[0];
  require(bool(barrier.dstAccessMask&vk::AccessFlagBits2::eShaderStorageRead) && bool(barrier.srcAccessMask&vk::AccessFlagBits2::eShaderStorageWrite),"Running mean lost read/write dependency");
  G invalid;d.initial={};auto unknown=invalid.importImage(d);invalid.addPass("bad",{{unknown,G::Usage::RayTracingReadWrite,vk::AttachmentLoadOp::eDontCare,vk::AttachmentStoreOp::eStore,true}});
  rejects([&]{invalid.compile();},"Undefined storage image was accepted for read-modify-write");
}
} // namespace
int main() {
  try {
    sceneGraph(); dependencies(); contentContracts(); validation(); bufferContracts(); motionMrt(); rayTracingStorage(); rayReadWrite();
    std::cout << "PASS graph compiler: RAW/WAR/WAW, stable topology/cycle rejection, "
                 "depth scopes, UI same-layout LOAD, content validity, exports, pure compile\n";
    return 0;
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n'; return 1;
  }
}
