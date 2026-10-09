#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "vulkan_include.hpp"
#include "resource_ledger.hpp"
#include "gpu_allocator.hpp"
#include "presentation.hpp"

class Device {
public:
  struct QueueFamilyIndices {
    std::uint32_t graphics = ~0u;
    std::uint32_t present = ~0u;

    bool isComplete() const { return graphics != ~0u && present != ~0u; }
  };

  Device(vk::raii::Instance const &instance,
         vk::raii::SurfaceKHR const &surface,
         std::vector<char const *> requiredDeviceExtensions,
         std::string preferredGpu = {}, bool debugUtils = false,
         PresentationInstanceSupport presentationInstance = {},
         PresentationPolicy presentationPolicy = PresentationPolicy::Automatic,
         bool allowRayTracing = true);

  using BufferResources = GpuBuffer;
  BufferResources
  createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
               vk::MemoryPropertyFlags properties,
               ResourceLedger::Scope scope = {}) const;

  // Upload memory has independent lifetime: resident UBOs cannot pin its blocks.
  BufferResources createUploadBuffer(vk::DeviceSize size) const;

  GpuAllocator::Statistics gpuAllocationStatistics() const { return gpuAllocator_.statistics(); }

  ResourceLedger const &resourceLedger() const { return resourceLedger_; }
  GpuImage createImage(vk::ImageCreateInfo const &description,
                       vk::DeviceSize payloadBytes,
                       vk::MemoryPropertyFlags properties,
                       ResourceLedger::Scope scope = {}) const;

  vk::raii::Device const &logicalDevice() const;
  vk::raii::PhysicalDevice const &physicalDevice() const;
  vk::raii::Queue const &graphicsQueue() const;
  vk::raii::Queue const &presentQueue() const;
  vk::Instance instanceHandle() const;
  vk::PhysicalDevice physicalDeviceHandle() const;
  vk::Device deviceHandle() const;
  vk::Queue graphicsQueueHandle() const;
  std::uint32_t graphicsQueueFamilyIndex() const;
  std::uint32_t presentQueueFamilyIndex() const;

  template <typename Handle>
  void nameObject(Handle handle, char const *name) const {
    auto raw = static_cast<typename Handle::CType>(handle);
    std::uint64_t value;
    if constexpr (std::is_pointer_v<decltype(raw)>)
      value = reinterpret_cast<std::uint64_t>(raw);
    else
      value = static_cast<std::uint64_t>(raw);
    setObjectName(Handle::objectType, value, name);
  }
  void beginLabel(vk::CommandBuffer command, char const *name) const;
  void endLabel(vk::CommandBuffer command) const;
  PresentationSupport const &presentationSupport() const { return presentationSupport_; }
  bool memoryBudgetSupported() const;
  bool rayTracingSupported() const { return rayTracingSupported_; }
  bool timelineSemaphoreSupported() const { return timelineSemaphoreSupported_; }
  float maxSamplerAnisotropy() const { return maxSamplerAnisotropy_; }
  std::pair<std::uint64_t, std::uint64_t> memoryUsageBudget() const;

private:
  void setObjectName(vk::ObjectType type, std::uint64_t handle,
                     char const *name) const;
  QueueFamilyIndices
  findQueueFamilies(vk::raii::PhysicalDevice const &physicalDevice) const;
  bool supportsRequiredExtensions(
      vk::raii::PhysicalDevice const &physicalDevice) const;
  bool isDeviceSuitable(vk::raii::PhysicalDevice const &physicalDevice) const;
  void pickPhysicalDevice();
  void createLogicalDevice();

private:
  vk::raii::Instance const &instance_;
  vk::raii::SurfaceKHR const &surface_;
  std::vector<char const *> requiredDeviceExtensions_;

  ResourceLedger resourceLedger_;
  vk::raii::PhysicalDevice physicalDevice_ = nullptr;
  vk::raii::Device device_ = nullptr;
  vk::raii::Queue graphicsQueue_ = nullptr;
  vk::raii::Queue presentQueue_ = nullptr;
  QueueFamilyIndices queueFamilyIndices_{};
  std::string preferredGpu_;
  bool debugUtils_ = false;
  bool timelineSemaphoreSupported_ = false;
  bool rayTracingSupported_ = false;
  bool allowRayTracing_ = true;
  float maxSamplerAnisotropy_ = 1.0f;
  PresentationInstanceSupport presentationInstance_;
  PresentationPolicy presentationPolicy_;
  PresentationSupport presentationSupport_;
  PFN_vkSetDebugUtilsObjectNameEXT setName_ = nullptr;
  PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel_ = nullptr;
  PFN_vkCmdEndDebugUtilsLabelEXT endLabel_ = nullptr;
  GpuAllocator gpuAllocator_;
};
