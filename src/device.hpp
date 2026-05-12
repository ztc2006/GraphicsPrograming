#pragma once

#include <cstdint>
#include <vector>

#include "vulkan_include.hpp"

class Device {
public:
  struct QueueFamilyIndices {
    std::uint32_t graphics = ~0u;
    std::uint32_t present = ~0u;

    bool isComplete() const { return graphics != ~0u && present != ~0u; }
  };

  Device(vk::raii::Instance const &instance,
         vk::raii::SurfaceKHR const &surface,
         std::vector<char const *> requiredDeviceExtensions);

  vk::raii::Device const &logicalDevice() const;
  vk::raii::PhysicalDevice const &physicalDevice() const;
  vk::raii::Queue const &graphicsQueue() const;
  vk::raii::Queue const &presentQueue() const;
  std::uint32_t graphicsQueueFamilyIndex() const;
  std::uint32_t presentQueueFamilyIndex() const;

private:
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

  vk::raii::PhysicalDevice physicalDevice_ = nullptr;
  vk::raii::Device device_ = nullptr;
  vk::raii::Queue graphicsQueue_ = nullptr;
  vk::raii::Queue presentQueue_ = nullptr;
  QueueFamilyIndices queueFamilyIndices_{};
};
