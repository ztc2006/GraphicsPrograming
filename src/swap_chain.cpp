#include "swap_chain.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>

#include <GLFW/glfw3.h>

SwapChain::SwapChain(Device const &device, vk::raii::SurfaceKHR const &surface,
                     GLFWwindow *window, vk::SwapchainKHR oldSwapChain)
    : device_(device), surface_(surface), window_(window),
      oldSwapChain_(oldSwapChain) {
  createSwapChain();
  createImageViews();
}

vk::raii::SwapchainKHR const &SwapChain::handle() const { return swapChain_; }

vk::Format SwapChain::imageFormat() const { return surfaceFormat_.format; }

vk::Extent2D SwapChain::extent() const { return extent_; }

std::vector<vk::Image> const &SwapChain::images() const { return images_; }

std::vector<vk::raii::ImageView> const &SwapChain::imageViews() const {
  return imageViews_;
}

std::uint32_t SwapChain::chooseSwapMinImageCount(
    vk::SurfaceCapabilitiesKHR const &surfaceCapabilities) {
  auto minImageCount = std::max(3u, surfaceCapabilities.minImageCount);
  if ((0 < surfaceCapabilities.maxImageCount) &&
      (surfaceCapabilities.maxImageCount < minImageCount)) {
    minImageCount = surfaceCapabilities.maxImageCount;
  }

  return minImageCount;
}

vk::SurfaceFormatKHR SwapChain::chooseSwapSurfaceFormat(
    std::vector<vk::SurfaceFormatKHR> const &availableFormats) {
  assert(!availableFormats.empty());
  auto formatIt =
      std::ranges::find_if(availableFormats, [](auto const &format) {
        return format.format == vk::Format::eB8G8R8A8Srgb &&
               format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
      });

  return formatIt != availableFormats.end() ? *formatIt : availableFormats[0];
}

vk::PresentModeKHR SwapChain::chooseSwapPresentMode(
    std::vector<vk::PresentModeKHR> const &availablePresentModes) {
  assert(std::ranges::any_of(availablePresentModes, [](auto presentMode) {
    return presentMode == vk::PresentModeKHR::eFifo;
  }));

  return std::ranges::any_of(availablePresentModes,
                             [](vk::PresentModeKHR presentMode) {
                               return presentMode ==
                                      vk::PresentModeKHR::eMailbox;
                             })
             ? vk::PresentModeKHR::eMailbox
             : vk::PresentModeKHR::eFifo;
}

vk::Extent2D SwapChain::chooseSwapExtent(
    vk::SurfaceCapabilitiesKHR const &capabilities) const {
  if (capabilities.currentExtent.width !=
      std::numeric_limits<std::uint32_t>::max()) {
    return capabilities.currentExtent;
  }

  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);

  return {
      std::clamp<std::uint32_t>(width, capabilities.minImageExtent.width,
                                capabilities.maxImageExtent.width),
      std::clamp<std::uint32_t>(height, capabilities.minImageExtent.height,
                                capabilities.maxImageExtent.height),
  };
}

void SwapChain::createSwapChain() {
  auto surfaceCapabilities =
      device_.physicalDevice().getSurfaceCapabilitiesKHR(*surface_);
  extent_ = chooseSwapExtent(surfaceCapabilities);
  auto minImageCount = chooseSwapMinImageCount(surfaceCapabilities);

  auto availableFormats =
      device_.physicalDevice().getSurfaceFormatsKHR(*surface_);
  surfaceFormat_ = chooseSwapSurfaceFormat(availableFormats);

  auto availablePresentModes =
      device_.physicalDevice().getSurfacePresentModesKHR(*surface_);
  auto presentMode = chooseSwapPresentMode(availablePresentModes);

  std::array<std::uint32_t, 2> queueFamilyIndices = {
      device_.graphicsQueueFamilyIndex(),
      device_.presentQueueFamilyIndex(),
  };
  const bool separateQueues = queueFamilyIndices[0] != queueFamilyIndices[1];

  vk::SwapchainCreateInfoKHR swapChainCreateInfo{
      .surface = *surface_,
      .minImageCount = minImageCount,
      .imageFormat = surfaceFormat_.format,
      .imageColorSpace = surfaceFormat_.colorSpace,
      .imageExtent = extent_,
      .imageArrayLayers = 1,
      .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
      .imageSharingMode = separateQueues ? vk::SharingMode::eConcurrent
                                         : vk::SharingMode::eExclusive,
      .queueFamilyIndexCount = separateQueues ? 2u : 0u,
      .pQueueFamilyIndices =
          separateQueues ? queueFamilyIndices.data() : nullptr,
      .preTransform = surfaceCapabilities.currentTransform,
      .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
      .presentMode = presentMode,
      .clipped = true,
      .oldSwapchain = oldSwapChain_,
  };

  swapChain_ =
      vk::raii::SwapchainKHR(device_.logicalDevice(), swapChainCreateInfo);
  images_ = swapChain_.getImages();
}

void SwapChain::createImageViews() {
  imageViews_.clear();
  imageViews_.reserve(images_.size());

  for (vk::Image image : images_) {
    vk::ImageViewCreateInfo createInfo{
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = surfaceFormat_.format,
        .subresourceRange =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };

    imageViews_.emplace_back(device_.logicalDevice(), createInfo);
  }
}
