#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include "resource_ledger.hpp"
#include "vulkan_include.hpp"

class Device;
class GpuAllocator;
struct GpuAllocation;
struct GpuMemoryInfo {
  vk::DeviceMemory block;
  vk::DeviceSize offset = 0, suballocationBytes = 0, payloadBytes = 0;
  vk::MemoryPropertyFlags properties;
};

// Device must outlive its resources. GPU use must complete before
// release/write. VMA types stay private. Last resource releases its allocator's
// backing blocks.
class GpuBuffer {
  friend class GpuAllocator;
  ResourceLedger::Lease accounting_;
  std::unique_ptr<GpuAllocation> allocation_;

public:
  // Destroy the handle before freeing its allocation, then remove accounting.
  vk::raii::Buffer buffer = nullptr;
  using MemoryInfo = GpuMemoryInfo;
  GpuBuffer();
  ~GpuBuffer();
  GpuBuffer(GpuBuffer const &) = delete;
  GpuBuffer &operator=(GpuBuffer const &) = delete;
  GpuBuffer(GpuBuffer &&) noexcept;
  GpuBuffer &operator=(GpuBuffer &&) noexcept;
  bool valid() const;
  MemoryInfo memoryInfo() const;
  // Allocation-relative, bounded transfers; flush/invalidate handle atom
  // alignment. GPU/CPU synchronization remains the caller's responsibility.
  void write(std::span<std::byte const> bytes, vk::DeviceSize offset = 0) const;
  void read(std::span<std::byte> bytes, vk::DeviceSize offset = 0) const;

private:
  void reset() noexcept;
};

// Views must die before this owner. Layout tracking and transfers are external.
class GpuImage {
  friend class GpuAllocator;
  ResourceLedger::Lease accounting_;
  std::unique_ptr<GpuAllocation> allocation_;

public:
  vk::raii::Image image = nullptr;
  using MemoryInfo = GpuMemoryInfo;
  GpuImage();
  ~GpuImage();
  GpuImage(GpuImage const &) = delete;
  GpuImage &operator=(GpuImage const &) = delete;
  GpuImage(GpuImage &&) noexcept;
  GpuImage &operator=(GpuImage &&) noexcept;
  bool valid() const;
  MemoryInfo memoryInfo() const;

private:
  void reset() noexcept;
};

class GpuAllocator {
  friend struct GpuAllocation;
  struct State;
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  enum class Lifetime { Resident, Upload, Count };
  struct Statistics {
    std::uint64_t blocks = 0, blockBytes = 0, suballocations = 0,
                  suballocatedBytes = 0;
  };
  explicit GpuAllocator(Device const &device);
  ~GpuAllocator();
  GpuAllocator(GpuAllocator const &) = delete;
  GpuAllocator &operator=(GpuAllocator const &) = delete;
  GpuBuffer createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                         vk::MemoryPropertyFlags required,
                         ResourceLedger::Scope scope,
                         Lifetime lifetime = Lifetime::Resident) const;
  // Payload is caller-provided texel data size, not Vulkan memory requirements.
  // Image/view/sampler semantics and upload scheduling stay with their owners.
  GpuImage createImage(vk::ImageCreateInfo const &description,
                       vk::DeviceSize payloadBytes,
                       vk::MemoryPropertyFlags required,
                       ResourceLedger::Scope scope) const;
  // Compare with ledger only at quiescent points; this is a separate sample.
  Statistics statistics() const;
};
