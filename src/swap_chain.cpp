#include "pch.hpp"

#include "swap_chain.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <iostream>

#include <GLFW/glfw3.h>

SwapChain::SwapChain(Device const &device, vk::raii::SurfaceKHR const &surface,
                     GLFWwindow *window, vk::SwapchainKHR oldSwapChain,
                     std::string requestedPresent)
    : device_(device), surface_(surface), window_(window),
      oldSwapChain_(oldSwapChain),
      requestedPresent_(std::move(requestedPresent)) {
  createSwapChain();
  createImageViews();
  createPresentationResources();
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
  for (auto desired : {vk::Format::eB8G8R8A8Srgb, vk::Format::eR8G8B8A8Srgb,
                       vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm}) {
    auto found = std::ranges::find_if(availableFormats, [&](auto const &format) {
      return format.format == desired && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
    });
    if (found != availableFormats.end()) return *found;
  }
  throw std::runtime_error("No supported 8-bit SDR sRGB-nonlinear surface format");

}

vk::PresentModeKHR SwapChain::chooseSwapPresentMode(
    std::vector<vk::PresentModeKHR> const &availablePresentModes,
    std::string const &requested) {
  if (requested != "auto") {
    auto mode = requested == "fifo"      ? vk::PresentModeKHR::eFifo
                : requested == "mailbox" ? vk::PresentModeKHR::eMailbox
                                         : vk::PresentModeKHR::eImmediate;
    if (std::ranges::find(availablePresentModes, mode) ==
        availablePresentModes.end())
      throw std::runtime_error("Requested present mode is unsupported: " +
                               requested);
    return mode;
  }
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
  presentMode_ =
      chooseSwapPresentMode(availablePresentModes, requestedPresent_);

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
      .presentMode = presentMode_,
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


void SwapChain::createPresentationResources() {
  presentation_.reserve(images_.size());
  for (std::size_t i = 0; i < images_.size(); ++i) {
    PresentationResources resources;
    resources.finished = vk::raii::Semaphore(device_.logicalDevice(), vk::SemaphoreCreateInfo{});
    if (device_.presentationSupport().fencesEnabled())
      resources.fence = vk::raii::Fence(device_.logicalDevice(), vk::FenceCreateInfo{});
    device_.nameObject(*resources.finished, ("Swapchain present wait " + std::to_string(i)).c_str());
    if (*resources.fence) device_.nameObject(*resources.fence, ("Swapchain present release " + std::to_string(i)).c_str());
    presentation_.push_back(std::move(resources));
  }
}
SwapChain::~SwapChain() {
  if (!presentationDirty_) return;
  try { drainPresentations(); }
  catch (std::exception const &error) { std::cerr << "Swapchain teardown drain: " << error.what() << '\n'; }
}
vk::Semaphore SwapChain::renderFinishedSemaphore(std::uint32_t imageIndex) const {
  presentationDirty_ = true;
  return *presentation_.at(imageIndex).finished;
}
void SwapChain::waitPresentation(std::uint32_t imageIndex) const {
  auto &record = presentation_.at(imageIndex);
  if (!record.pending) return;
  ++presentationStatistics_.fenceWaits;
  try {
    auto result = device_.logicalDevice().waitForFences({*record.fence}, true, UINT64_MAX);
    if (result != vk::Result::eSuccess) throw std::runtime_error("Presentation fence wait did not complete");
  } catch (vk::DeviceLostError const &) { deviceLost_ = true; throw; }
  record.pending = false;
  ++presentationStatistics_.completed;
}
vk::Result SwapChain::present(std::uint32_t imageIndex) const {
  auto &record = presentation_.at(imageIndex);
  waitPresentation(imageIndex);
  vk::Fence fence = *record.fence;
  if (fence) device_.logicalDevice().resetFences({fence});
  vk::SwapchainPresentFenceInfoKHR presentFence{.swapchainCount = 1, .pFences = &fence};
  vk::Semaphore semaphore = *record.finished;
  vk::SwapchainKHR swapchain = *swapChain_;
  vk::PresentInfoKHR info{.pNext = fence ? &presentFence : nullptr,
      .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore,
      .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &imageIndex};
  // Raw result preserves bookkeeping even for enqueued WSI errors which Vulkan-Hpp throws.
  auto result = static_cast<vk::Result>(vkQueuePresentKHR(
      static_cast<VkQueue>(*device_.presentQueue()), reinterpret_cast<VkPresentInfoKHR const *>(&info)));
  recordPresentResult(imageIndex, result);
  return result;
}
void SwapChain::recordPresentResult(std::uint32_t imageIndex, vk::Result result) const {
  auto &record = presentation_.at(imageIndex);
  presentationDirty_ = true;
  switch (classifyPresentResult(result)) {
  case PresentEnqueueState::Enqueued:
    record.pending = bool(*record.fence); ++presentationStatistics_.queued; break;
  case PresentEnqueueState::Rejected: break; // OOM leaves the unsignaled fence unsubmitted.
  case PresentEnqueueState::DeviceLost: deviceLost_ = true; break;
  case PresentEnqueueState::Unknown: uncertainPresent_ = true; break;
  }
}
void SwapChain::collectPresentationCompletions() const {
  if (deviceLost_) return;
  try {
  for (auto &record : presentation_) {
    if (record.pending && record.fence.getStatus() == vk::Result::eSuccess) {
      record.pending = false; ++presentationStatistics_.completed;
    }
  }
  } catch (vk::DeviceLostError const &) { deviceLost_ = true; throw; }
}
void SwapChain::drainPresentations() const {
  if (deviceLost_) return; // Lost device teardown must not wait an unsignalable fence.
  try { device_.logicalDevice().waitIdle(); }
  catch (vk::DeviceLostError const &) { deviceLost_ = true; throw; }
  for (std::uint32_t i = 0; i < presentation_.size(); ++i) waitPresentation(i);
  if (presentationDirty_ && (!device_.presentationSupport().fencesEnabled() || uncertainPresent_)) {
    ++presentationStatistics_.legacyDrains;
    std::cerr << "Presentation drain: legacy WaitIdle fallback; presentation resource release is unproven\n";
  }
  presentationDirty_ = false;
}
bool SwapChain::presentationReleaseProven() const {
  return device_.presentationSupport().fencesEnabled() && !presentationDirty_ && !uncertainPresent_ && !deviceLost_ &&
      std::none_of(presentation_.begin(), presentation_.end(), [](auto const &record) { return record.pending; });
}
SwapChain::PresentationStatistics SwapChain::presentationStatistics() const {
  auto statistics = presentationStatistics_;
  statistics.pendingFences = std::count_if(presentation_.begin(), presentation_.end(),
                                         [](auto const &record) { return record.pending; });
  return statistics;
}
