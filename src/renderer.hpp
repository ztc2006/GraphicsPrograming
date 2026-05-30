#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "device.hpp"
#include "material_gpu_store.hpp"
#include "mesh.hpp"
#include "scene.hpp"
#include "scene_object.hpp"
#include "swap_chain.hpp"
#include "texture.hpp"

class Renderer {
public:
  struct RasterizerDebugSettings {
    vk::CullModeFlagBits cullMode = vk::CullModeFlagBits::eNone;
    vk::FrontFace frontFace = vk::FrontFace::eCounterClockwise;
  };

  enum class FrameResult {
    eSuccess,
    eSwapChainOutOfDate,
    eSwapChainSuboptimal,
  };

  explicit Renderer(Device const &device);

  void setMeshes(std::vector<Mesh> const &meshes);
  void setMaterials(std::vector<Material> const &materials);
  void setMaterialTint(MaterialId materialId, glm::vec4 const &tint);
  void setUiDrawCallback(std::function<void(vk::CommandBuffer)> callback);
  RasterizerDebugSettings rasterizerDebugSettings() const {
    return rasterizerDebugSettings_;
  }
  void setRasterizerDebugSettings(RasterizerDebugSettings settings);

  FrameResult beginFrame(glm::mat4 const &viewProjMatrix,
                         glm::vec3 const &cameraPosition,
                         LightingSettings const &lighting);
  void drawObject(MeshId meshId, MaterialId materialId,
                  glm::mat4 const &modelMatrix);
  void drawAabb(Aabb const &bounds, glm::vec4 const &color);

  FrameResult endFrame();

  FrameResult drawFrame(MeshId meshId, MaterialId materialId,
                        glm::mat4 const &modelMatrix,
                        glm::mat4 const &viewProjMatrix,
                        glm::vec3 const &cameraPosition,
                        LightingSettings const &lighting);
  void recreateForSwapChain(SwapChain const &swapChain);
  void beginMainPass();

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

  struct ShadowResources {
    vk::raii::Image image = nullptr;
    vk::raii::DeviceMemory memory = nullptr;
    vk::raii::ImageView imageView = nullptr;
    vk::raii::Sampler sampler = nullptr;
    vk::raii::Sampler debugSampler = nullptr;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
  };

  enum class ActivePass {
    eNone,
    eShadow,
    eMain,
  };

  static constexpr std::uint32_t kFramesInFlight = 1;
  static constexpr std::uint32_t kShadowMapSize = 2048;

  static std::vector<char> readBinaryFile(char const *path);

  void createPersistentResources();
  void createFrameResources();
  MeshGpuResources createGeometryResources(Mesh const &mesh);
  void createCommandBuffers();
  void createCommandPool();
  void updateFrameUniformBuffer(FrameContext &frame,
                                glm::mat4 const &viewProjMatrix,
                                glm::vec3 const &cameraPosition,
                                LightingSettings const &lighting) const;

  void validateSwapChainCandidate(SwapChain const &swapChain) const;
  void validateSwapChainState() const;
  vk::raii::Pipeline
  createGraphicsPipeline(SwapChain const &swapChain,
                         vk::raii::PipelineLayout const &pipelineLayout) const;
  vk::raii::Pipeline
  createDebugLinePipeline(SwapChain const &swapChain,
                          vk::raii::PipelineLayout const &pipelineLayout) const;

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

  void createFrameDescriptorSetLayout();
  void createFrameDescriptorPool();
  void allocateAndWriteFrameDescriptorSets();

  ShadowResources createShadowResources() const;
  vk::raii::Pipeline
  createShadowPipeline(vk::raii::PipelineLayout const &pipelineLayout) const;
  void beginShadowPass(vk::raii::CommandBuffer const &commandBuffer,
                       FrameContext const &frame);
  void beginMainPass(vk::raii::CommandBuffer const &commandBuffer,
                     FrameContext const &frame, std::uint32_t imageIndex);
  void transitionShadowImage(vk::raii::CommandBuffer const &commandBuffer,
                             vk::ImageLayout newLayout,
                             vk::PipelineStageFlags2 srcStage,
                             vk::AccessFlags2 srcAccess,
                             vk::PipelineStageFlags2 dstStage,
                             vk::AccessFlags2 dstAccess);

private:
  DepthResources depthResources_{};

  ShadowResources shadowResources_{};
  vk::raii::Pipeline shadowPipeline_ = nullptr;
  MeshGpuResources debugAabbLineResources_{};
  ActivePass activePass_ = ActivePass::eNone;

  Device const &device_;
  MaterialGpuStore materialGpuStore_;
  SwapChain const *swapChain_ = nullptr;

  vk::raii::CommandPool commandPool_ = nullptr;
  std::vector<FrameContext> frames_;
  vk::raii::CommandBuffers commandBuffers_ = nullptr;
  std::uint32_t currentFrame_ = 0;
  std::optional<ActiveFrameState> activeFrame_;

  vk::raii::DescriptorSetLayout frameDescriptorSetLayout_ = nullptr;
  vk::raii::DescriptorPool frameDescriptorPool_ = nullptr;

  vk::raii::PipelineLayout pipelineLayout_ = nullptr;
  vk::raii::Pipeline graphicsPipeline_ = nullptr;
  vk::raii::Pipeline debugLinePipeline_ = nullptr;
  std::vector<vk::raii::Semaphore> renderFinishedSemaphores_;
  std::vector<vk::ImageLayout> swapChainImageLayouts_;
  std::vector<vk::Fence> imagesInFlight_;
  std::vector<MeshGpuResources> meshGpuResources_;
  std::function<void(vk::CommandBuffer)> uiDrawCallback_;
  RasterizerDebugSettings rasterizerDebugSettings_{};
};
