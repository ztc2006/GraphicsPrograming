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

Device::QueueFamilyIndices
Device::findQueueFamilies(vk::raii::PhysicalDevice const &physicalDevice) const {
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
