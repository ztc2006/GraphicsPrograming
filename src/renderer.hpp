#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "device.hpp"
#include "mesh.hpp"
#include "swap_chain.hpp"

class Renderer {
public:
  enum class FrameResult {
    eSuccess,
    eSwapChainOutOfDate,
    eSwapChainSuboptimal,
  };

  explicit Renderer(Device const &device);

  void setMesh(Mesh const &mesh);
  FrameResult drawFrame(glm::mat4 const &modelMatrix);
  void recreateForSwapChain(SwapChain const &swapChain);

private:
  struct FrameContext {
    vk::raii::Semaphore imageAvailableSemaphore = nullptr;
    vk::raii::Fence inFlightFence = nullptr;
    vk::raii::Buffer uniformBuffer = nullptr;
    vk::raii::DeviceMemory uniformBufferMemory = nullptr;
    vk::DescriptorSet descriptorSet = nullptr;
  };

  static constexpr std::uint32_t kFramesInFlight = 1;

  static std::vector<char> readBinaryFile(char const *path);

  void createPersistentResources();
  void createFrameResources();
  void createGeometryResources(Mesh const &mesh);
  void createCommandBuffers();
  void createCommandPool();
  void createDescriptorSetLayout();
  void createDescriptorPool();
  void allocateAndWriteDescriptorSets();
  void updateFrameUniformBuffer(FrameContext &frame) const;
  void validateSwapChainCandidate(SwapChain const &swapChain) const;
  void validateSwapChainState() const;
  vk::raii::Pipeline
  createGraphicsPipeline(SwapChain const &swapChain,
                         vk::raii::PipelineLayout const &pipelineLayout) const;

  void recordCommandBuffer(vk::raii::CommandBuffer const &commandBuffer,
                           FrameContext const &frame, std::uint32_t imageIndex,
                           glm::mat4 const &modelMatrix);

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
  vk::raii::Buffer vertexBuffer_ = nullptr;
  vk::raii::DeviceMemory vertexBufferMemory_ = nullptr;
  vk::raii::Buffer indexBuffer_ = nullptr;
  vk::raii::DeviceMemory indexBufferMemory_ = nullptr;
  std::uint32_t indexCount_ = 0;
  vk::raii::DescriptorSetLayout descriptorSetLayout_ = nullptr;
  vk::raii::DescriptorPool descriptorPool_ = nullptr;
};
