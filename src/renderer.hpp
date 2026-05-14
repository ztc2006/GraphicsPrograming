#pragma once

#include <cstdint>
#include <vector>

#include "device.hpp"
#include "swap_chain.hpp"

class Renderer {
public:
  enum class FrameResult {
    eSuccess,
    eSwapChainOutOfDate,
    eSwapChainSuboptimal,
  };

  explicit Renderer(Device const &device);

  FrameResult drawFrame();
  void recreateForSwapChain(SwapChain const &swapChain);

private:
  struct FrameContext {
    vk::raii::Semaphore imageAvailableSemaphore = nullptr;
    vk::raii::Fence inFlightFence = nullptr;
  };

  static constexpr std::uint32_t kFramesInFlight = 1;

  static std::vector<char> readBinaryFile(char const *path);

  void createPersistentResources();
  void createFrameResources();
  void createCommandBuffers();
  void createCommandPool();
  void validateSwapChainCandidate(SwapChain const &swapChain) const;
  void validateSwapChainState() const;
  vk::raii::Pipeline createGraphicsPipeline(
      SwapChain const &swapChain,
      vk::raii::PipelineLayout const &pipelineLayout) const;

  void recordCommandBuffer(vk::raii::CommandBuffer const &commandBuffer,
                           std::uint32_t imageIndex);
  void transitionSwapChainImage(vk::raii::CommandBuffer const &commandBuffer,
                                std::uint32_t imageIndex,
                                vk::ImageLayout newLayout,
                                vk::PipelineStageFlags2 srcStageMask,
                                vk::AccessFlags2 srcAccessMask,
                                vk::PipelineStageFlags2 dstStageMask,
                                vk::AccessFlags2 dstAccessMask);

private:
  Device const &device_;
  SwapChain const *swapChain_ = nullptr;

  vk::raii::CommandPool commandPool_ = nullptr;
  std::vector<FrameContext> frames_;
  vk::raii::CommandBuffers commandBuffers_ = nullptr;
  std::uint32_t currentFrame_ = 0;

  vk::raii::PipelineLayout pipelineLayout_ = nullptr;
  vk::raii::Pipeline graphicsPipeline_ = nullptr;
  std::vector<vk::raii::Semaphore> renderFinishedSemaphores_;
  std::vector<vk::ImageLayout> swapChainImageLayouts_;
  std::vector<vk::Fence> imagesInFlight_;
};
