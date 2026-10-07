#include "renderer.hpp"
#include <stdexcept>

void Renderer::invalidateTemporalHistory() {
  if (activeFrame_)
    throw std::runtime_error("Cannot reset temporal history during recording");
  temporalHistory_.reset();
  if (taa_)
    taa_->reset();
  lastTemporalCamera_ = {};
}
void Renderer::setTaaHistoryFilter(TaaHistoryFilter filter) {
  if (activeFrame_)
    throw std::runtime_error("Cannot change history filter during recording");
  if (filter != TaaHistoryFilter::Bilinear &&
      filter != TaaHistoryFilter::CatmullRom)
    throw std::runtime_error("Invalid history filter");
  if (filter != taaHistoryFilter_) {
    taaHistoryFilter_ = filter;
    if (taa_)
      taa_->reset();
  }
}
void Renderer::setTaaEnabled(bool enabled) {
  if (activeFrame_)
    throw std::runtime_error("Cannot change TAA during recording");
  if (enabled != taaEnabled_) {
    taaEnabled_ = enabled;
    temporalJitterEnabled_ = enabled;
    invalidateTemporalHistory();
  }
}
void Renderer::setTemporalJitterEnabled(bool enabled) {
  if (activeFrame_)
    throw std::runtime_error("Cannot change jitter during recording");
  if (enabled != temporalJitterEnabled_) {
    temporalJitterEnabled_ = enabled;
    if (!enabled)
      taaEnabled_ = false;
    invalidateTemporalHistory();
  }
}
void Renderer::updateFrameMotion(FrameContext &frame,
                                 TemporalSnapshot &&snapshot) {
  auto bytes = std::as_bytes(std::span{snapshot.gpu});
  auto limit =
      device_.physicalDevice().getProperties().limits.maxStorageBufferRange;
  if (bytes.size() > limit)
    throw std::runtime_error("Motion snapshot exceeds storage buffer range");
  if (bytes.size() > frame.motionCapacityBytes) {
    auto replacement = device_.createBuffer(
        bytes.size(), vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible);
    replacement.write(bytes);
    vk::DescriptorBufferInfo info{.buffer = *replacement.buffer,
                                  .range = bytes.size()};
    device_.logicalDevice().updateDescriptorSets(
        {vk::WriteDescriptorSet{.dstSet = frame.descriptorSet,
                                .dstBinding = 10,
                                .descriptorCount = 1,
                                .descriptorType =
                                    vk::DescriptorType::eStorageBuffer,
                                .pBufferInfo = &info}},
        {});
    frame.motionBuffer = std::move(replacement);
    frame.motionCapacityBytes = bytes.size();
  } else
    frame.motionBuffer.write(bytes);
  lastTemporalCamera_ = snapshot.camera;
  frame.temporal = std::move(snapshot);
}
Renderer::DepthResources
Renderer::createMotionResources(vk::Extent2D extent) const {
  DepthResources r;
  auto scope =
      device_.resourceLedger().scope(ResourceLedger::Domain::Persistent);
  r.storage = device_.createImage(
      vk::ImageCreateInfo{.imageType = vk::ImageType::e2D,
                          .format = kMotionFormat,
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
  r.imageView = vk::raii::ImageView(
      device_.logicalDevice(),
      vk::ImageViewCreateInfo{
          .image = *r.storage.image,
          .viewType = vk::ImageViewType::e2D,
          .format = kMotionFormat,
          .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
  r.accounting = scope.track({.imageViews = 1});
  return r;
}
