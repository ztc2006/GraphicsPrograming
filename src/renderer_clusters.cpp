#include "pch.hpp"
#include "renderer.hpp"
#include <span>

void Renderer::createClusterPipeline() {
  auto const limits = device_.physicalDevice().getProperties().limits;
  auto queue =
      device_.physicalDevice()
          .getQueueFamilyProperties()[device_.graphicsQueueFamilyIndex()];
  clusterSupported_ = bool(queue.queueFlags & vk::QueueFlagBits::eCompute) &&
                      limits.maxComputeWorkGroupInvocations >= 64 &&
                      limits.maxComputeWorkGroupSize[0] >= 64;
  if (!clusterSupported_)
    return;
  auto layout = *frameDescriptorSetLayout_;
  clusterPipelineLayout_ = vk::raii::PipelineLayout(
      device_.logicalDevice(),
      vk::PipelineLayoutCreateInfo{.setLayoutCount = 1,
                                   .pSetLayouts = &layout});
  auto code = readBinaryFile("shaders/cluster_cull.comp.spv");
  vk::raii::ShaderModule module(
      device_.logicalDevice(),
      vk::ShaderModuleCreateInfo{
          .codeSize = code.size(),
          .pCode = reinterpret_cast<std::uint32_t const *>(code.data())});
  clusterPipeline_ = vk::raii::Pipeline(
      device_.logicalDevice(), nullptr,
      vk::ComputePipelineCreateInfo{
          .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                    .module = *module,
                    .pName = "main"},
          .layout = *clusterPipelineLayout_});
  ++resourceStatistics_.pipelineBuilds;
  device_.nameObject(*clusterPipeline_, "Cluster light assignment");
}

void Renderer::updateFrameClusters(FrameContext &frame,
                                   ClusterGrid const &grid) {
  auto bytes = clusterListBytes(grid);
  if (bytes > frame.clusterCapacityBytes) {
    auto replacement =
        device_.createBuffer(bytes,
                             vk::BufferUsageFlagBits::eStorageBuffer |
                                 vk::BufferUsageFlagBits::eTransferSrc,
                             vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::DescriptorBufferInfo info{.buffer = *replacement.buffer,
                                  .range = bytes};
    device_.logicalDevice().updateDescriptorSets(
        {vk::WriteDescriptorSet{.dstSet = frame.descriptorSet,
                                .dstBinding = 8,
                                .descriptorCount = 1,
                                .descriptorType =
                                    vk::DescriptorType::eStorageBuffer,
                                .pBufferInfo = &info}},
        {});
    frame.clusterIndices = std::move(replacement);
    frame.clusterCapacityBytes = bytes;
    frame.clusterState = {};
    device_.nameObject(*frame.clusterIndices.buffer, "Frame cluster indices");
  }
  frame.clusterConfig.write(std::as_bytes(std::span{&grid, 1}));
  frame.clusterGrid = grid;
  frame.clusterEnabled = grid.screen.z != 0;
  lastClusterGrid_ = grid;
}
