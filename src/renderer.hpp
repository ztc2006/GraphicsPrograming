#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "device.hpp"
#include "mesh.hpp"
#include "scene_object.hpp"
#include "swap_chain.hpp"

class Renderer {
public:
  enum class FrameResult {
    eSuccess,
    eSwapChainOutOfDate,
    eSwapChainSuboptimal,
  };

  explicit Renderer(Device const &device);

  // Transitional single-mesh upload path until multi-object submission lands.
  void setMeshes(std::vector<Mesh> const &meshes);

  FrameResult beginFrame(glm::mat4 const &viewProjMatrix);
  void drawObject(MeshId meshId, glm::mat4 const &modelMatrix);
  FrameResult endFrame();

  FrameResult drawFrame(MeshId meshId, glm::mat4 const &modelMatrix,
                        glm::mat4 const &viewProjMatrix);
  void recreateForSwapChain(SwapChain const &swapChain);

private:
  struct FrameContext {
    vk::raii::Semaphore imageAvailableSemaphore = nullptr;
    vk::raii::Fence inFlightFence = nullptr;
    vk::raii::Buffer uniformBuffer = nullptr;
    vk::raii::DeviceMemory uniformBufferMemory = nullptr;
    vk::DescriptorSet descriptorSet = nullptr;
  };

  struct ActiveFrameState {
    std::uint32_t frameIndex = 0;
    std::uint32_t imageIndex = 0;
    vk::Result acquireResult = vk::Result::eSuccess;
  };

  struct MeshGpuResources {
    vk::raii::Buffer vertexBuffer = nullptr;
    vk::raii::DeviceMemory vertexBufferMemory = nullptr;
    vk::raii::Buffer indexBuffer = nullptr;
    vk::raii::DeviceMemory indexBufferMemory = nullptr;
    std::uint32_t indexCount = 0;
  };

  static constexpr std::uint32_t kFramesInFlight = 1;

  static std::vector<char> readBinaryFile(char const *path);

  void createPersistentResources();
  void createFrameResources();
  MeshGpuResources createGeometryResources(Mesh const &mesh);
  void createCommandBuffers();
  void createCommandPool();
  void createDescriptorSetLayout();
  void createDescriptorPool();
  void allocateAndWriteDescriptorSets();
  void updateFrameUniformBuffer(FrameContext &frame,
                                glm::mat4 const &viewProjMatrix) const;
  void validateSwapChainCandidate(SwapChain const &swapChain) const;
  void validateSwapChainState() const;
  vk::raii::Pipeline
  createGraphicsPipeline(SwapChain const &swapChain,
                         vk::raii::PipelineLayout const &pipelineLayout) const;

  void beginCommandBuffer(vk::raii::CommandBuffer const &commandBuffer,
                          FrameContext const &frame, std::uint32_t imageIndex);

  void endCommandBuffer(vk::raii::CommandBuffer const &commandBuffer,
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
  std::optional<ActiveFrameState> activeFrame_;

  vk::raii::PipelineLayout pipelineLayout_ = nullptr;
  vk::raii::Pipeline graphicsPipeline_ = nullptr;
  std::vector<vk::raii::Semaphore> renderFinishedSemaphores_;
  std::vector<vk::ImageLayout> swapChainImageLayouts_;
  std::vector<vk::Fence> imagesInFlight_;
  std::vector<MeshGpuResources> meshGpuResources_;
  vk::raii::DescriptorSetLayout descriptorSetLayout_ = nullptr;
  vk::raii::DescriptorPool descriptorPool_ = nullptr;
};
