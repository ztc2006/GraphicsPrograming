#include "upload_batch.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

UploadBatch::UploadBatch(Device const &device) : device_(device) {
  pool_ = vk::raii::CommandPool(
      device.logicalDevice(),
      vk::CommandPoolCreateInfo{
          .flags = vk::CommandPoolCreateFlagBits::eTransient,
          .queueFamilyIndex = device.graphicsQueueFamilyIndex()});
  commands_ = vk::raii::CommandBuffers(
      device.logicalDevice(),
      vk::CommandBufferAllocateInfo{.commandPool = *pool_,
                                    .level = vk::CommandBufferLevel::ePrimary,
                                    .commandBufferCount = 1});
  fence_ = vk::raii::Fence(device.logicalDevice(), vk::FenceCreateInfo{});
  commands_.front().begin(vk::CommandBufferBeginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
}

UploadBatch::~UploadBatch() {
  if (submitted_ && !finished_) {
    // Never release staging or command storage while an upload is pending.
    try {
      (void)device_.logicalDevice().waitForFences(
          {*fence_}, true, std::numeric_limits<std::uint64_t>::max());
    } catch (...) { /* Device loss is handled by the caller. */
    }
  }
}

vk::Buffer UploadBatch::stage(std::span<std::byte const> data) {
  if (finished_ || submitted_)
    throw std::runtime_error("Cannot record a completed upload batch.");
  if (data.empty())
    throw std::runtime_error("Cannot upload an empty resource.");
  auto staging = device_.createUploadBuffer(data.size_bytes());
  staging.write(data);
  staging_.push_back(std::move(staging));
  statistics_.bytes += data.size_bytes();
  return *staging_.back().buffer;
}

void UploadBatch::copyBuffer(std::span<std::byte const> data,
                             vk::Buffer destination,
                             vk::BufferUsageFlags usage) {
  vk::AccessFlags2 read;
  if (usage & vk::BufferUsageFlagBits::eVertexBuffer)
    read |= vk::AccessFlagBits2::eVertexAttributeRead;
  if (usage & vk::BufferUsageFlagBits::eIndexBuffer)
    read |= vk::AccessFlagBits2::eIndexRead;
  if (!read)
    throw std::runtime_error(
        "Upload batch only supports vertex/index buffers.");
  vk::PipelineStageFlags2 stages = vk::PipelineStageFlagBits2::eVertexInput;
  if (usage & vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR) {
    stages |= vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR | vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
    read |= vk::AccessFlagBits2::eShaderRead;
  }
  auto &command = commands_.front();
  command.copyBuffer(stage(data), destination,
                     {vk::BufferCopy{.size = data.size_bytes()}});
  vk::BufferMemoryBarrier2 barrier{
      .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
      .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
      .dstStageMask = stages,
      .dstAccessMask = read,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = destination,
      .offset = 0,
      .size = data.size_bytes()};
  command.pipelineBarrier2(vk::DependencyInfo{
      .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier});
  ++statistics_.bufferCopies;
}

void UploadBatch::copyImage(std::span<std::byte const> data,
                            vk::Image destination,
                            std::span<TextureMipLevel const> levels,
                            std::size_t texelBytes, std::uint32_t arrayLayers) {
  if (levels.empty() || (texelBytes != 4 && texelBytes != 16) ||
      (arrayLayers != 1 && arrayLayers != 6 && arrayLayers != 12 &&
       arrayLayers != 18) ||
      (arrayLayers >= 6 && levels[0].width != levels[0].height))
    throw std::runtime_error("Image upload requires RGBA8 or RGBA32F levels.");
  auto expected = textureMipLayout(levels[0].width, levels[0].height,
                                   texelBytes, levels.size() > 1);
  for (auto &level : expected) {
    level.offset *= arrayLayers;
    level.size *= arrayLayers;
  }
  if (levels.size() != expected.size() ||
      !std::ranges::equal(levels, expected) ||
      data.size() != expected.back().offset + expected.back().size)
    throw std::runtime_error("Image upload mip layout does not match data.");
  auto count = static_cast<std::uint32_t>(levels.size());
  std::vector<vk::BufferImageCopy> regions;
  for (std::uint32_t mip = 0; mip < count; ++mip)
    regions.push_back(vk::BufferImageCopy{
        .bufferOffset = levels[mip].offset,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0,
                             arrayLayers},
        .imageExtent = {levels[mip].width, levels[mip].height, 1}});
  auto staging = stage(data);
  auto &command = commands_.front();
  vk::ImageMemoryBarrier2 barrier{
      .srcStageMask = vk::PipelineStageFlagBits2::eNone,
      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
      .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
      .oldLayout = vk::ImageLayout::eUndefined,
      .newLayout = vk::ImageLayout::eTransferDstOptimal,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = destination,
      .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, count, 0,
                           arrayLayers}};
  command.pipelineBarrier2(vk::DependencyInfo{
      .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
  command.copyBufferToImage(staging, destination,
                            vk::ImageLayout::eTransferDstOptimal, regions);
  barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
  barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
  barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
  if (device_.rayTracingSupported())
    barrier.dstStageMask |= vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
  barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
  barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
  barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  command.pipelineBarrier2(vk::DependencyInfo{
      .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
  ++statistics_.imageCopies;
}

vk::CommandBuffer UploadBatch::recordingCommand() const {
  if (submitted_ || finished_) throw std::runtime_error("Upload batch is not recording");
  return *commands_.front();
}
void UploadBatch::submit() {
  if (finished_ || submitted_)
    return;
  commands_.front().end();
  if (staging_.empty()) {
    finished_ = true;
    return;
  }
  vk::CommandBuffer raw = *commands_.front();
  device_.graphicsQueue().submit(
      {vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &raw}},
      *fence_);
  submitted_ = true;
  ticket_->submitted.store(true, std::memory_order_release);
  ++statistics_.submissions;
}

bool UploadBatch::ready() {
  if (finished_)
    return true;
  if (!submitted_ || fence_.getStatus() == vk::Result::eNotReady)
    return false;
  finished_ = true;
  staging_.clear();
  return true;
}

UploadBatch::Statistics UploadBatch::finish() {
  submit();
  if (finished_)
    return statistics_;
  ++statistics_.fenceWaits;
  auto result = device_.logicalDevice().waitForFences(
      {*fence_}, true, std::numeric_limits<std::uint64_t>::max());
  if (result != vk::Result::eSuccess)
    throw std::runtime_error("Scene upload fence did not complete.");
  finished_ = true;
  staging_.clear();
  return statistics_;
}
