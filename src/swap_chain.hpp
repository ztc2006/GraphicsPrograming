#pragma once

#include <string>
#include <vector>

#include "device.hpp"

struct GLFWwindow;

class SwapChain {
  friend struct SwapChainPresentationTestAccess;
public:
  SwapChain(Device const &device, vk::raii::SurfaceKHR const &surface,
            GLFWwindow *window, vk::SwapchainKHR oldSwapChain = nullptr,
            std::string requestedPresent = "auto");

  ~SwapChain();
  SwapChain(SwapChain const &) = delete;
  SwapChain &operator=(SwapChain const &) = delete;
  // Presentation resources belong to this swapchain generation. Borrow only
  // after acquire; the frame submit waits on its acquire semaphore.
  vk::Semaphore renderFinishedSemaphore(std::uint32_t imageIndex) const;
  vk::Result present(std::uint32_t imageIndex) const;
  void collectPresentationCompletions() const;
  // Blocking boundary for resize/exit: drains GPU submissions AND all known
  // presentation fences. Legacy has an explicit, unproven idle fallback.
  void drainPresentations() const;
  bool presentationReleaseProven() const;
  bool presentationDeviceLost() const { return deviceLost_; }
  struct PresentationStatistics {
    std::uint64_t queued = 0, completed = 0, fenceWaits = 0, legacyDrains = 0;
    std::size_t pendingFences = 0;
  };
  PresentationStatistics presentationStatistics() const;
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
  void createPresentationResources();
  void waitPresentation(std::uint32_t imageIndex) const;
  void recordPresentResult(std::uint32_t imageIndex, vk::Result result) const;

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
  struct PresentationResources {
    vk::raii::Semaphore finished = nullptr;
    vk::raii::Fence fence = nullptr;
    bool pending = false;
  };
  mutable std::vector<PresentationResources> presentation_;
  mutable PresentationStatistics presentationStatistics_;
  mutable bool presentationDirty_ = false, uncertainPresent_ = false, deviceLost_ = false;
};
