#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "asset_ids.hpp"
#include "asset_library.hpp"
#include "device.hpp"
#include "hdr_ibl.hpp"
#include "hdr_output.hpp"
#include "material_gpu_store.hpp"
#include "measurement.hpp"
#include "mesh.hpp"
#include "scene.hpp"
#include "scene_object.hpp"
#include "swap_chain.hpp"
#include "texture.hpp"

class Renderer {
  friend struct RendererHdrTestAccess;
  struct MeshGpuResources {
    Device::BufferResources vertex, index;
    std::uint32_t indexCount = 0;
  };

public:
  // Preparation only records independent resource work; it may run in one
  // background task while frames render. Candidates must not outlive Renderer.
  class SceneAssets {
    friend class Renderer;
    SceneAssets() = default;
    Renderer const *owner_ = nullptr;
    ResourceLedger::Scope resourceScope_;
    std::vector<MeshGpuResources> meshes_;
    std::unique_ptr<MaterialGpuStore> materials_;
    // Destroy command/staging storage before destination resources on fallback.
    std::unique_ptr<UploadBatch> uploads_;

  public:
    ~SceneAssets() = default;
  };
  using PreparedScene = std::shared_ptr<SceneAssets>;

  struct ResourceStatistics {
    std::uint64_t sceneCommits = 0;
    std::uint64_t environmentUploads = 0;
    std::uint64_t pipelineBuilds = 0;
    UploadBatch::Statistics lastSceneUpload{};
    std::uint64_t sceneUploadSubmissions = 0, sceneImageCopies = 0;
    std::uint64_t sceneUploadFenceWaits = 0, completedFrameId = 0;
    std::size_t pendingSceneUploads = 0, retiredScenes = 0;
  };
  ResourceLedger::Snapshot resourceSnapshot() const {
    return device_.resourceLedger().snapshot();
  }
  ResourceStatistics const &resourceStatistics() const {
    return resourceStatistics_;
  }

  struct RasterizerDebugSettings {
    vk::CullModeFlagBits cullMode = vk::CullModeFlagBits::eBack;
    // The projection flips Y for Vulkan's positive-height viewport. Imported
    // glTF/OBJ triangles with outward CCW winding remain CCW in framebuffer
    // coordinates; marking them CW makes double-sided shading invert normals.
    vk::FrontFace frontFace = vk::FrontFace::eCounterClockwise;
  };

  enum class FrameResult {
    eSuccess,
    eSwapChainOutOfDate,
    eSwapChainSuboptimal,
  };

  explicit Renderer(Device const &device);
  Renderer(Renderer const &) = delete;
  Renderer &operator=(Renderer const &) = delete;
  Renderer(Renderer &&) = delete;
  Renderer &operator=(Renderer &&) = delete;

  PreparedScene prepareScene(AssetLibrary const &assets);
  // Main thread, outside frame recording. First call submits upload and returns
  // false; later calls poll and commit when ready. false preserves the candidate.
  // retireSceneUi runs after old frame users complete, while old assets exist;
  // it must not throw or reenter Renderer. Commit itself never waits for frames.
  bool commitScene(PreparedScene &candidate,
                   std::function<void()> retireSceneUi = {});
  // Explicit blocking convenience only for cold startup and offline tests.
  void waitSceneUpload(PreparedScene const &candidate);
  void setMaterialTint(MaterialId materialId, glm::vec4 const &tint);
  void setMaterialSurfaceParams(MaterialId materialId, float normalScale,
                                float parallaxScale);
  void setMaterialAlphaParams(MaterialId materialId, AlphaMode alphaMode,
                              float alphaCutoff);
  void setSurfaceDebugEnabled(bool normalMapsEnabled, bool parallaxEnabled);
  TextureResources const &materialAlbedoTexture(MaterialId materialId) const;
  TextureResources const *materialAlphaTexture(MaterialId materialId) const;
  void setUiDrawCallback(std::function<void(vk::CommandBuffer)> callback);
  RasterizerDebugSettings rasterizerDebugSettings() const {
    return rasterizerDebugSettings_;
  }
  void setRasterizerDebugSettings(RasterizerDebugSettings settings);

  FrameResult beginFrame(glm::mat4 const &viewProjMatrix,
                         glm::vec3 const &cameraPosition,
                         LightingSettings const &lighting,
                         bool shadowPassEnabled);
  void drawObject(MeshId meshId, MaterialId materialId,
                  glm::mat4 const &modelMatrix);
  void drawEnvironment();
  void drawAabb(Aabb const &bounds, glm::vec4 const &color);

  FrameResult endFrame();

  FrameResult drawFrame(MeshId meshId, MaterialId materialId,
                        glm::mat4 const &modelMatrix,
                        glm::mat4 const &viewProjMatrix,
                        glm::vec3 const &cameraPosition,
                        LightingSettings const &lighting);
  void recreateForSwapChain(SwapChain const &swapChain);
  void beginMainPass();
  GpuTimings const &gpuTimings() const { return gpuTimings_; }
  bool gpuTimingSupported() const { return timestampBits_ != 0; }
  std::uint64_t submittedFrameId() const { return submittedFrameId_; }
  void collectCompletedWork();
  struct CpuSyncTimes {
    double fenceMs = 0, acquireMs = 0, submitMs = 0, presentMs = 0;
  };
  CpuSyncTimes const &cpuSyncTimes() const { return cpuSyncTimes_; }

private:
  struct FrameContext {
    vk::raii::Semaphore imageAvailableSemaphore = nullptr;
    vk::raii::Fence inFlightFence = nullptr;
    Device::BufferResources uniform;
    vk::DescriptorSet descriptorSet = nullptr;
    vk::raii::QueryPool timestamps = nullptr;
    std::uint64_t frameId = 0;
    bool submitted = false;
    bool shadowEnabled = false, uiEnabled = false;
  };

  struct ActiveFrameState {
    std::uint32_t frameIndex = 0;
    std::uint32_t imageIndex = 0;
    vk::Result acquireResult = vk::Result::eSuccess;
  };

  struct DepthResources {
    DepthResources() = default;
    DepthResources(DepthResources const &) = delete;
    DepthResources &operator=(DepthResources const &) = delete;
    DepthResources(DepthResources &&) noexcept = default;
    DepthResources &operator=(DepthResources &&other) noexcept {
      if (this != &other) {
        imageView.clear();
        accounting.reset();
        storage = std::move(other.storage);
        accounting = std::move(other.accounting);
        imageView = std::move(other.imageView);
        layout = other.layout;
      }
      return *this;
    }
    ResourceLedger::Lease accounting;
    GpuImage storage;
    vk::raii::ImageView imageView = nullptr;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
  };

  struct ShadowResources {
    ShadowResources() = default;
    ShadowResources(ShadowResources const &) = delete;
    ShadowResources &operator=(ShadowResources const &) = delete;
    ShadowResources(ShadowResources &&) noexcept = default;
    ShadowResources &operator=(ShadowResources &&other) noexcept {
      if (this != &other) {
        sampler.clear();
        debugSampler.clear();
        imageView.clear();
        accounting.reset();
        storage = std::move(other.storage);
        accounting = std::move(other.accounting);
        imageView = std::move(other.imageView);
        sampler = std::move(other.sampler);
        debugSampler = std::move(other.debugSampler);
        layout = other.layout;
      }
      return *this;
    }
    ResourceLedger::Lease accounting;
    GpuImage storage;
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

  void collectFrameTimings(FrameContext &frame);
  void timestamp(vk::raii::CommandBuffer const &command, std::uint32_t query);
  unsigned timestampBits_ = 0;
  double timestampPeriod_ = 0;
  GpuTimings gpuTimings_{};
  CpuSyncTimes cpuSyncTimes_{};
  std::uint64_t submittedFrameId_ = 0;
  void createPersistentResources();
  void createFrameResources();
  MeshGpuResources createGeometryResources(Mesh const &mesh, UploadBatch &uploads,
      ResourceLedger::Scope scope = {});
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
                         vk::raii::PipelineLayout const &pipelineLayout,
                         vk::CullModeFlagBits cullMode) const;
  vk::raii::Pipeline
  createTransparentPipeline(SwapChain const &swapChain,
                            vk::raii::PipelineLayout const &pipelineLayout,
                            vk::CullModeFlagBits cullMode) const;
  vk::raii::Pipeline
  createDebugLinePipeline(SwapChain const &swapChain,
                          vk::raii::PipelineLayout const &pipelineLayout) const;
  vk::raii::Pipeline createEnvironmentPipeline(
      SwapChain const &swapChain,
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
  createShadowPipeline(vk::raii::PipelineLayout const &pipelineLayout,
                       vk::CullModeFlagBits cullMode) const;
  void beginShadowPass(vk::raii::CommandBuffer const &commandBuffer,
                       FrameContext const &frame);
  void beginMainPass(vk::raii::CommandBuffer const &commandBuffer,
                     FrameContext const &frame, std::uint32_t imageIndex,
                     bool shadowPassEnabled);
  void transitionShadowImage(vk::raii::CommandBuffer const &commandBuffer,
                             vk::ImageLayout newLayout,
                             vk::PipelineStageFlags2 srcStage,
                             vk::AccessFlags2 srcAccess,
                             vk::PipelineStageFlags2 dstStage,
                             vk::AccessFlags2 dstAccess);

private:
  DepthResources depthResources_{};
  std::unique_ptr<HdrOutput> hdrOutput_;
  DisplaySettings displaySettings_;

  ShadowResources shadowResources_{};
  vk::raii::Pipeline shadowPipeline_ = nullptr;
  vk::raii::Pipeline doubleSidedShadowPipeline_ = nullptr;
  MeshGpuResources debugAabbLineResources_{};
  ActivePass activePass_ = ActivePass::eNone;

  Device const &device_;
  vk::raii::DescriptorSetLayout materialDescriptorSetLayout_ = nullptr;
  MaterialGpuStore &materials() const;
  TextureCache textureCache_;
  std::mutex preparationMutex_;
  void submitSceneUpload(PreparedScene const &candidate);
  void validateSceneCandidate(PreparedScene const &candidate) const;
  ResourceStatistics resourceStatistics_{};
  bool normalMapsEnabled_ = true;
  bool parallaxEnabled_ = true;
  SwapChain const *swapChain_ = nullptr;

  vk::raii::CommandPool commandPool_ = nullptr;
  std::vector<FrameContext> frames_;
  vk::raii::CommandBuffers commandBuffers_ = nullptr;
  std::uint32_t currentFrame_ = 0;
  std::optional<ActiveFrameState> activeFrame_;

  vk::raii::DescriptorSetLayout frameDescriptorSetLayout_ = nullptr;
  ResourceLedger::Lease frameDescriptorAccounting_;
  vk::raii::DescriptorPool frameDescriptorPool_ = nullptr;

  vk::raii::PipelineLayout pipelineLayout_ = nullptr;
  vk::raii::Pipeline graphicsPipeline_ = nullptr;
  vk::raii::Pipeline doubleSidedGraphicsPipeline_ = nullptr;
  vk::raii::Pipeline transparentPipeline_ = nullptr;
  vk::raii::Pipeline doubleSidedTransparentPipeline_ = nullptr;
  vk::raii::Pipeline debugLinePipeline_ = nullptr;
  vk::raii::Pipeline environmentPipeline_ = nullptr;
  TextureResources environmentTexture_{};
  EnvironmentSh environmentSh_{};
  std::vector<vk::raii::Semaphore> renderFinishedSemaphores_;
  std::vector<vk::ImageLayout> swapChainImageLayouts_;
  std::vector<vk::Fence> imagesInFlight_;
  PreparedScene sceneAssets_;
  std::vector<PreparedScene> pendingSceneUploads_;
  struct RetiredScene {
    std::uint64_t lastFrame = 0;
    PreparedScene assets;
    std::function<void()> releaseUi;
  };
  std::vector<RetiredScene> retiredScenes_;
  std::function<void(vk::CommandBuffer)> uiDrawCallback_;
  RasterizerDebugSettings rasterizerDebugSettings_{};
};
