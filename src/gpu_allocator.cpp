#include "gpu_allocator.hpp"
#include "device.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vk_mem_alloc.h>

namespace {
void check(VkResult result, char const *operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) +
                             " failed: " + std::to_string(result));
}
} // namespace
struct GpuAllocator::State {
  struct Block {
    ResourceLedger::Lease lease;
    bool freed = false;
  };
  VmaAllocator allocator = nullptr;
  ResourceLedger::Scope backing;
  VkPhysicalDeviceMemoryProperties properties{};
  std::mutex operations;
  std::unordered_map<VkDeviceMemory, Block> blocks;
  bool accountingFailed = false;

  // Memory callbacks cannot throw through the C API. Allocation failure is
  // reported after VMA returns; the candidate still owns and frees its
  // allocation.
  static void VKAPI_PTR allocated(VmaAllocator, uint32_t type,
                                  VkDeviceMemory memory, VkDeviceSize size,
                                  void *user) noexcept {
    auto &state = *static_cast<State *>(user);
    try {
      auto flags = state.properties.memoryTypes[type].propertyFlags;
      auto found = state.blocks.find(memory);
      if (found != state.blocks.end()) {
        // A driver may reuse a handle freed earlier in this same VMA operation.
        if (!found->second.freed) {
          state.accountingFailed = true;
          return;
        }
        state.blocks.erase(found);
      }
      auto lease = state.backing.track(
          {.allocations = 1,
           .allocatedBytes = size,
           .deviceLocalBytes =
               (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? size : 0,
           .hostVisibleBytes =
               (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? size : 0});
      state.blocks.emplace(memory, Block{std::move(lease), false});
    } catch (...) {
      state.accountingFailed = true;
    }
  }
  static void VKAPI_PTR freed(VmaAllocator, uint32_t, VkDeviceMemory memory,
                              VkDeviceSize, void *user) noexcept {
    auto &state = *static_cast<State *>(user);
    auto found = state.blocks.find(memory);
    if (found != state.blocks.end())
      found->second.freed = true;
    else
      state.accountingFailed = true;
  }
  void removeFreed() noexcept {
    // VMA calls freed BEFORE vkFreeMemory. Drain only after the complete
    // operation, under the adapter lock, so another thread cannot retire a
    // still-live block.
    std::erase_if(blocks, [](auto const &entry) { return entry.second.freed; });
  }
  explicit State(Device const &device)
      : backing(device.resourceLedger().scope(
            ResourceLedger::Domain::AllocatorBlocks)) {
    vkGetPhysicalDeviceMemoryProperties(
        static_cast<VkPhysicalDevice>(device.physicalDeviceHandle()),
        &properties);
    VmaDeviceMemoryCallbacks callbacks{};
    callbacks.pfnAllocate = allocated;
    callbacks.pfnFree = freed;
    callbacks.pUserData = this;
    VmaAllocatorCreateInfo info{};
    info.instance = static_cast<VkInstance>(device.instanceHandle());
    info.physicalDevice =
        static_cast<VkPhysicalDevice>(device.physicalDeviceHandle());
    info.device = static_cast<VkDevice>(device.deviceHandle());
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.pDeviceMemoryCallbacks = &callbacks;
    // A conservative initial block size; allocation policy tuning needs
    // measurement.
    info.preferredLargeHeapBlockSize = 16 * 1024 * 1024;
    check(vmaCreateAllocator(&info, &allocator), "create GPU allocator");
  }
  ~State() {
    vmaDestroyAllocator(allocator);
    removeFreed();
  }
};
struct GpuAllocator::Impl {
  Device const &device;
  mutable std::mutex mutex;
  mutable std::array<std::weak_ptr<State>,
                     static_cast<std::size_t>(Lifetime::Count)>
      states;
  explicit Impl(Device const &device) : device(device) {}
  std::shared_ptr<State> acquire(Lifetime lifetime) const {
    std::lock_guard lock(mutex);
    auto &slot = states.at(static_cast<std::size_t>(lifetime));
    auto current = slot.lock();
    if (!current) {
      current = std::make_shared<State>(device);
      slot = current;
    }
    return current;
  }
  std::array<std::shared_ptr<State>, static_cast<std::size_t>(Lifetime::Count)>
  existing() const {
    std::lock_guard lock(mutex);
    std::array<std::shared_ptr<State>,
               static_cast<std::size_t>(Lifetime::Count)>
        current;
    for (std::size_t i = 0; i < states.size(); ++i)
      current[i] = states[i].lock();
    return current;
  }
};
struct GpuAllocation {
  std::shared_ptr<GpuAllocator::State> state;
  VmaAllocation handle = nullptr;
  GpuMemoryInfo info;
  explicit GpuAllocation(std::shared_ptr<GpuAllocator::State> state)
      : state(std::move(state)) {}
  ~GpuAllocation() {
    if (handle) {
      std::lock_guard lock(state->operations);
      vmaFreeMemory(state->allocator, handle);
      state->removeFreed();
    }
  }
  void bounds(std::size_t size, vk::DeviceSize offset) const {
    if (offset > info.payloadBytes || size > info.payloadBytes - offset)
      throw std::runtime_error("Buffer transfer exceeds payload range");
    if (!(info.properties & vk::MemoryPropertyFlagBits::eHostVisible))
      throw std::runtime_error("Buffer is not host visible");
  }
  struct Mapping {
    GpuAllocation const &allocation;
    void *data = nullptr;
    explicit Mapping(GpuAllocation const &allocation) : allocation(allocation) {
      check(vmaMapMemory(allocation.state->allocator, allocation.handle, &data),
            "map buffer");
    }
    ~Mapping() {
      vmaUnmapMemory(allocation.state->allocator, allocation.handle);
    }
  };
};
GpuBuffer::GpuBuffer() = default;
GpuBuffer::~GpuBuffer() { reset(); }
GpuBuffer::GpuBuffer(GpuBuffer &&other) noexcept = default;
GpuBuffer &GpuBuffer::operator=(GpuBuffer &&other) noexcept {
  if (this != &other) {
    reset();
    accounting_ = std::move(other.accounting_);
    allocation_ = std::move(other.allocation_);
    buffer = std::move(other.buffer);
  }
  return *this;
}
void GpuBuffer::reset() noexcept {
  buffer.clear();
  allocation_.reset();
  accounting_.reset();
}
bool GpuBuffer::valid() const {
  return buffer != nullptr && allocation_ && allocation_->handle;
}
GpuBuffer::MemoryInfo GpuBuffer::memoryInfo() const {
  if (!valid())
    throw std::runtime_error("Buffer has no allocation");
  return allocation_->info;
}
void GpuBuffer::write(std::span<std::byte const> bytes,
                      vk::DeviceSize offset) const {
  if (!valid())
    throw std::runtime_error("Cannot write an empty buffer");
  allocation_->bounds(bytes.size(), offset);
  if (bytes.empty())
    return;
  GpuAllocation::Mapping mapping(*allocation_);
  std::memcpy(static_cast<std::byte *>(mapping.data) + offset, bytes.data(),
              bytes.size());
  check(vmaFlushAllocation(allocation_->state->allocator, allocation_->handle,
                           offset, bytes.size()),
        "flush buffer");
}
void GpuBuffer::read(std::span<std::byte> bytes, vk::DeviceSize offset) const {
  if (!valid())
    throw std::runtime_error("Cannot read an empty buffer");
  allocation_->bounds(bytes.size(), offset);
  if (bytes.empty())
    return;
  GpuAllocation::Mapping mapping(*allocation_);
  check(vmaInvalidateAllocation(allocation_->state->allocator,
                                allocation_->handle, offset, bytes.size()),
        "invalidate buffer");
  std::memcpy(bytes.data(),
              static_cast<std::byte const *>(mapping.data) + offset,
              bytes.size());
}
GpuImage::GpuImage() = default;
GpuImage::~GpuImage() { reset(); }
GpuImage::GpuImage(GpuImage &&other) noexcept = default;
GpuImage &GpuImage::operator=(GpuImage &&other) noexcept {
  if (this != &other) {
    reset();
    accounting_ = std::move(other.accounting_);
    allocation_ = std::move(other.allocation_);
    image = std::move(other.image);
  }
  return *this;
}
void GpuImage::reset() noexcept {
  image.clear();
  allocation_.reset();
  accounting_.reset();
}
bool GpuImage::valid() const {
  return image != nullptr && allocation_ && allocation_->handle;
}
GpuImage::MemoryInfo GpuImage::memoryInfo() const {
  if (!valid())
    throw std::runtime_error("Image has no allocation");
  return allocation_->info;
}
GpuAllocator::GpuAllocator(Device const &device)
    : impl_(std::make_unique<Impl>(device)) {}
GpuAllocator::~GpuAllocator() = default;
GpuBuffer GpuAllocator::createBuffer(vk::DeviceSize size,
                                     vk::BufferUsageFlags usage,
                                     vk::MemoryPropertyFlags required,
                                     ResourceLedger::Scope scope,
                                     Lifetime lifetime) const {
  if (!size || !usage)
    throw std::runtime_error("Buffer size/usage must be nonzero");
  GpuBuffer result;
  result.buffer = vk::raii::Buffer(
      impl_->device.logicalDevice(),
      vk::BufferCreateInfo{.size = size,
                           .usage = usage,
                           .sharingMode = vk::SharingMode::eExclusive});
  auto state = impl_->acquire(lifetime);
  result.allocation_ = std::make_unique<GpuAllocation>(state);
  VmaAllocationCreateInfo create{};
  create.usage = VMA_MEMORY_USAGE_UNKNOWN;
  create.requiredFlags = static_cast<VkMemoryPropertyFlags>(required);
  if (required & vk::MemoryPropertyFlagBits::eHostVisible)
    create.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VmaAllocationInfo info{};
  {
    std::lock_guard lock(state->operations);
    auto status = vmaAllocateMemoryForBuffer(
        state->allocator, static_cast<VkBuffer>(*result.buffer), &create,
        &result.allocation_->handle, &info);
    state->removeFreed();
    check(status, "allocate buffer");
    if (state->accountingFailed)
      throw std::runtime_error("Allocator backing accounting failed");
    check(vmaBindBufferMemory(state->allocator, result.allocation_->handle,
                              static_cast<VkBuffer>(*result.buffer)),
          "bind buffer");
  }
  auto const flags =
      state->properties.memoryTypes[info.memoryType].propertyFlags;
  result.allocation_->info = {.block = vk::DeviceMemory{info.deviceMemory},
                              .offset = info.offset,
                              .suballocationBytes = info.size,
                              .payloadBytes = size,
                              .properties = vk::MemoryPropertyFlags{flags}};
  if (!scope)
    scope = impl_->device.resourceLedger().scope(
        ResourceLedger::Domain::Persistent);
  result.accounting_ = scope.track(
      {.buffers = 1,
       .payloadBytes = size,
       .suballocations = 1,
       .suballocatedBytes = info.size,
       .suballocationDeviceLocalBytes =
           (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? info.size : 0,
       .suballocationHostVisibleBytes =
           (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? info.size : 0});
  return result;
}
GpuImage GpuAllocator::createImage(vk::ImageCreateInfo const &description,
                                   vk::DeviceSize payloadBytes,
                                   vk::MemoryPropertyFlags required,
                                   ResourceLedger::Scope scope) const {
  // This adapter owns one fully bound, nonsparse allocation per image.
  auto unsupported = vk::ImageCreateFlagBits::eSparseBinding |
                     vk::ImageCreateFlagBits::eSparseResidency |
                     vk::ImageCreateFlagBits::eSparseAliased |
                     vk::ImageCreateFlagBits::eDisjoint;
  if (!description.extent.width || !description.extent.height ||
      !description.extent.depth || !description.mipLevels ||
      !description.arrayLayers || !description.usage ||
      description.format == vk::Format::eUndefined || !payloadBytes ||
      (description.flags & unsupported))
    throw std::runtime_error(
        "Invalid or unsupported allocated image description");
  GpuImage result;
  result.image = vk::raii::Image(impl_->device.logicalDevice(), description);
  auto state = impl_->acquire(Lifetime::Resident);
  result.allocation_ = std::make_unique<GpuAllocation>(state);
  VmaAllocationCreateInfo create{};
  create.usage = VMA_MEMORY_USAGE_UNKNOWN;
  create.requiredFlags = static_cast<VkMemoryPropertyFlags>(required);
  VmaAllocationInfo info{};
  {
    std::lock_guard lock(state->operations);
    // Default VMA pools retain buffer-image granularity handling. Never set
    // IGNORE_BUFFER_IMAGE_GRANULARITY on this mixed resident allocator.
    auto status = vmaAllocateMemoryForImage(
        state->allocator, static_cast<VkImage>(*result.image), &create,
        &result.allocation_->handle, &info);
    state->removeFreed();
    check(status, "allocate image");
    if (state->accountingFailed)
      throw std::runtime_error("Allocator backing accounting failed");
    check(vmaBindImageMemory(state->allocator, result.allocation_->handle,
                             static_cast<VkImage>(*result.image)),
          "bind image");
  }
  auto flags = state->properties.memoryTypes[info.memoryType].propertyFlags;
  result.allocation_->info = {.block = vk::DeviceMemory{info.deviceMemory},
                              .offset = info.offset,
                              .suballocationBytes = info.size,
                              .payloadBytes = payloadBytes,
                              .properties = vk::MemoryPropertyFlags{flags}};
  if (!scope)
    scope = impl_->device.resourceLedger().scope(
        ResourceLedger::Domain::Persistent);
  result.accounting_ = scope.track(
      {.images = 1,
       .payloadBytes = payloadBytes,
       .suballocations = 1,
       .suballocatedBytes = info.size,
       .suballocationDeviceLocalBytes =
           (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? info.size : 0,
       .suballocationHostVisibleBytes =
           (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? info.size : 0});
  return result;
}
GpuAllocator::Statistics GpuAllocator::statistics() const {
  Statistics total;
  for (auto const &state : impl_->existing()) {
    if (!state)
      continue;
    std::lock_guard lock(state->operations);
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(state->allocator, &stats);
    auto const &s = stats.total.statistics;
    total.blocks += s.blockCount;
    total.blockBytes += s.blockBytes;
    total.suballocations += s.allocationCount;
    total.suballocatedBytes += s.allocationBytes;
  }
  return total;
}
