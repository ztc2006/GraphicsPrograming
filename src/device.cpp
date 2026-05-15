#include "device.hpp"

#include <cstring>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>

Device::Device(vk::raii::Instance const &instance,
               vk::raii::SurfaceKHR const &surface,
               std::vector<char const *> requiredDeviceExtensions)
    : instance_(instance), surface_(surface),
      requiredDeviceExtensions_(std::move(requiredDeviceExtensions)) {
  pickPhysicalDevice();
  createLogicalDevice();
}

vk::raii::Device const &Device::logicalDevice() const { return device_; }

vk::raii::PhysicalDevice const &Device::physicalDevice() const {
  return physicalDevice_;
}

vk::raii::Queue const &Device::graphicsQueue() const { return graphicsQueue_; }

vk::raii::Queue const &Device::presentQueue() const { return presentQueue_; }

std::uint32_t Device::graphicsQueueFamilyIndex() const {
  return queueFamilyIndices_.graphics;
}

std::uint32_t Device::presentQueueFamilyIndex() const {
  return queueFamilyIndices_.present;
}

std::uint32_t Device::findMemoryType(std::uint32_t typeFilter,
                                     vk::MemoryPropertyFlags properties) const {
  auto memoryProperties = physicalDevice_.getMemoryProperties();

  for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount;
       ++index) {
    const bool supportsType = (typeFilter & (1u << index)) != 0;
    const bool supportsProperties =
        (memoryProperties.memoryTypes[index].propertyFlags & properties) ==
        properties;

    if (supportsType && supportsProperties) {
      return index;
    }
  }
  throw std::runtime_error("Failed to find suitable buffer memory type.");
}

std::pair<vk::raii::Buffer, vk::raii::DeviceMemory>
Device::createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                     vk::MemoryPropertyFlags properties) const {
  vk::BufferCreateInfo bufferCreateInfo{
      .size = size,
      .usage = usage,
      .sharingMode = vk::SharingMode::eExclusive,
  };
  vk::raii::Buffer buffer(device_, bufferCreateInfo);

  auto memoryRequirements = buffer.getMemoryRequirements();
  vk::MemoryAllocateInfo allocatioInfo{
      .allocationSize = memoryRequirements.size,
      .memoryTypeIndex =
          findMemoryType(memoryRequirements.memoryTypeBits, properties),
  };
  vk::raii::DeviceMemory bufferMemory(device_, allocatioInfo);

  buffer.bindMemory(*bufferMemory, 0);
  return {std::move(buffer), std::move(bufferMemory)};
}

void Device::copyBuffer(vk::Buffer sourceBuffer, vk::Buffer destinationBuffer,
                        vk::DeviceSize size) const {
  vk::CommandPoolCreateInfo commandCreatePoolInfo{
      .flags = vk::CommandPoolCreateFlagBits::eTransient,
      .queueFamilyIndex = graphicsQueueFamilyIndex(),
  };
  vk::raii::CommandPool commandPool(device_, commandCreatePoolInfo);

  vk::CommandBufferAllocateInfo allocateInfo{
      .commandPool = *commandPool,
      .level = vk::CommandBufferLevel::ePrimary,
      .commandBufferCount = 1,
  };
  vk::raii::CommandBuffers commandBuffers(device_, allocateInfo);
  auto const &commandBuffer = commandBuffers.front();

  commandBuffer.begin(vk::CommandBufferBeginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
  });

  vk::BufferCopy copyRegion{
      .size = size,
  };

  commandBuffer.copyBuffer(sourceBuffer, destinationBuffer, {copyRegion});
  commandBuffer.end();

  vk::CommandBuffer rawCommandBuffer = *commandBuffer;
  vk::SubmitInfo submitInfo{
      .commandBufferCount = 1,
      .pCommandBuffers = &rawCommandBuffer,
  };

  graphicsQueue_.submit({submitInfo}, nullptr);
  graphicsQueue_.waitIdle();
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
          .synchronization2;

  return supportsRequiredFeatures;
}

void Device::pickPhysicalDevice() {
  auto physicalDevices = instance_.enumeratePhysicalDevices();
  auto deviceIt =
      std::ranges::find_if(physicalDevices, [&](auto const &physicalDevice) {
        return isDeviceSuitable(physicalDevice);
      });

  if (deviceIt == physicalDevices.end()) {
    throw std::runtime_error("Failed to find a suitable GPU.");
  }

  physicalDevice_ = *deviceIt;
}

void Device::createLogicalDevice() {
  queueFamilyIndices_ = findQueueFamilies(physicalDevice_);
  if (!queueFamilyIndices_.isComplete()) {
    throw std::runtime_error(
        "Could not find queue families for graphics and present.");
  }

  vk::StructureChain<vk::PhysicalDeviceFeatures2,
                     vk::PhysicalDeviceVulkan13Features>
      featureChain = {
          {},
          {
              .synchronization2 = true,
              .dynamicRendering = true,
          },
      };

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
