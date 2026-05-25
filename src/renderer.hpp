#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "device.hpp"
#include "mesh.hpp"
#include "scene.hpp"
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

  void setMeshes(std::vector<Mesh> const &meshes);
  void setMaterials(std::vector<Material> const &materials);

  FrameResult beginFrame(glm::mat4 const &viewProjMatrix,
                         glm::vec3 const &cameraPosition);
  void drawObject(MeshId meshId, MaterialId materialId,
                  glm::mat4 const &modelMatrix);

  FrameResult endFrame();

  FrameResult drawFrame(MeshId meshId, MaterialId materialId,
                        glm::mat4 const &modelMatrix,
                        glm::mat4 const &viewProjMatrix,
                        glm::vec3 const &cameraPosition);
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
  struct DepthResources {
    vk::raii::Image image = nullptr;
    vk::raii::DeviceMemory memory = nullptr;
    vk::raii::ImageView imageView = nullptr;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
  };

  struct TextureResources {
    vk::raii::Image image = nullptr;
    vk::raii::DeviceMemory memory = nullptr;
    vk::raii::ImageView imageView = nullptr;
    vk::raii::Sampler sampler = nullptr;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
  };

  struct MaterialGpuResources {
    TextureResources albedoTexture;
    vk::DescriptorSet descriptorSet = nullptr;
    glm::vec4 tint{1.0f};
  };

  static constexpr std::uint32_t kFramesInFlight = 1;

  static std::vector<char> readBinaryFile(char const *path);

  void createPersistentResources();
  void createFrameResources();
  MeshGpuResources createGeometryResources(Mesh const &mesh);
  void createCommandBuffers();
  void createCommandPool();
  void updateFrameUniformBuffer(FrameContext &frame,
                                glm::mat4 const &viewProjMatrix,
                                glm::vec3 const &cameraPosition) const;

  void validateSwapChainCandidate(SwapChain const &swapChain) const;
  void validateSwapChainState() const;
  vk::raii::Pipeline
  createGraphicsPipeline(SwapChain const &swapChain,
                         vk::raii::PipelineLayout const &pipelineLayout) const;

  void beginCommandBuffer(vk::raii::CommandBuffer const &commandBuffer,
                          FrameContext const &frame, std::uint32_t imageIndex);

  void endCommandBuffer(vk::raii::CommandBuffer const &commandBuffer,
                        std::uint32_t imageIndex);

  static constexpr vk::Format kDepthFormat = vk::Format::eD32Sfloat;

  DepthResources createDepthResources(SwapChain const &swapChain) const;
  void transitionSwapChainImage(vk::raii::CommandBuffer const &commandBuffer,
                                std::uint32_t imageIndex,
                                vk::ImageLayout newLayout,
                                vk::PipelineStageFlags2 srcStageMask,
                                vk::AccessFlags2 srcAccessMask,
                                vk::PipelineStageFlags2 dstStageMask,
                                vk::AccessFlags2 dstAccessMask);

  void transitionDepthImage(vk::raii::CommandBuffer const &commanderBuffer,
                            vk::ImageLayout newLayout,
                            vk::PipelineStageFlags2 srcStageMask,
                            vk::AccessFlags2 srcAccessMask,
                            vk::PipelineStageFlags2 dstStageMask,
                            vk::AccessFlags2 dstAccessMask);

  TextureResources createCheckerTextureResources();
  void transitionTextureImage(TextureResources &texture,
                              vk::ImageLayout newLayout,
                              vk::PipelineStageFlags2 srcStageMask,
                              vk::AccessFlags2 srcAccessMask,
                              vk::PipelineStageFlags2 dstStageMask,
                              vk::AccessFlags2 dstAccessMask);
  void copyBufferToImage(vk::Buffer sourceBuffer, vk::Image destinationImage,
                         std::uint32_t width, std::uint32_t height);

  void createFrameDescriptorSetLayout();
  void createMaterialDescriptorSetLayout();
  void createFrameDescriptorPool();
  void allocateAndWriteFrameDescriptorSets();

  TextureResources createTextureResourcesFromFile(std::string const &path);
  vk::raii::DescriptorPool
  createMaterialDescriptorPool(std::uint32_t materialCount) const;
  void writeMaterialDescriptorSets();

private:
  DepthResources depthResources_{};

  Device const &device_;
  SwapChain const *swapChain_ = nullptr;

  vk::raii::CommandPool commandPool_ = nullptr;
  std::vector<FrameContext> frames_;
  vk::raii::CommandBuffers commandBuffers_ = nullptr;
  std::uint32_t currentFrame_ = 0;
  std::optional<ActiveFrameState> activeFrame_;

  std::vector<MaterialGpuResources> materialGpuResources_;

  vk::raii::DescriptorSetLayout frameDescriptorSetLayout_ = nullptr;
  vk::raii::DescriptorSetLayout materialDescriptorSetLayout_ = nullptr;
  vk::raii::DescriptorPool frameDescriptorPool_ = nullptr;
  vk::raii::DescriptorPool materialDescriptorPool_ = nullptr;

  vk::raii::PipelineLayout pipelineLayout_ = nullptr;
  vk::raii::Pipeline graphicsPipeline_ = nullptr;
  std::vector<vk::raii::Semaphore> renderFinishedSemaphores_;
  std::vector<vk::ImageLayout> swapChainImageLayouts_;
  std::vector<vk::Fence> imagesInFlight_;
  std::vector<MeshGpuResources> meshGpuResources_;
};
