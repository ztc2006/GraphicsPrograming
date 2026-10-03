#pragma once

#include <string>
#include <vector>

#include "device.hpp"

struct GLFWwindow;

class SwapChain {
public:
  SwapChain(Device const &device, vk::raii::SurfaceKHR const &surface,
            GLFWwindow *window, vk::SwapchainKHR oldSwapChain = nullptr,
            std::string requestedPresent = "auto");

  vk::raii::SwapchainKHR const &handle() const;
  vk::Format imageFormat() const;
  vk::Extent2D extent() const;
  vk::PresentModeKHR presentMode() const { return presentMode_; }
  std::vector<vk::Image> const &images() const;
  std::vector<vk::raii::ImageView> const &imageViews() const;

private:
  static std::uint32_t chooseSwapMinImageCount(
      vk::SurfaceCapabilitiesKHR const &surfaceCapabilities);
  static vk::SurfaceFormatKHR chooseSwapSurfaceFormat(
      std::vector<vk::SurfaceFormatKHR> const &availableFormats);
  static vk::PresentModeKHR chooseSwapPresentMode(
      std::vector<vk::PresentModeKHR> const &availablePresentModes,
      std::string const &requested);
  vk::Extent2D
  chooseSwapExtent(vk::SurfaceCapabilitiesKHR const &capabilities) const;
  void createSwapChain();
  void createImageViews();

private:
  Device const &device_;
  vk::raii::SurfaceKHR const &surface_;
  GLFWwindow *window_ = nullptr;
  vk::SwapchainKHR oldSwapChain_ = nullptr;

  vk::raii::SwapchainKHR swapChain_ = nullptr;
  std::vector<vk::Image> images_;
  std::vector<vk::raii::ImageView> imageViews_;
  vk::SurfaceFormatKHR surfaceFormat_{};
  vk::Extent2D extent_{};
  std::string requestedPresent_;
  vk::PresentModeKHR presentMode_{};
};
