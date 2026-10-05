#pragma once
#include "device.hpp"
#include "texture_mip.hpp"
#include <atomic>
#include <memory>
#include <span>
#include <vector>

// One graphics-queue submission, with staging retained until its fence signals.
// Destinations must survive completion. submit()/ready() never wait; finish()
// is reserved for initialization/tests. Recording failure discards the batch.
class UploadBatch {
public:
  struct Ticket {
    std::atomic<bool> submitted{false};
  };
  std::shared_ptr<Ticket const> ticket() const { return ticket_; }
  struct Statistics {
    std::uint64_t bufferCopies = 0, imageCopies = 0, bytes = 0;
    std::uint64_t submissions = 0, fenceWaits = 0;
  };
  explicit UploadBatch(Device const &device);
  ~UploadBatch();
  UploadBatch(UploadBatch const &) = delete;
  UploadBatch &operator=(UploadBatch const &) = delete;
  void copyBuffer(std::span<std::byte const> data, vk::Buffer destination,
                  vk::BufferUsageFlags finalUsage);
  void copyImage(std::span<std::byte const> data, vk::Image destination,
                 std::span<TextureMipLevel const> levels,
                 std::size_t texelBytes, std::uint32_t arrayLayers = 1);
  void submit();
  bool ready();
  bool submitted() const { return submitted_; }
  Statistics const &statistics() const { return statistics_; }
  Statistics finish();

private:
  using Staging = Device::BufferResources;
  vk::Buffer stage(std::span<std::byte const> data);
  Device const &device_;
  std::vector<Staging> staging_;
  vk::raii::CommandPool pool_ = nullptr;
  vk::raii::CommandBuffers commands_ = nullptr;
  vk::raii::Fence fence_ = nullptr;
  Statistics statistics_;
  std::shared_ptr<Ticket> ticket_ = std::make_shared<Ticket>();
  bool submitted_ = false, finished_ = false;
};
