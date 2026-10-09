#include "pch.hpp"

#include "device.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>

Device::Device(vk::raii::Instance const &instance,
               vk::raii::SurfaceKHR const &surface,
               std::vector<char const *> requiredDeviceExtensions,
               std::string preferredGpu, bool debugUtils,
               PresentationInstanceSupport presentationInstance, PresentationPolicy presentationPolicy, bool allowRayTracing)
    : instance_(instance), surface_(surface),
      requiredDeviceExtensions_(std::move(requiredDeviceExtensions)),
      preferredGpu_(std::move(preferredGpu)), debugUtils_(debugUtils), allowRayTracing_(allowRayTracing),
      presentationInstance_(presentationInstance), presentationPolicy_(presentationPolicy), gpuAllocator_(*this) {
  pickPhysicalDevice();
  createLogicalDevice();
  if (debugUtils_) {
    auto raw = static_cast<VkDevice>(*device_);
    setName_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(raw, "vkSetDebugUtilsObjectNameEXT"));
    beginLabel_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(raw, "vkCmdBeginDebugUtilsLabelEXT"));
    endLabel_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(raw, "vkCmdEndDebugUtilsLabelEXT"));
  }
}

vk::raii::Device const &Device::logicalDevice() const { return device_; }

vk::raii::PhysicalDevice const &Device::physicalDevice() const {
  return physicalDevice_;
}

vk::raii::Queue const &Device::graphicsQueue() const { return graphicsQueue_; }

vk::raii::Queue const &Device::presentQueue() const { return presentQueue_; }

vk::Instance Device::instanceHandle() const { return *instance_; }

vk::PhysicalDevice Device::physicalDeviceHandle() const {
  return *physicalDevice_;
}

vk::Device Device::deviceHandle() const { return *device_; }

vk::Queue Device::graphicsQueueHandle() const { return *graphicsQueue_; }

std::uint32_t Device::graphicsQueueFamilyIndex() const {
  return queueFamilyIndices_.graphics;
}

std::uint32_t Device::presentQueueFamilyIndex() const {
  return queueFamilyIndices_.present;
}

Device::BufferResources
Device::createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                     vk::MemoryPropertyFlags properties,
                     ResourceLedger::Scope scope) const {
  return gpuAllocator_.createBuffer(size, usage, properties, std::move(scope));
}

Device::BufferResources Device::createUploadBuffer(vk::DeviceSize size) const {
  return gpuAllocator_.createBuffer(size, vk::BufferUsageFlagBits::eTransferSrc,
      vk::MemoryPropertyFlagBits::eHostVisible,
      resourceLedger_.scope(ResourceLedger::Domain::Staging), GpuAllocator::Lifetime::Upload);
}

GpuImage Device::createImage(vk::ImageCreateInfo const &description,
                              vk::DeviceSize payloadBytes,
                              vk::MemoryPropertyFlags properties,
                              ResourceLedger::Scope scope) const {
  return gpuAllocator_.createImage(description, payloadBytes, properties, std::move(scope));
}

Device::QueueFamilyIndices Device::findQueueFamilies(
    vk::raii::PhysicalDevice const &physicalDevice) const {
  QueueFamilyIndices indices{};

  auto queueFamilies = physicalDevice.getQueueFamilyProperties();
  for (std::uint32_t index = 0; index < queueFamilies.size(); ++index) {
    if (queueFamilies[index].queueFlags & vk::QueueFlagBits::eGraphics) {
      indices.graphics = index;
    }

    if (physicalDevice.getSurfaceSupportKHR(index, *surface_)) {
      indices.present = index;
    }

    if (indices.isComplete()) {
      break;
    }
  }

  return indices;
}

bool Device::supportsRequiredExtensions(
    vk::raii::PhysicalDevice const &physicalDevice) const {
  auto availableDeviceExtensions =
      physicalDevice.enumerateDeviceExtensionProperties();

  return std::ranges::all_of(
      requiredDeviceExtensions_,
      [&availableDeviceExtensions](auto const &requiredExtension) {
        return std::ranges::any_of(
            availableDeviceExtensions,
            [requiredExtension](auto const &availableExtension) {
              return std::strcmp(availableExtension.extensionName,
                                 requiredExtension) == 0;
            });
      });
}

bool Device::isDeviceSuitable(
    vk::raii::PhysicalDevice const &physicalDevice) const {
  auto indices = findQueueFamilies(physicalDevice);
  if (!indices.isComplete()) {
    return false;
  }

  const bool supportsVulkan13 =
      physicalDevice.getProperties().apiVersion >= VK_API_VERSION_1_3;
  if (!supportsVulkan13 || !supportsRequiredExtensions(physicalDevice)) {
    return false;
  }

  auto availableFormats = physicalDevice.getSurfaceFormatsKHR(*surface_);
  auto availablePresentModes =
      physicalDevice.getSurfacePresentModesKHR(*surface_);
  if (availableFormats.empty() || availablePresentModes.empty()) {
    return false;
  }

  auto features = physicalDevice.template getFeatures2<
      vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features>();

  const bool supportsRequiredFeatures =
      features.template get<vk::PhysicalDeviceVulkan13Features>()
          .dynamicRendering &&
      features.template get<vk::PhysicalDeviceVulkan13Features>()
          .synchronization2 &&
      features.template get<vk::PhysicalDeviceFeatures2>().features.imageCubeArray;

  return supportsRequiredFeatures;
}

void Device::pickPhysicalDevice() {
  auto physicalDevices = instance_.enumeratePhysicalDevices();
  auto lower = [](std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return s;
  };
  auto matches = [&](auto const &gpu) {
    return isDeviceSuitable(gpu) &&
           (preferredGpu_.empty() ||
            lower(gpu.getProperties().deviceName.data())
                    .find(lower(preferredGpu_)) != std::string::npos);
  };
  auto deviceIt = std::ranges::find_if(physicalDevices, [&](auto const &gpu) {
    return matches(gpu) && gpu.getProperties().deviceType ==
                               vk::PhysicalDeviceType::eDiscreteGpu;
  });
  if (deviceIt == physicalDevices.end())
    deviceIt = std::ranges::find_if(physicalDevices, matches);

  if (deviceIt == physicalDevices.end()) {
    throw std::runtime_error(
        "Failed to find a suitable Vulkan 1.3 GPU matching: " + preferredGpu_);
  }

  physicalDevice_ = *deviceIt;
}

void Device::createLogicalDevice() {
  queueFamilyIndices_ = findQueueFamilies(physicalDevice_);
  if (!queueFamilyIndices_.isComplete()) {
    throw std::runtime_error(
        "Could not find queue families for graphics and present.");
  }

  auto supported = physicalDevice_.getFeatures2<vk::PhysicalDeviceFeatures2,
      vk::PhysicalDeviceVulkan12Features>();
  timelineSemaphoreSupported_ = supported.get<vk::PhysicalDeviceVulkan12Features>().timelineSemaphore;
  if (!supported.get<vk::PhysicalDeviceFeatures2>().features.imageCubeArray)
    throw std::runtime_error("Local environment probes require imageCubeArray");
  bool anisotropy =
      supported.get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy;
  maxSamplerAnisotropy_ =
      anisotropy ? physicalDevice_.getProperties().limits.maxSamplerAnisotropy
                 : 1.0f;
  auto extensions = physicalDevice_.enumerateDeviceExtensionProperties();
  auto has = [&](char const *name) {
    return std::ranges::any_of(extensions, [&](auto const &e) { return std::strcmp(e.extensionName, name) == 0; });
  };
  if (allowRayTracing_ && has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
      has(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
      has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
    auto rt = physicalDevice_.getFeatures2<vk::PhysicalDeviceFeatures2,
        vk::PhysicalDeviceVulkan12Features,
        vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
        vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>();
    auto const &v12 = rt.get<vk::PhysicalDeviceVulkan12Features>();
    rayTracingSupported_ = rt.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructure &&
        rt.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipeline &&
        v12.bufferDeviceAddress && v12.scalarBlockLayout && v12.runtimeDescriptorArray &&
        v12.shaderSampledImageArrayNonUniformIndexing &&
        rt.get<vk::PhysicalDeviceFeatures2>().features.shaderInt64;
    if (rayTracingSupported_)
      for (auto name : {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME})
        if (std::ranges::none_of(requiredDeviceExtensions_, [&](auto p) { return std::strcmp(p, name) == 0; }))
          requiredDeviceExtensions_.push_back(name);
  }
  PresentationDeviceSupport present{has(vk::KHRSwapchainMaintenance1ExtensionName),
                                    has(vk::EXTSwapchainMaintenance1ExtensionName), false};
  if (presentationPolicy_ != PresentationPolicy::Legacy &&
      ((presentationInstance_.khr && present.khr) || (presentationInstance_.ext && present.ext))) {
    auto features = physicalDevice_.getFeatures2<vk::PhysicalDeviceFeatures2,
        vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR>();
    present.feature = features.get<vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR>().swapchainMaintenance1;
  }
  presentationSupport_ = choosePresentationSupport(presentationInstance_, present, presentationPolicy_);
  if (presentationSupport_.fencesEnabled()) {
    auto name = presentationSupport_.backend == PresentationBackend::KhrFence
        ? vk::KHRSwapchainMaintenance1ExtensionName : vk::EXTSwapchainMaintenance1ExtensionName;
    if (std::ranges::none_of(requiredDeviceExtensions_, [&](auto p) { return std::strcmp(p, name) == 0; }))
      requiredDeviceExtensions_.push_back(name);
  }
  vk::StructureChain<vk::PhysicalDeviceFeatures2,
                     vk::PhysicalDeviceVulkan12Features,
                     vk::PhysicalDeviceVulkan13Features,
                     vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR,
                     vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                     vk::PhysicalDeviceRayTracingPipelineFeaturesKHR> featureChain = {
      {}, {.timelineSemaphore = timelineSemaphoreSupported_},
      {.synchronization2 = true, .dynamicRendering = true},
      {.swapchainMaintenance1 = presentationSupport_.fencesEnabled()}, {}, {}};
  if (!presentationSupport_.fencesEnabled())
    featureChain.unlink<vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR>();
  featureChain.get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy =
      anisotropy;

  featureChain.get<vk::PhysicalDeviceFeatures2>().features.imageCubeArray = true;

  if (rayTracingSupported_) {
    auto &v12 = featureChain.get<vk::PhysicalDeviceVulkan12Features>();
    v12.bufferDeviceAddress = true;
    v12.scalarBlockLayout = true;
    v12.runtimeDescriptorArray = true;
    v12.shaderSampledImageArrayNonUniformIndexing = true;
    featureChain.get<vk::PhysicalDeviceFeatures2>().features.shaderInt64 = true;
    featureChain.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructure = true;
    featureChain.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipeline = true;
  } else {
    featureChain.unlink<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>();
    featureChain.unlink<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>();
  }
  float queuePriority = 1.0f;
  std::set<std::uint32_t> uniqueQueueFamilies = {
      queueFamilyIndices_.graphics,
      queueFamilyIndices_.present,
  };

  std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
  queueCreateInfos.reserve(uniqueQueueFamilies.size());
  for (auto queueFamilyIndex : uniqueQueueFamilies) {
    queueCreateInfos.push_back(vk::DeviceQueueCreateInfo{
        .queueFamilyIndex = queueFamilyIndex,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    });
  }

  vk::DeviceCreateInfo deviceCreateInfo{
      .pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
      .queueCreateInfoCount =
          static_cast<std::uint32_t>(queueCreateInfos.size()),
      .pQueueCreateInfos = queueCreateInfos.data(),
      .enabledExtensionCount =
          static_cast<std::uint32_t>(requiredDeviceExtensions_.size()),
      .ppEnabledExtensionNames = requiredDeviceExtensions_.data(),
  };

  device_ = vk::raii::Device(physicalDevice_, deviceCreateInfo);
  graphicsQueue_ = vk::raii::Queue(device_, queueFamilyIndices_.graphics, 0);
  presentQueue_ = vk::raii::Queue(device_, queueFamilyIndices_.present, 0);
}

void Device::setObjectName(vk::ObjectType type, std::uint64_t handle,
                           char const *name) const {
  if (!setName_)
    return;
  VkDebugUtilsObjectNameInfoEXT info{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
  info.objectType = static_cast<VkObjectType>(type);
  info.objectHandle = handle;
  info.pObjectName = name;
  (void)setName_(static_cast<VkDevice>(*device_), &info);
}
void Device::beginLabel(vk::CommandBuffer command, char const *name) const {
  if (!beginLabel_)
    return;
  VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
  label.pLabelName = name;
  label.color[0] = 0.2f;
  label.color[1] = 0.6f;
  label.color[2] = 0.9f;
  label.color[3] = 1;
  beginLabel_(static_cast<VkCommandBuffer>(command), &label);
}
void Device::endLabel(vk::CommandBuffer command) const {
  if (endLabel_)
    endLabel_(static_cast<VkCommandBuffer>(command));
}
bool Device::memoryBudgetSupported() const {
  auto extensions = physicalDevice_.enumerateDeviceExtensionProperties();
  return std::ranges::any_of(extensions, [](auto const &e) {
    return std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) ==
           0;
  });
}
std::pair<std::uint64_t, std::uint64_t> Device::memoryUsageBudget() const {
  if (!memoryBudgetSupported())
    return {};
  auto chain =
      physicalDevice_
          .getMemoryProperties2<vk::PhysicalDeviceMemoryProperties2,
                                vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
  auto const &memory =
      chain.get<vk::PhysicalDeviceMemoryProperties2>().memoryProperties;
  auto const &budget = chain.get<vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
  std::uint64_t usage = 0, available = 0;
  for (unsigned i = 0; i < memory.memoryHeapCount; ++i) {
    if (memory.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
      usage += budget.heapUsage[i];
      available += budget.heapBudget[i];
    }
  }
  return {usage, available};
}
