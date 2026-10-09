#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "asset_ids.hpp"
#include "asset_library.hpp"
#include "cluster_grid.hpp"
#include "device.hpp"
#include "gtao.hpp"
#include "hdr_ibl.hpp"
#include "hdr_output.hpp"
#include "material_gpu_store.hpp"
#include "measurement.hpp"
#include "mesh.hpp"
#include "render_graph.hpp"
#include "ray_tracing.hpp"
#include "scene.hpp"
#include "scene_object.hpp"
#include "swap_chain.hpp"
#include "taa_resolve.hpp"
#include "temporal_motion.hpp"
#include "texture.hpp"

class Renderer {
  friend struct RendererTransportTestAccess;
  friend struct RendererRtTestAccess;
  friend struct RendererAoTestAccess;
  friend struct RendererMotionTestAccess;
  friend struct RendererHdrTestAccess;
  friend struct RendererFrameTestAccess;
  const unsigned framesInFlight_;
  struct MeshGpuResources {
    Device::BufferResources vertex, index;
    std::uint32_t indexCount = 0, vertexCount = 0;
    bool unitVertexAlpha = true;
    DielectricGeometry dielectric;
  };

public:
  // Preparation only records independent resource work; it may run in one
  // background task while frames render. Candidates must not outlive Renderer.
  class SceneAssets {
    friend class Renderer;
    friend struct RendererTransportTestAccess;
    friend struct RendererRtTestAccess;
    SceneAssets() = default;
    Renderer const *owner_ = nullptr;
    ResourceLedger::Scope resourceScope_;
    std::vector<MeshGpuResources> meshes_;
    std::unique_ptr<MaterialGpuStore> materials_;
    std::unique_ptr<RayTracingGeometry> rtGeometry_;
    std::size_t materialCount_ = 0, rtTextureCount_ = 0;
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

  explicit Renderer(Device const &device, unsigned framesInFlight = 1);
  unsigned framesInFlight() const { return framesInFlight_; }
  Renderer(Renderer const &) = delete;
  Renderer &operator=(Renderer const &) = delete;
  Renderer(Renderer &&) = delete;
  Renderer &operator=(Renderer &&) = delete;

  struct DrawItem {
    std::size_t objectIndex = invalidMotionIdentity;
    MeshId meshId = 0;
    MaterialId materialId = 0;
    glm::mat4 modelMatrix{1.0f};
    Aabb worldBounds{};
    float sortDepthSq = 0.0f;
    bool primaryVisible = true, shadowCaster = true;
  };
  struct SceneDrawList {
    std::span<DrawItem const> opaque, mask, transparent;
    // Complete sets, including objects outside the camera frustum.
    std::span<DrawItem const> allOpaque, allMask, allTransparent;
    bool sky = true, bounds = false;
  };
  struct DrawStatistics {
    std::uint32_t shadow = 0, main = 0, debug = 0;
    std::uint32_t shadowCandidates = 0, shadowCulled = 0;
  };
  FrameResult renderFrame(SceneDrawList const &scene, glm::mat4 const &viewProj,
                          glm::vec3 const &camera,
                          LightingSettings const &lighting, bool shadows);
  enum class RenderMethod { Raster, RayTracing };
  void setRenderMethod(RenderMethod);
  std::uint32_t rayTracingSamples() const { return rayTracing_ ? rayTracing_->accumulatedSamples() : 0; }
  RenderMethod renderMethod() const { return renderMethod_; }
  bool rayTracingAvailable() const {
    return bool(rayTracing_) && (!sceneAssets_ || sceneAssets_->materialCount_ <= RayTracingRenderer::textureCapacity / 2 && sceneAssets_->rtTextureCount_ <= RayTracingRenderer::textureCapacity);
  }
  std::string const &rayTracingUnavailableReason() const {
    static std::string const limit = "Scene exceeds RT-A capacity of 256 materials / 512 unique textures; raster remains available";
    return rayTracing_ && sceneAssets_ && (sceneAssets_->materialCount_ > RayTracingRenderer::textureCapacity / 2 || sceneAssets_->rtTextureCount_ > RayTracingRenderer::textureCapacity)
        ? limit : rtUnavailableReason_;
  }
  void setTemporalJitterEnabled(bool enabled);
  void setAoSettings(AoSettings const &);
  AoSettings const &aoSettings() const { return aoSettings_; }
  bool aoActive() const { return lastAoActive_; }
  void setTaaEnabled(bool);
  void setShadowCasterCullingEnabled(bool);
  bool shadowCasterCullingEnabled() const { return shadowCasterCullingEnabled_; }
  void setTaaHistoryFilter(TaaHistoryFilter);
  TaaHistoryFilter taaHistoryFilter() const {return taaHistoryFilter_;}
  bool taaEnabled() const {return taaEnabled_;}
  bool taaActive() const {return lastTaaActive_;}
  bool temporalJitterEnabled() const { return temporalJitterEnabled_; }
  void invalidateTemporalHistory();
  TemporalCamera const &temporalCamera() const { return lastTemporalCamera_; }
  DrawStatistics const &drawStatistics() const { return drawStatistics_; }
  // Explicit offline capture. Complete scene lists required; drains frame users
  // before capture/publication and never acquires or presents a swapchain image.
  void captureLocalProbe(SceneDrawList const &, LightingSettings const &);
  bool localProbeValid() const { return probeValid_; }
  bool detailReflectionProbeValid() const {
    return probeValid_ && detailProbeValid_;
  }
  unsigned sunCascadesAssigned() const { return unsigned(lastIndoor_.sun.params.x); }
  SunCascadeGpu const &sunCascades() const { return lastIndoor_.sun; }
  unsigned spotShadowsAssigned() const { return lastIndoor_.counts.x; }
  unsigned spotShadowsRequested() const { return lastIndoor_.counts.y; }
  bool localProbeMatches(LightingSettings const &) const;
  bool clusterSupported() const { return clusterSupported_; }
  ClusterGrid const &clusterGrid() const { return lastClusterGrid_; }
  std::string const &renderGraphDump() const { return lastGraphDump_; }

  PreparedScene prepareScene(AssetLibrary const &assets);
  // Main thread, outside frame recording. First call submits upload and returns
  // false; later calls poll and commit when ready. false preserves the
  // candidate. retireSceneUi runs after old frame users complete, while old
  // assets exist; it must not throw or reenter Renderer. Commit itself never
  // waits for frames.
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

  // Compatibility recording facade: collects items, never schedules GPU passes.
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
  // Delivered once per completed submission, outside frame recording. The
  // callback must not throw or reenter Renderer; UI keeps the highest frame ID.
  void setGpuTimingCallback(std::function<void(GpuTimings const &)> callback);
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
    Device::BufferResources punctualLights;
    std::size_t punctualCapacity = 0;
    Device::BufferResources clusterConfig, clusterIndices;
    Device::BufferResources motionBuffer;
    std::size_t motionCapacityBytes = sizeof(MotionObjectGpu);
    std::optional<TemporalSnapshot> temporal;
    Device::BufferResources indoorBuffer;
    IndoorLightingGpu indoor;
    glm::vec3 cameraPosition{};
    bool shadowReceiverCullingAllowed=false;
    std::size_t clusterCapacityBytes = 4;
    ClusterGrid clusterGrid;
    RenderGraph::BufferState clusterState;
    vk::DescriptorSet descriptorSet = nullptr;
    vk::raii::QueryPool timestamps = nullptr, taaTimestamps = nullptr,
                        aoTimestamps = nullptr;
    bool taaEnabled = false, aoEnabled = false, rayTracing = false;
    std::uint64_t frameId = 0;
    bool submitted = false;
    bool shadowEnabled = false, uiEnabled = false, clusterEnabled = false;
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
      }
      return *this;
    }
    ResourceLedger::Lease accounting;
    GpuImage storage;
    vk::raii::ImageView imageView = nullptr;
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
      }
      return *this;
    }
    ResourceLedger::Lease accounting;
    GpuImage storage;
    vk::raii::ImageView imageView = nullptr;
    vk::raii::Sampler sampler = nullptr;
    vk::raii::Sampler debugSampler = nullptr;
  };

  enum class ActivePass {
    eNone,
    eShadow,
    eMain,
  };

  static constexpr std::uint32_t kShadowMapSize = 2048, kShadowAtlasWidth = 4096;

  static std::vector<char> readBinaryFile(char const *path);

  void onFrameSubmitted(FrameContext &frame);
  void collectFrameTimings(FrameContext &frame);
  std::function<void(GpuTimings const &)> gpuTimingCallback_;
  void timestamp(vk::raii::CommandBuffer const &command, std::uint32_t query);
  unsigned timestampBits_ = 0;
  double timestampPeriod_ = 0;
  GpuTimings gpuTimings_{};
  CpuSyncTimes cpuSyncTimes_{};
  std::uint64_t submittedFrameId_ = 0;
  void createPersistentResources();
  void createFrameResources();
  void createClusterPipeline();
  void updateFrameClusters(FrameContext &frame, ClusterGrid const &grid);
  MeshGpuResources createGeometryResources(Mesh const &mesh,
                                           UploadBatch &uploads,
                                           ResourceLedger::Scope scope = {});
  void createCommandBuffers();
  void createCommandPool();
  // Call only after this slot's fence completes, before image acquisition.
  FrameResult beginFrameImpl(glm::mat4 const &, glm::vec3 const &,
                             LightingSettings const &, bool, std::span<MotionObject const>);
  void updateFrameMotion(FrameContext &, TemporalSnapshot &&);
  DepthResources createMotionResources(vk::Extent2D) const;
  void updateFrameLights(FrameContext &frame,
                         PackedPunctualLights const &lights);
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

  struct FrameGraph {
    RenderGraph::Plan plan;
    RenderGraph::ImageId shadow{}, depth{}, hdr{}, motion{}, diffuse{},
        output{};
    std::optional<RenderGraph::PassId> shadowPass, uiPass, clusterPass, rtBuildPass;
    bool rayTracing = false;
    RenderGraph::BufferId clusterIndices{};
    RenderGraph::PassId mainPass{}, outputPass{};
    std::optional<TaaResolve::Frame> taa;
    std::optional<Gtao::Frame> ao;
  };
  struct ImageStates {
    RenderGraph::State shadow, depth, hdr, motion;
    std::vector<RenderGraph::State> output;
  } imageStates_;
  void prepareRayTracing(SceneDrawList const &);
  std::vector<vk::DescriptorImageInfo> rtTextures(std::vector<glm::uvec4> *main=nullptr,
      std::vector<glm::uvec4> *more=nullptr, std::vector<glm::uvec4> *maps=nullptr, MaterialGpuStore const *store=nullptr, std::size_t count=0) const;
  FrameGraph buildRayTracingGraph(std::uint32_t imageIndex) const;
  FrameGraph buildFrameGraph(std::uint32_t imageIndex, bool shadows) const;
  FrameResult finishFrame(SceneDrawList const &scene);
  void recordGraph(SceneDrawList const &scene);
  void recordObject(MeshId meshId, MaterialId materialId,
                    glm::mat4 const &modelMatrix, std::uint32_t motionIndex = 0);
  void recordEnvironment();
  void recordShadowTiles(SceneDrawList const &);
  int activeShadowIndex_ = -1;
  bool activeSunShadow_ = true;
  IndoorLightingGpu lastIndoor_;
  IndoorLightingGpu makeIndoorLighting(LightingSettings const &, PackedPunctualLights &, bool) const;
  BakedEnvironment globalEnvironment_;
  EnvironmentSh probeSh_;
  bool probeValid_ = false;
  LocalProbeSettings capturedProbe_, capturedDetailProbe_;
  bool detailProbeValid_ = false;
  std::vector<std::byte> capturedLightingKey_, capturedGeometryKey_;
  bool probeGeometryDirty_ = false, probeMaterialsDirty_ = false;
  std::vector<std::byte> probeGeometryKey(SceneDrawList const &) const;
  std::vector<std::byte> probeLightingKey(LightingSettings const &) const;
  void recordAabb(Aabb const &bounds, glm::vec4 const &color);
  void validateDrawItem(DrawItem const &item, bool caster) const;
  RenderGraph::State const &hdrState() const;
  std::optional<FrameGraph> activeGraph_;
  std::string lastGraphDump_;
  DrawStatistics drawStatistics_;
  bool shadowCasterCullingEnabled_ = true;
  bool requestedShadows_ = false, queuedSky_ = false;
  std::vector<DrawItem> queuedObjects_, queuedCasters_;
  struct DebugBox {
    Aabb bounds;
    glm::vec4 color;
  };
  std::vector<DebugBox> queuedBoxes_;

  static constexpr vk::Format kDepthFormat = vk::Format::eD32Sfloat;

  DepthResources createDepthResources(SwapChain const &swapChain) const;

  void createFrameDescriptorSetLayout();
  void createFrameDescriptorPool();
  void allocateAndWriteFrameDescriptorSets();

  ShadowResources createShadowResources() const;
  vk::raii::Pipeline
  createShadowPipeline(vk::raii::PipelineLayout const &pipelineLayout,
                       vk::CullModeFlagBits cullMode) const;

private:
  RenderMethod renderMethod_ = RenderMethod::Raster;
  std::unique_ptr<RayTracingRenderer> rayTracing_;
  std::string rtUnavailableReason_ = "RT resources not initialized";
  RtPush rtPush_{.options={3,2,0,0}};
  LightingSettings rtLighting_;
  bool rtShadows_ = false;
  unsigned rtMaxBounces_ = 8, rtSpp_ = 1;
  bool rtAccumulate_ = true;
  TaaHistoryFilter taaHistoryFilter_=TaaHistoryFilter::CatmullRom;
  bool taaEnabled_=false,lastTaaActive_=false;
  std::unique_ptr<TaaResolve> taa_;
  TaaPush taaPush_;
  AoSettings aoSettings_;
  bool lastAoActive_ = false;
  std::unique_ptr<Gtao> gtao_;
  GtaoPush aoPush_;
  TemporalMotionHistory temporalHistory_;
  TemporalCamera lastTemporalCamera_;
  bool temporalJitterEnabled_ = false;
  DepthResources motionResources_{};
  static constexpr vk::Format kMotionFormat = vk::Format::eR16G16B16A16Sfloat;
  DepthResources depthResources_{};
  std::unique_ptr<HdrOutput> hdrOutput_;
  DisplaySettings displaySettings_;

  ShadowResources shadowResources_{};
  vk::raii::Pipeline shadowPipeline_ = nullptr;
  vk::raii::Pipeline mirroredShadowPipeline_ = nullptr;
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

  bool clusterSupported_ = false;
  ClusterGrid lastClusterGrid_;
  vk::raii::PipelineLayout clusterPipelineLayout_ = nullptr;
  vk::raii::Pipeline clusterPipeline_ = nullptr;
  vk::raii::PipelineLayout pipelineLayout_ = nullptr;
  vk::raii::Pipeline graphicsPipeline_ = nullptr;
  vk::raii::Pipeline mirroredGraphicsPipeline_ = nullptr;
  vk::raii::Pipeline doubleSidedGraphicsPipeline_ = nullptr;
  vk::raii::Pipeline transparentPipeline_ = nullptr;
  vk::raii::Pipeline mirroredTransparentPipeline_ = nullptr;
  vk::raii::Pipeline doubleSidedTransparentPipeline_ = nullptr;
  vk::raii::Pipeline debugLinePipeline_ = nullptr;
  vk::raii::Pipeline environmentPipeline_ = nullptr;
  TextureResources environmentTexture_{};
  TextureResources environmentPrefilter_{};
  TextureResources environmentBrdfLut_{};
  EnvironmentSh environmentSh_{};
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
