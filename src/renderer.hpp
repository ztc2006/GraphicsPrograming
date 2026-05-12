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

  Renderer(Device const &device, SwapChain const &swapChain);

  FrameResult drawFrame();

private:
  static std::vector<char> readBinaryFile(char const *path);
  void createCommandPool();
  void createGraphicsPipeline();
  void createCommandBuffers();
  void createSyncObjects();
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
  SwapChain const &swapChain_;

  vk::raii::PipelineLayout pipelineLayout_ = nullptr;
  vk::raii::Pipeline graphicsPipeline_ = nullptr;
  vk::raii::CommandPool commandPool_ = nullptr;
  vk::raii::CommandBuffers commandBuffers_ = nullptr;
  vk::raii::Semaphore imageAvailableSemaphore_ = nullptr;
  vk::raii::Fence inFlightFence_ = nullptr;
  std::vector<vk::ImageLayout> swapChainImageLayouts_;
  std::vector<vk::raii::Semaphore> renderFinishedSemaphores_;
};
