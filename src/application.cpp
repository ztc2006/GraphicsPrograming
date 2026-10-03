#include "pch.hpp"

#include "application.hpp"
#include "build_info.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include "device.hpp"
#include "gltf_loader.hpp"
#include "obj_loader.hpp"
#include "renderer.hpp"
#include "swap_chain.hpp"

namespace {

const std::vector<char const *> kValidationLayers = {
    "VK_LAYER_KHRONOS_validation"};

#ifdef NDEBUG
constexpr bool kEnableValidationLayers = false;
#else
constexpr bool kEnableValidationLayers = true;
#endif

struct RenderQueueItem {
  std::size_t objectIndex = 0;
  MeshId meshId = 0;
  MaterialId materialId = 0;
  glm::mat4 modelMatrix{1.0f};
  Aabb worldBounds{};
  float sortDepthSq = 0.0f;
};

struct Frustum {
  std::array<glm::vec4, 6> planes{};
};

struct RenderQueueBuckets {
  std::vector<RenderQueueItem> opaque;
  std::vector<RenderQueueItem> mask;
  std::vector<RenderQueueItem> transparent;
};

struct VisibleRenderQueueBuckets {
  std::vector<RenderQueueItem> opaque;
  std::vector<RenderQueueItem> mask;
  std::vector<RenderQueueItem> transparent;
  std::size_t culledCount = 0;
};

std::string lowercase(std::string value) {
  for (char &c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

ImportedScene loadStaticModelScene(std::filesystem::path const &path,
                                   std::string const &fallbackAlbedoPath) {
  std::string const extension = lowercase(path.extension().string());
  if (extension == ".obj") {
    return loadStaticObjScene(path, fallbackAlbedoPath);
  }
  if (extension == ".gltf") {
    return loadStaticGltfScene(path, fallbackAlbedoPath);
  }
  if (extension == ".glb") {
    return loadStaticGlbScene(path, fallbackAlbedoPath);
  }
  throw std::runtime_error("Unsupported static model asset extension: " +
                           path.string());
}

RenderQueueBuckets buildRenderQueues(SceneEcs const &sceneEcs,
                                     AssetLibrary const &assets,
                                     glm::vec3 const &cameraPosition) {
  RenderQueueBuckets buckets;
  buckets.opaque.reserve(sceneEcs.sceneObjectCount());
  buckets.mask.reserve(sceneEcs.sceneObjectCount());
  buckets.transparent.reserve(sceneEcs.sceneObjectCount());

  sceneEcs.forEachSceneObject([&](std::size_t objectIndex, Entity,
                                  TransformComponent const &transform,
                                  RenderableComponent const &renderable,
                                  BoundsComponent const &bounds) {
    if (renderable.meshId >= assets.meshes.size()) {
      throw std::runtime_error("ECS renderable mesh id is out of range.");
    }
    if (renderable.materialId >= assets.materials.size()) {
      throw std::runtime_error("ECS renderable material id is out of range.");
    }

    RenderQueueItem item{
        .objectIndex = objectIndex,
        .meshId = renderable.meshId,
        .materialId = renderable.materialId,
        .modelMatrix = transform.transform.matrix(),
        .worldBounds = bounds.worldBounds,
    };
    if (item.worldBounds.valid) {
      glm::vec3 const center =
          (item.worldBounds.min + item.worldBounds.max) * 0.5f;
      glm::vec3 const delta = center - cameraPosition;
      item.sortDepthSq = glm::dot(delta, delta);
    }

    Material const &material = assets.materials[renderable.materialId];
    switch (material.alphaMode) {
    case AlphaMode::Opaque:
      buckets.opaque.push_back(item);
      break;
    case AlphaMode::Mask:
      buckets.mask.push_back(item);
      break;
    case AlphaMode::Blend:
      buckets.transparent.push_back(item);
      break;
    }
  });

  return buckets;
}

glm::vec4 matrixRow(glm::mat4 const &matrix, int row) {
  return {matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]};
}

glm::vec4 normalizePlane(glm::vec4 plane) {
  float const length = glm::length(glm::vec3{plane});
  if (length <= 0.0f) {
    return plane;
  }
  return plane / length;
}

Frustum extractFrustum(glm::mat4 const &viewProjMatrix) {
  glm::vec4 const row0 = matrixRow(viewProjMatrix, 0);
  glm::vec4 const row1 = matrixRow(viewProjMatrix, 1);
  glm::vec4 const row2 = matrixRow(viewProjMatrix, 2);
  glm::vec4 const row3 = matrixRow(viewProjMatrix, 3);

  return Frustum{{
      normalizePlane(row3 + row0),
      normalizePlane(row3 - row0),
      normalizePlane(row3 + row1),
      normalizePlane(row3 - row1),
      normalizePlane(row2),
      normalizePlane(row3 - row2),
  }};
}

bool intersectsFrustum(Frustum const &frustum, Aabb const &bounds) {
  if (!bounds.valid) {
    return true;
  }

  for (glm::vec4 const &plane : frustum.planes) {
    glm::vec3 const positiveVertex{
        plane.x >= 0.0f ? bounds.max.x : bounds.min.x,
        plane.y >= 0.0f ? bounds.max.y : bounds.min.y,
        plane.z >= 0.0f ? bounds.max.z : bounds.min.z,
    };

    if (glm::dot(glm::vec3{plane}, positiveVertex) + plane.w < 0.0f) {
      return false;
    }
  }

  return true;
}

std::vector<RenderQueueItem>
filterRenderQueueByFrustum(std::vector<RenderQueueItem> const &items,
                           Frustum const &frustum, std::size_t &culledItems) {
  std::vector<RenderQueueItem> visibleItems;
  visibleItems.reserve(items.size());
  culledItems = 0;

  for (RenderQueueItem const &item : items) {
    if (intersectsFrustum(frustum, item.worldBounds)) {
      visibleItems.push_back(item);
    } else {
      ++culledItems;
    }
  }

  return visibleItems;
}

void sortTransparentQueue(std::vector<RenderQueueItem> &items) {
  std::stable_sort(items.begin(), items.end(),
                   [](RenderQueueItem const &lhs, RenderQueueItem const &rhs) {
                     if (lhs.sortDepthSq != rhs.sortDepthSq) {
                       return lhs.sortDepthSq > rhs.sortDepthSq;
                     }
                     return lhs.objectIndex < rhs.objectIndex;
                   });
}

VisibleRenderQueueBuckets
filterVisibleRenderQueues(RenderQueueBuckets const &queues,
                          Frustum const &frustum) {
  VisibleRenderQueueBuckets visible{};
  visible.opaque =
      filterRenderQueueByFrustum(queues.opaque, frustum, visible.culledCount);
  std::size_t maskCulled = 0;
  visible.mask = filterRenderQueueByFrustum(queues.mask, frustum, maskCulled);
  visible.culledCount += maskCulled;
  std::size_t transparentCulled = 0;
  visible.transparent = filterRenderQueueByFrustum(queues.transparent, frustum,
                                                   transparentCulled);
  visible.culledCount += transparentCulled;
  sortTransparentQueue(visible.transparent);
  return visible;
}

Aabb emptyBounds() {
  return {
      .min = glm::vec3{std::numeric_limits<float>::max()},
      .max = glm::vec3{std::numeric_limits<float>::lowest()},
      .valid = false,
  };
}

void includePoint(Aabb &bounds, glm::vec3 point) {
  bounds.min = glm::min(bounds.min, point);
  bounds.max = glm::max(bounds.max, point);
  bounds.valid = true;
}

Aabb computeMeshBounds(Mesh const &mesh) {
  Aabb bounds = emptyBounds();
  for (Vertex const &vertex : mesh.vertices) {
    includePoint(bounds, vertex.position);
  }
  return bounds;
}

Aabb transformBounds(Aabb const &localBounds, glm::mat4 const &matrix) {
  if (!localBounds.valid) {
    return {};
  }

  Aabb worldBounds = emptyBounds();
  for (int x = 0; x < 2; ++x) {
    for (int y = 0; y < 2; ++y) {
      for (int z = 0; z < 2; ++z) {
        glm::vec3 corner{
            x == 0 ? localBounds.min.x : localBounds.max.x,
            y == 0 ? localBounds.min.y : localBounds.max.y,
            z == 0 ? localBounds.min.z : localBounds.max.z,
        };
        includePoint(worldBounds, glm::vec3(matrix * glm::vec4(corner, 1.0f)));
      }
    }
  }
  return worldBounds;
}
} // namespace

struct Application::SceneCandidate {
  AssetLibrary assets;
  SceneEcs ecs;
  Camera camera;
  std::vector<Camera> cameras;
  glm::vec3 center{0};
  float radius = 1;
  std::filesystem::path path;
  std::string pathText;
  std::vector<std::string> warnings;
  Renderer::PreparedScene gpu;
};

struct Application::SceneLoad {
  std::filesystem::path path;
  std::future<SceneCandidate> preparation;
  std::optional<SceneCandidate> candidate;
  bool discard = false;
};

Application::Application(ViewerOptions options)
    : options_(std::move(options)), startupScenePath_(options_.scene) {}

Application::~Application() = default;

void Application::run() {
  initWindow();

  try {
    initVulkan();
    mainLoop();
    finishBenchmark();
    cleanup();
  } catch (...) {
    cleanup();
    throw;
  }
}

void Application::initWindow() {
  if (glfwInit() != GLFW_TRUE) {
    char const *errorDescription = nullptr;
    int const errorCode = glfwGetError(&errorDescription);
    std::string message = "Failed to initialize GLFW.";
    if (errorCode != GLFW_NO_ERROR && errorDescription != nullptr) {
      message += " ";
      message += errorDescription;
    }
    char const *display = std::getenv("DISPLAY");
    char const *waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if ((display == nullptr || std::strlen(display) == 0) &&
        (waylandDisplay == nullptr || std::strlen(waylandDisplay) == 0)) {
      message += " DISPLAY and WAYLAND_DISPLAY are both unset.";
    }
    throw std::runtime_error(message);
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE,
                 options_.benchmarkDirectory ? GLFW_FALSE : GLFW_TRUE);
  if (options_.benchmarkDirectory) {
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
  }
  window_ = glfwCreateWindow(options_.width, options_.height, "Vulkan", nullptr,
                             nullptr);
  if (window_ == nullptr) {
    throw std::runtime_error("Failed to create GLFW window.");
  }

  glfwSetWindowUserPointer(window_, this);
  glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
  glfwSetWindowFocusCallback(window_, windowFocusCallback);
  glfwSetCursorEnterCallback(window_, cursorEnterCallback);
  glfwSetMouseButtonCallback(window_, mouseButtonCallback);
  glfwSetCursorPosCallback(window_, cursorPositionCallback);
  glfwSetScrollCallback(window_, scrollCallback);
  glfwSetKeyCallback(window_, keyCallback);
  glfwSetCharCallback(window_, charCallback);
  glfwSetDropCallback(window_, dropCallback);
}

void Application::initVulkan() {
  createInstance();
  setupDebugMessenger();
  createSurface();
  createScene();

  requiredDeviceExtensions_ = {vk::KHRSwapchainExtensionName};
  device_ =
      std::make_unique<Device>(instance_, surface_, requiredDeviceExtensions_,
                               options_.gpu, debugUtilsEnabled_);
  swapChain_ = std::make_unique<SwapChain>(*device_, surface_, window_, nullptr,
                                           options_.present);
  renderer_ = std::make_unique<Renderer>(*device_);
  renderer_->recreateForSwapChain(*swapChain_);
  auto loadStart = std::chrono::steady_clock::now();
  if (startupScenePath_) {
    loadScene(*startupScenePath_);
    advanceSceneLoad(true); // Cold startup, before UI/benchmark frames exist.
  }
  benchmarkMetadata_.loadMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - loadStart)
                                  .count();
  if (options_.ui)
    initImGui();
  auto props = device_->physicalDevice().getProperties();
  std::cout << "Vulkan device: " << props.deviceName << " ("
            << vk::to_string(props.deviceType) << "), API "
            << VK_VERSION_MAJOR(props.apiVersion) << '.'
            << VK_VERSION_MINOR(props.apiVersion)
            << ", GPU timestamps: " << renderer_->gpuTimingSupported()
            << ", validation: " << validationLayersEnabled_ << '\n';
  if (options_.benchmarkDirectory) {
    if (std::filesystem::exists(*options_.benchmarkDirectory /
                                "summary.json") ||
        std::filesystem::exists(*options_.benchmarkDirectory / "frames.csv"))
      throw std::runtime_error(
          "Benchmark output exists; select a new directory");
    auto &m = benchmarkMetadata_;
    m.projectSourceSha256 = VULKAN_SOURCE_SHA256;
    m.gpu = props.deviceName.data();
    m.deviceType = vk::to_string(props.deviceType);
    m.software = props.deviceType == vk::PhysicalDeviceType::eCpu;
    auto driver = device_->physicalDevice()
                      .getProperties2<vk::PhysicalDeviceProperties2,
                                      vk::PhysicalDeviceDriverProperties>();
    m.driver =
        std::string(driver.get<vk::PhysicalDeviceDriverProperties>()
                        .driverName.data()) +
        " / " +
        driver.get<vk::PhysicalDeviceDriverProperties>().driverInfo.data() +
        " (raw " + std::to_string(props.driverVersion) + ")";
    m.api = std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." +
            std::to_string(VK_VERSION_MINOR(props.apiVersion)) + "." +
            std::to_string(VK_VERSION_PATCH(props.apiVersion));
    m.scene = loadedScenePath_.string();
    m.cameraPath = options_.cameraPath;
    m.presentMode = vk::to_string(swapChain_->presentMode());
    m.buildType = VULKAN_BUILD_TYPE;
    m.width = swapChain_->extent().width;
    m.height = swapChain_->extent().height;
    if (m.width != options_.width || m.height != options_.height)
      throw std::runtime_error("Benchmark framebuffer is " +
                               std::to_string(m.width) + "x" +
                               std::to_string(m.height) + "; requested " +
                               std::to_string(options_.width) + "x" +
                               std::to_string(options_.height));
    m.validation = validationLayersEnabled_;
    m.ui = options_.ui;
    m.warmupSeconds = options_.warmupSeconds;
    m.requestedSeconds = options_.durationSeconds;
    auto memory = device_->physicalDevice().getMemoryProperties();
    for (unsigned i = 0; i < memory.memoryHeapCount; ++i)
      if (memory.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal)
        m.deviceLocalBytes += memory.memoryHeaps[i].size;
    m.memoryBudget = device_->memoryBudgetSupported();
    auto [usage, budget] = device_->memoryUsageBudget();
    m.sampledPeakHeapUsage = std::max(m.sampledPeakHeapUsage, usage);
    m.lastHeapBudget = budget;
    benchmarkCamera_ = scene_.cameras[scene_.activeCameraIndex];
    m.cameraPosition = {benchmarkCamera_.position.x,
                        benchmarkCamera_.position.y,
                        benchmarkCamera_.position.z};
    m.cameraTarget = {benchmarkCamera_.target.x, benchmarkCamera_.target.y,
                      benchmarkCamera_.target.z};
    m.fovRadians = benchmarkCamera_.fovRadians;
    m.nearPlane = benchmarkCamera_.nearPlane;
    m.farPlane = benchmarkCamera_.farPlane;
    m.environmentIntensity = scene_.lighting.environmentIntensity;
    m.exposureEv = scene_.lighting.exposureEv;
    m.toneMappingEnabled = scene_.lighting.toneMappingEnabled;
  }
}

void Application::mainLoop() {
  benchmarkStart_ = lastFrameTime_ = std::chrono::steady_clock::now();
  double lastMemorySample = -1;
  while (!glfwWindowShouldClose(window_)) {
    auto const now = std::chrono::steady_clock::now();
    float const deltaSeconds =
        std::chrono::duration<float>(now - lastFrameTime_).count();
    lastFrameTime_ = now;
    frameTimeMs_ = deltaSeconds * 1000.0f;
    framesPerSecond_ = deltaSeconds > 0.0f ? 1.0f / deltaSeconds : 0.0f;

    double elapsed =
        std::chrono::duration<double>(now - benchmarkStart_).count();
    if (options_.benchmarkDirectory &&
        elapsed >= options_.warmupSeconds + options_.durationSeconds) {
      benchmarkMetadata_.completed = true;
      break;
    }
    if (options_.benchmarkDirectory && benchmarkMetadata_.memoryBudget &&
        elapsed - lastMemorySample >= 1) {
      auto [usage, budget] = device_->memoryUsageBudget();
      benchmarkMetadata_.sampledPeakHeapUsage =
          std::max(benchmarkMetadata_.sampledPeakHeapUsage, usage);
      benchmarkMetadata_.lastHeapBudget = budget;
      lastMemorySample = elapsed;
    }
    glfwPollEvents();
    processPendingScene();
    beginImGuiFrame();
    updateScene();
    if (scene_.cameras.empty()) {
      throw std::runtime_error("Scene has no cameras.");
    }

    if (scene_.activeCameraIndex >= scene_.cameras.size()) {
      throw std::runtime_error("Active camera index is out of range.");
    }

    float aspect = static_cast<float>(swapChain_->extent().width) /
                   static_cast<float>(swapChain_->extent().height);

    auto &camera = scene_.cameras[scene_.activeCameraIndex];
    if (options_.benchmarkDirectory) {
      camera = benchmarkCamera_;
      if (options_.cameraPath == "orbit") {
        double trajectoryTime = std::max(0.0, elapsed - options_.warmupSeconds);
        float angle = static_cast<float>(std::floor(trajectoryTime * 60.0) /
                                         60.0 * 0.3141592653589793);
        auto offset = benchmarkCamera_.position - benchmarkCamera_.target;
        camera.position =
            camera.target +
            glm::vec3(std::cos(angle) * offset.x + std::sin(angle) * offset.z,
                      offset.y,
                      -std::sin(angle) * offset.x + std::cos(angle) * offset.z);
      }
    } else {
      orbitCameraController_.updateFromInput(input_, deltaSeconds);
      orbitCameraController_.update(camera);
    }
    input_.clearFrameDeltas();
    glm::mat4 viewProjMatrix = camera.viewProj(aspect);
    renderer_->setSurfaceDebugEnabled(normalMapDebugEnabled_,
                                      parallaxDebugEnabled_);
    RenderQueueBuckets renderQueues =
        buildRenderQueues(sceneEcs_, assets_, camera.position);
    VisibleRenderQueueBuckets visibleRenderQueues{
        .opaque = renderQueues.opaque,
        .mask = renderQueues.mask,
        .transparent = renderQueues.transparent,
    };
    sortTransparentQueue(visibleRenderQueues.transparent);
    if (frustumCullingEnabled_) {
      Frustum const cameraFrustum = extractFrustum(viewProjMatrix);
      visibleRenderQueues =
          filterVisibleRenderQueues(renderQueues, cameraFrustum);
    }
    renderQueueItems_ = renderQueues.opaque.size() + renderQueues.mask.size() +
                        renderQueues.transparent.size();
    visibleRenderQueueItems_ = visibleRenderQueues.opaque.size() +
                               visibleRenderQueues.mask.size() +
                               visibleRenderQueues.transparent.size();
    culledRenderQueueItems_ = visibleRenderQueues.culledCount;
    visibleOpaqueItems_ = visibleRenderQueues.opaque.size();
    visibleMaskItems_ = visibleRenderQueues.mask.size();
    visibleTransparentItems_ = visibleRenderQueues.transparent.size();
    frameDrawCalls_ = 0;
    shadowDrawCalls_ = 0;
    mainDrawCalls_ = 0;
    debugDrawCalls_ = 0;

    LightingSettings frameLighting = scene_.lighting;
    bool const shadowPassEnabled = shadowDebugEnabled_;
    if (!shadowPassEnabled) {
      frameLighting.shadowDebugMode = 0;
    }

    auto prepareEnd = std::chrono::steady_clock::now();
    auto beginResult = renderer_->beginFrame(viewProjMatrix, camera.position,
                                             frameLighting, shadowPassEnabled);
    if (beginResult != Renderer::FrameResult::eSuccess) {
      if (options_.benchmarkDirectory)
        throw std::runtime_error("Benchmark interrupted during image "
                                 "acquisition; rerun at fixed resolution");
      recreateSwapChain();
      continue;
    }

    harvestBenchmarkTimings();
    if (shadowPassEnabled) {
      for (RenderQueueItem const &item : renderQueues.opaque) {
        renderer_->drawObject(item.meshId, item.materialId, item.modelMatrix);
        ++frameDrawCalls_;
        ++shadowDrawCalls_;
      }
      for (RenderQueueItem const &item : renderQueues.mask) {
        renderer_->drawObject(item.meshId, item.materialId, item.modelMatrix);
        ++frameDrawCalls_;
        ++shadowDrawCalls_;
      }

      renderer_->beginMainPass();
    }

    renderer_->drawEnvironment();

    for (RenderQueueItem const &item : visibleRenderQueues.opaque) {
      renderer_->drawObject(item.meshId, item.materialId, item.modelMatrix);
      ++frameDrawCalls_;
      ++mainDrawCalls_;
    }
    for (RenderQueueItem const &item : visibleRenderQueues.mask) {
      renderer_->drawObject(item.meshId, item.materialId, item.modelMatrix);
      ++frameDrawCalls_;
      ++mainDrawCalls_;
    }
    for (RenderQueueItem const &item : visibleRenderQueues.transparent) {
      renderer_->drawObject(item.meshId, item.materialId, item.modelMatrix);
      ++frameDrawCalls_;
      ++mainDrawCalls_;
    }

    if (showAabbDebug_) {
      for (RenderQueueItem const &item : visibleRenderQueues.opaque) {
        renderer_->drawAabb(item.worldBounds, {0.1f, 0.95f, 0.65f, 0.95f});
        ++frameDrawCalls_;
        ++debugDrawCalls_;
      }
      for (RenderQueueItem const &item : visibleRenderQueues.mask) {
        renderer_->drawAabb(item.worldBounds, {0.1f, 0.95f, 0.65f, 0.95f});
        ++frameDrawCalls_;
        ++debugDrawCalls_;
      }
      for (RenderQueueItem const &item : visibleRenderQueues.transparent) {
        renderer_->drawAabb(item.worldBounds, {0.1f, 0.95f, 0.65f, 0.95f});
        ++frameDrawCalls_;
        ++debugDrawCalls_;
      }
    }

    auto frameResult = renderer_->endFrame();
    if (options_.benchmarkDirectory && elapsed >= options_.warmupSeconds) {
      auto const &sync = renderer_->cpuSyncTimes();
      BenchmarkFrame sample{
          .id = renderer_->submittedFrameId(),
          .elapsedSeconds = elapsed - options_.warmupSeconds,
          .cpuFrameMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - now)
                            .count(),
          .cpuPrepareMs =
              std::chrono::duration<double, std::milli>(prepareEnd - now)
                  .count(),
          .fenceMs = sync.fenceMs,
          .acquireMs = sync.acquireMs,
          .submitMs = sync.submitMs,
          .presentMs = sync.presentMs,
          .shadowDraws = shadowDrawCalls_,
          .mainDraws = mainDrawCalls_};
      benchmarkFrameIndices_[sample.id] = benchmarkFrames_.size();
      benchmarkFrames_.push_back(sample);
    }
    if (frameResult != Renderer::FrameResult::eSuccess || framebufferResized_) {
      if (options_.benchmarkDirectory)
        throw std::runtime_error("Benchmark interrupted by swapchain change; "
                                 "rerun at fixed resolution");
      recreateSwapChain();
    }
  }
}

void Application::cleanup() {
  // Join the one preparation task before its borrowed Renderer/Device die.
  sceneLoad_.reset();
  if (device_) {
    device_->logicalDevice().waitIdle();
    if (renderer_)
      renderer_->collectCompletedWork(); // Release retired previews before ImGui shutdown.
  }

  cleanupImGui();

  // Wayland presentation objects still reference the window/display. Destroy
  // Vulkan consumers before GLFW tears down those native objects.
  renderer_.reset();
  swapChain_.reset();
  device_.reset();
  surface_.clear();
  debugMessenger_.clear();
  instance_.clear();

  if (window_ != nullptr) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
  glfwTerminate();
}

void Application::recreateSwapChain() {
  if (window_ == nullptr) {
    throw std::runtime_error("Cannot recreate swapchain without a window.");
  }

  if (!device_) {
    throw std::runtime_error("Cannot recreate swapchain without a device.");
  }

  if (!renderer_) {
    throw std::runtime_error("Cannot recreate swapchain without a renderer.");
  }

  if (!swapChain_) {
    throw std::runtime_error("Cannot recreate swapchain without a swapchain.");
  }

  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);

  while (width == 0 || height == 0) {
    glfwWaitEvents();
    glfwGetFramebufferSize(window_, &width, &height);
  }
  device_->logicalDevice().waitIdle();

  vk::SwapchainKHR oldSwapChainHandle = *swapChain_->handle();
  auto newSwapChain = std::make_unique<SwapChain>(
      *device_, surface_, window_, oldSwapChainHandle, options_.present);
  renderer_->recreateForSwapChain(*newSwapChain);

  swapChain_.swap(newSwapChain);
  framebufferResized_ = false;
}

void Application::initImGui() {
  if (imguiInitialized_) {
    return;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForVulkan(window_, false);

  VkFormat colorAttachmentFormat =
      static_cast<VkFormat>(swapChain_->imageFormat());
  VkPipelineRenderingCreateInfo pipelineRenderingCreateInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &colorAttachmentFormat,
  };

  ImGui_ImplVulkan_InitInfo initInfo{};
  initInfo.ApiVersion = VK_API_VERSION_1_3;
  initInfo.Instance = static_cast<VkInstance>(device_->instanceHandle());
  initInfo.PhysicalDevice =
      static_cast<VkPhysicalDevice>(device_->physicalDeviceHandle());
  initInfo.Device = static_cast<VkDevice>(device_->deviceHandle());
  initInfo.QueueFamily = device_->graphicsQueueFamilyIndex();
  initInfo.Queue = static_cast<VkQueue>(device_->graphicsQueueHandle());
  initInfo.DescriptorPoolSize = 64;
  initInfo.MinImageCount = 2;
  initInfo.ImageCount = static_cast<std::uint32_t>(swapChain_->images().size());
  initInfo.UseDynamicRendering = true;
  initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  initInfo.PipelineInfoMain.PipelineRenderingCreateInfo =
      pipelineRenderingCreateInfo;
  ImGui_ImplVulkan_Init(&initInfo);

  renderer_->setUiDrawCallback([](vk::CommandBuffer commandBuffer) {
    ImGui_ImplVulkan_RenderDrawData(
        ImGui::GetDrawData(), static_cast<VkCommandBuffer>(commandBuffer));
  });

  imguiInitialized_ = true;
}

void Application::beginImGuiFrame() {
  if (!imguiInitialized_) {
    return;
  }

  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  ImGui::BeginDisabled(options_.benchmarkDirectory.has_value());
  drawImGui();
  ImGui::EndDisabled();
  ImGui::Render();
}

void Application::drawImGui() {
  ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                               ImGuiDockNodeFlags_PassthruCentralNode);
  drawSceneBrowser();

  if (ImGui::Begin("Camera")) {
    Camera &camera = scene_.cameras[scene_.activeCameraIndex];
    ImGui::Text("Position: %.2f, %.2f, %.2f", camera.position.x,
                camera.position.y, camera.position.z);
    ImGui::Text("Target: %.2f, %.2f, %.2f", camera.target.x, camera.target.y,
                camera.target.z);

    float fovDegrees = glm::degrees(camera.fovRadians);
    if (ImGui::SliderFloat("FOV", &fovDegrees, 20.0f, 90.0f, "%.1f deg")) {
      camera.fovRadians = glm::radians(fovDegrees);
    }
    ImGui::DragFloat("Near", &camera.nearPlane, 0.01f, 0.01f,
                     camera.farPlane - 0.01f);
    ImGui::DragFloat("Far", &camera.farPlane, 0.1f, camera.nearPlane + 0.01f,
                     200.0f);

    float moveSpeed = orbitCameraController_.moveSpeed();
    if (ImGui::SliderFloat("Move Speed", &moveSpeed, 0.1f, 20.0f)) {
      orbitCameraController_.setMoveSpeed(moveSpeed);
    }
    float sensitivity = orbitCameraController_.rotateSensitivity();
    if (ImGui::SliderFloat("Look Sensitivity", &sensitivity, 0.0005f, 0.05f,
                           "%.4f")) {
      orbitCameraController_.setRotateSensitivity(sensitivity);
    }

    if (ImGui::Button("Reset Camera")) {
      camera = sceneCamera_;
      orbitCameraController_.reset(camera);
    }

    ImGui::TextUnformatted("Right mouse: look");
    ImGui::TextUnformatted("Right mouse + WASD/Space/Ctrl: fly");
    ImGui::TextUnformatted("Wheel: speed");
  }
  ImGui::End();

  if (ImGui::Begin("Lighting")) {
    ImGui::TextUnformatted("Directional Cook-Torrance PBR");
    ImGui::DragFloat3("Direction", &scene_.lighting.direction.x, 0.01f);
    ImGui::SliderFloat("Intensity", &scene_.lighting.intensity, 0.0f, 4.0f);
    ImGui::ColorEdit3("Color", &scene_.lighting.color.x);
    ImGui::SliderFloat("Diffuse", &scene_.lighting.diffuseStrength, 0.0f, 2.0f);
    ImGui::SliderFloat("Specular", &scene_.lighting.specularStrength, 0.0f,
                       4.0f);
    char const *pbrDebugModes[] = {"Final",     "Base Color",  "Metallic",
                                   "Roughness", "Normal",      "AO",
                                   "Emissive",  "Diffuse IBL", "Specular IBL"};
    ImGui::Combo("PBR Debug", &scene_.lighting.pbrDebugMode, pbrDebugModes,
                 static_cast<int>(std::size(pbrDebugModes)));
    ImGui::SliderFloat("Environment Intensity",
                       &scene_.lighting.environmentIntensity, 0.0f, 8.0f,
                       "%.2f");
    ImGui::SliderFloat("Exposure EV", &scene_.lighting.exposureEv, -8.0f, 8.0f, "%.2f");
    ImGui::Checkbox("Tone Mapping", &scene_.lighting.toneMappingEnabled);
    ImGui::SliderAngle("Environment Rotation",
                       &scene_.lighting.environmentRotation, -180.0f, 180.0f);
    ImGui::SliderFloat("IBL Diffuse",
                       &scene_.lighting.environmentDiffuseStrength, 0.0f, 4.0f,
                       "%.2f");
    ImGui::SliderFloat("IBL Specular",
                       &scene_.lighting.environmentSpecularStrength, 0.0f, 4.0f,
                       "%.2f");

    ImGui::Separator();
    ImGui::TextUnformatted("Shadow");
    char const *shadowModes[] = {"Off", "Lit", "Visibility", "Depth Map"};
    int shadowMode = scene_.lighting.shadowDebugMode;
    if (ImGui::Combo("Mode", &shadowMode, shadowModes, 4)) {
      scene_.lighting.shadowDebugMode = shadowMode;
    }
    ImGui::SliderFloat("Bias Slope", &scene_.lighting.shadowBiasSlope, 0.0f,
                       0.02f, "%.5f");
    ImGui::SliderFloat("Bias Constant", &scene_.lighting.shadowBiasConstant,
                       0.0f, 0.005f, "%.5f");
    ImGui::SliderFloat("PCF Radius", &scene_.lighting.shadowPcfRadius, 0.0f,
                       4.0f, "%.2f");
    ImGui::DragFloat("Light Distance", &scene_.lighting.shadowLightDistance,
                     0.05f, 0.1f, 50.0f, "%.2f");
    ImGui::DragFloat("Ortho Extent", &scene_.lighting.shadowOrthoExtent, 0.05f,
                     0.1f, 50.0f, "%.2f");
    ImGui::DragFloat("Shadow Near", &scene_.lighting.shadowNearPlane, 0.01f,
                     0.001f, scene_.lighting.shadowFarPlane - 0.001f, "%.3f");
    ImGui::DragFloat("Shadow Far", &scene_.lighting.shadowFarPlane, 0.05f,
                     scene_.lighting.shadowNearPlane + 0.001f, 100.0f, "%.2f");
  }
  ImGui::End();

  if (ImGui::Begin("Material")) {
    if (assets_.materials.empty()) {
      ImGui::TextUnformatted("No materials");
    } else {
      if (selectedMaterialIndex_ >= assets_.materials.size()) {
        selectedMaterialIndex_ = 0;
      }

      int selectedMaterial = static_cast<int>(selectedMaterialIndex_);
      int const maxMaterialIndex =
          static_cast<int>(assets_.materials.size() - 1);
      ImGui::SliderInt("Material", &selectedMaterial, 0, maxMaterialIndex);
      selectedMaterialIndex_ = static_cast<std::size_t>(selectedMaterial);

      Material &material = assets_.materials[selectedMaterialIndex_];
      if (!material.name.empty())
        ImGui::TextWrapped("%s", material.name.c_str());
      auto textureLabel = [](std::string const &path,
                             std::vector<std::byte> const &bytes) {
        return !bytes.empty() ? "embedded image"
               : path.empty() ? "flat default"
                              : path.c_str();
      };
      ImGui::TextWrapped(
          "Albedo (UV %d): %s", material.albedoTexCoord,
          textureLabel(material.albedoPath, material.albedoBytes));
      ImGui::TextWrapped(
          "Normal (UV %d): %s", material.normalTexCoord,
          textureLabel(material.normalPath, material.normalBytes));
      ImGui::TextWrapped("Metallic-Roughness (UV %d): %s",
                         material.metallicRoughnessTexCoord,
                         textureLabel(material.metallicRoughnessPath,
                                      material.metallicRoughnessBytes));
      ImGui::TextWrapped(
          "Occlusion (UV %d): %s", material.occlusionTexCoord,
          textureLabel(material.occlusionPath, material.occlusionBytes));
      ImGui::TextWrapped(
          "Emissive (UV %d): %s", material.emissiveTexCoord,
          textureLabel(material.emissivePath, material.emissiveBytes));
      ImGui::Text("Metallic: %.3f", material.metallicFactor);
      ImGui::Text("Roughness: %.3f", material.roughnessFactor);
      ImGui::Text("AO Strength: %.3f", material.occlusionStrength);
      ImGui::Text("Emissive Factor: %.3f, %.3f, %.3f",
                  material.emissiveFactor.x, material.emissiveFactor.y,
                  material.emissiveFactor.z);
      ImGui::Text("Height: %s", material.heightPath.empty()
                                    ? "flat default"
                                    : material.heightPath.c_str());
      ImGui::Text("Alpha: %s", material.alphaPath.empty()
                                   ? "alpha from base color"
                                   : material.alphaPath.c_str());
      ImGui::Text("Mode: %s", alphaModeLabel(material.alphaMode));
      ImGui::Text("Double-sided: %s", material.doubleSided ? "Yes" : "No");
      if (ImGui::ColorEdit4("Tint", &material.tint.x)) {
        renderer_->setMaterialTint(
            static_cast<MaterialId>(selectedMaterialIndex_), material.tint);
      }
      bool surfaceChanged = false;
      surfaceChanged |= ImGui::SliderFloat(
          "Normal Strength", &material.normalScale, 0.0f, 2.0f, "%.2f");
      surfaceChanged |= ImGui::SliderFloat(
          "Parallax Scale", &material.parallaxScale, 0.0f, 0.12f, "%.3f");
      if (surfaceChanged) {
        renderer_->setMaterialSurfaceParams(
            static_cast<MaterialId>(selectedMaterialIndex_),
            material.normalScale, material.parallaxScale);
      }

      int alphaMode = static_cast<int>(material.alphaMode);
      char const *alphaModeLabels[] = {"Opaque", "Mask", "Blend"};
      bool alphaChanged = false;
      if (ImGui::Combo("Alpha Mode", &alphaMode, alphaModeLabels, 3)) {
        material.alphaMode = static_cast<AlphaMode>(alphaMode);
        alphaChanged = true;
      }
      alphaChanged |= ImGui::SliderFloat("Alpha Cutoff", &material.alphaCutoff,
                                         0.0f, 1.0f, "%.2f");
      if (alphaChanged) {
        renderer_->setMaterialAlphaParams(
            static_cast<MaterialId>(selectedMaterialIndex_), material.alphaMode,
            material.alphaCutoff);
      }

      ImGui::Separator();
      ImGui::TextUnformatted("Preview");
      ImVec2 const previewSize{128.0f, 128.0f};
      ImTextureID const albedoPreview = materialAlbedoPreviewTexture(
          static_cast<MaterialId>(selectedMaterialIndex_));
      if (albedoPreview != 0) {
        ImGui::TextUnformatted("Albedo");
        ImGui::Image(albedoPreview, previewSize);
      }

      if (renderer_->materialAlphaTexture(
              static_cast<MaterialId>(selectedMaterialIndex_)) != nullptr) {
        ImTextureID const alphaPreview = materialAlphaPreviewTexture(
            static_cast<MaterialId>(selectedMaterialIndex_));
        if (alphaPreview != 0) {
          ImGui::TextUnformatted("Alpha Mask");
          ImGui::Image(alphaPreview, previewSize);
        }
      }
    }
  }
  ImGui::End();

  if (ImGui::Begin("Render Debug")) {
    Renderer::RasterizerDebugSettings settings =
        renderer_->rasterizerDebugSettings();

    ImGui::Text("FPS: %.1f", framesPerSecond_);
    ImGui::Text("CPU loop cadence: %.2f ms", frameTimeMs_);
    auto const &gpu = renderer_->gpuTimings();
    if (gpu.valid) {
      ImGui::Text("GPU frame #%llu: %.3f ms",
                  static_cast<unsigned long long>(gpu.frameId), gpu.totalMs);
      ImGui::Text("GPU shadow / main: %.3f / %.3f ms", gpu.shadowMs, gpu.mainMs);
      ImGui::Text("GPU output / UI: %.3f / %.3f ms", gpu.outputMs, gpu.uiMs);
    } else
      ImGui::TextUnformatted("GPU timing: pending or unsupported");
    auto const &sync = renderer_->cpuSyncTimes();
    ImGui::Text("CPU fence / acquire / present call: %.3f / %.3f / %.3f ms",
                sync.fenceMs, sync.acquireMs, sync.presentMs);
    ImGui::Text("Device: %s",
                device_->physicalDevice().getProperties().deviceName.data());
    ImGui::Separator();
    ImGui::Checkbox("Shadows", &shadowDebugEnabled_);
    ImGui::Checkbox("Normal/Bump Mapping", &normalMapDebugEnabled_);
    ImGui::Checkbox("Parallax Mapping", &parallaxDebugEnabled_);
    ImGui::Checkbox("Frustum Culling", &frustumCullingEnabled_);
    ImGui::Checkbox("Show AABBs", &showAabbDebug_);
    ImGui::Text("Objects: %zu", sceneEcs_.entityCount());
    ImGui::Text("ECS Entities: %zu", sceneEcs_.entityCount());
    ImGui::Text("ECS Transforms: %zu", sceneEcs_.transformCount());
    ImGui::Text("ECS Renderables: %zu", sceneEcs_.renderableCount());
    ImGui::Text("ECS Bounds: %zu", sceneEcs_.boundsCount());
    ImGui::Text("Render Queue: %zu", renderQueueItems_);
    ImGui::Text("Visible: %zu", visibleRenderQueueItems_);
    ImGui::Text("Culled: %zu", culledRenderQueueItems_);
    ImGui::Text("Visible Opaque: %zu", visibleOpaqueItems_);
    ImGui::Text("Visible Mask: %zu", visibleMaskItems_);
    ImGui::Text("Visible Transparent: %zu", visibleTransparentItems_);
    auto const &resources = renderer_->resourceStatistics();
    ImGui::Text("Scene commits / environment uploads / pipeline builds: %llu / "
                "%llu / %llu",
                static_cast<unsigned long long>(resources.sceneCommits),
                static_cast<unsigned long long>(resources.environmentUploads),
                static_cast<unsigned long long>(resources.pipelineBuilds));
    ImGui::Text("Last scene upload: %llu submits, %llu images, %llu buffers, %.2f MiB",
                static_cast<unsigned long long>(resources.lastSceneUpload.submissions),
                static_cast<unsigned long long>(resources.lastSceneUpload.imageCopies),
                static_cast<unsigned long long>(resources.lastSceneUpload.bufferCopies),
                static_cast<double>(resources.lastSceneUpload.bytes) / (1024.0 * 1024.0));
    ImGui::Text("Pending uploads: %zu | Retired scenes: %zu | Completed frame: %llu",
                resources.pendingSceneUploads, resources.retiredScenes,
                static_cast<unsigned long long>(resources.completedFrameId));
    auto ledger = renderer_->resourceSnapshot();
    auto const mib = [](std::uint64_t bytes) { return double(bytes) / (1024 * 1024); };
    ImGui::Text("Owned GPU allocations: %.2f MiB | Peak: %.2f MiB | Payload: %.2f MiB",
                mib(ledger.current.allocatedBytes), mib(ledger.peak.allocatedBytes),
                mib(ledger.current.payloadBytes));
    ImGui::Text("Resource suballocations: %.2f MiB | Peak: %.2f MiB",
                mib(ledger.current.suballocatedBytes), mib(ledger.peak.suballocatedBytes));
    if (ImGui::BeginTable("Resource ledger", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthFixed, 130.0f);
      ImGui::TableSetupColumn("Backing MiB", ImGuiTableColumnFlags_WidthFixed, 95.0f);
      ImGui::TableSetupColumn("Resource range MiB", ImGuiTableColumnFlags_WidthFixed, 105.0f);
      for (auto label : {"Buffers", "Images", "Samplers"})
        ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableHeadersRow();
      for (std::size_t i = 0; i < ResourceLedger::domainCount; ++i) {
        auto const &row = ledger.domains[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        auto name = ResourceLedger::name(static_cast<ResourceLedger::Domain>(i));
        ImGui::TextUnformatted(name.data(), name.data() + name.size());
        ImGui::TableNextColumn(); ImGui::Text("%.2f", mib(row.allocatedBytes));
        ImGui::TableNextColumn(); ImGui::Text("%.2f", mib(row.suballocatedBytes));
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(row.buffers));
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(row.images));
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(row.samplers));
      }
      ImGui::EndTable();
    }
    ImGui::TextWrapped("Backing memory counts unique blocks shared by buffers and images. Resource ranges are separate; swapchain, ImGui backend and driver overhead are excluded. Heap usage is separate.");
    ImGui::Text("Meshes: %zu", assets_.meshes.size());
    ImGui::Text("Materials: %zu", assets_.materials.size());
    ImGui::Text("Draw Calls: %zu", frameDrawCalls_);
    ImGui::Text("Shadow Draws: %zu", shadowDrawCalls_);
    ImGui::Text("Main Draws: %zu", mainDrawCalls_);
    ImGui::Text("Debug Draws: %zu", debugDrawCalls_);
    ImGui::Separator();

    int cullMode = 0;
    if (settings.cullMode == vk::CullModeFlagBits::eBack) {
      cullMode = 1;
    } else if (settings.cullMode == vk::CullModeFlagBits::eFront) {
      cullMode = 2;
    }

    bool changed = false;
    char const *cullLabels[] = {"None", "Back", "Front"};
    if (ImGui::Combo("Single-sided Cull Mode", &cullMode, cullLabels, 3)) {
      if (cullMode == 1) {
        settings.cullMode = vk::CullModeFlagBits::eBack;
      } else if (cullMode == 2) {
        settings.cullMode = vk::CullModeFlagBits::eFront;
      } else {
        settings.cullMode = vk::CullModeFlagBits::eNone;
      }
      changed = true;
    }

    int frontFace =
        settings.frontFace == vk::FrontFace::eCounterClockwise ? 0 : 1;
    char const *frontFaceLabels[] = {"Counter-clockwise", "Clockwise"};
    if (ImGui::Combo("Front Face", &frontFace, frontFaceLabels, 2)) {
      settings.frontFace = frontFace == 0 ? vk::FrontFace::eCounterClockwise
                                          : vk::FrontFace::eClockwise;
      changed = true;
    }

    if (changed) {
      device_->logicalDevice().waitIdle();
      renderer_->setRasterizerDebugSettings(settings);
    }
  }
  ImGui::End();
}

void Application::cleanupImGui() {
  if (!imguiInitialized_) {
    return;
  }

  clearMaterialPreviewTextures();
  renderer_->setUiDrawCallback({});
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  imguiInitialized_ = false;
}

void Application::clearMaterialPreviewTextures() {
  for (ImTextureID textureId : materialAlbedoPreviewTextures_) {
    if (textureId != 0) {
      ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)textureId);
    }
  }
  for (ImTextureID textureId : materialAlphaPreviewTextures_) {
    if (textureId != 0) {
      ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)textureId);
    }
  }
  materialAlbedoPreviewTextures_.clear();
  materialAlphaPreviewTextures_.clear();
}

ImTextureID Application::materialAlbedoPreviewTexture(MaterialId materialId) {
  if (!imguiInitialized_ || renderer_ == nullptr ||
      materialId >= assets_.materials.size()) {
    return 0;
  }

  if (materialAlbedoPreviewTextures_.size() != assets_.materials.size()) {
    clearMaterialPreviewTextures();
    materialAlbedoPreviewTextures_.resize(assets_.materials.size(), 0);
    materialAlphaPreviewTextures_.resize(assets_.materials.size(), 0);
  }

  ImTextureID &cached = materialAlbedoPreviewTextures_[materialId];
  if (cached == 0) {
    TextureResources const &texture =
        renderer_->materialAlbedoTexture(materialId);
    cached = (ImTextureID)ImGui_ImplVulkan_AddTexture(
        texture.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  return cached;
}

ImTextureID Application::materialAlphaPreviewTexture(MaterialId materialId) {
  if (!imguiInitialized_ || renderer_ == nullptr ||
      materialId >= assets_.materials.size()) {
    return 0;
  }

  if (materialAlphaPreviewTextures_.size() != assets_.materials.size()) {
    clearMaterialPreviewTextures();
    materialAlbedoPreviewTextures_.resize(assets_.materials.size(), 0);
    materialAlphaPreviewTextures_.resize(assets_.materials.size(), 0);
  }

  ImTextureID &cached = materialAlphaPreviewTextures_[materialId];
  if (cached == 0) {
    TextureResources const *texture =
        renderer_->materialAlphaTexture(materialId);
    if (texture == nullptr) {
      return 0;
    }
    cached = (ImTextureID)ImGui_ImplVulkan_AddTexture(
        texture->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  return cached;
}

void Application::framebufferResizeCallback(GLFWwindow *window, int width,
                                            int height) {
  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app != nullptr) {
    app->framebufferResized_ = true;
  }
}

void Application::windowFocusCallback(GLFWwindow *window, int focused) {
  if (ImGui::GetCurrentContext())
    ImGui_ImplGlfw_WindowFocusCallback(window, focused);

  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app != nullptr && focused == GLFW_FALSE) {
    app->input_.clearAll();
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
  }
}

void Application::cursorEnterCallback(GLFWwindow *window, int entered) {
  if (ImGui::GetCurrentContext())
    ImGui_ImplGlfw_CursorEnterCallback(window, entered);

  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app != nullptr) {
    app->input_.cursorEntered = entered == GLFW_TRUE;
  }
}

void Application::mouseButtonCallback(GLFWwindow *window, int button,
                                      int action, int mods) {
  if (ImGui::GetCurrentContext())
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);

  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app == nullptr) {
    return;
  }

  if (button >= 0 &&
      button < static_cast<int>(app->input_.mouseButtons.size())) {
    if (action == GLFW_PRESS) {
      app->input_.mouseButtons[button] = true;
    } else if (action == GLFW_RELEASE) {
      app->input_.mouseButtons[button] = false;
    }
  }

  if (button != GLFW_MOUSE_BUTTON_RIGHT) {
    return;
  }

  if (action == GLFW_PRESS) {
    app->input_.rightMouseCaptured = true;
    glfwGetCursorPos(window, &app->input_.cursorX, &app->input_.cursorY);
    app->input_.cursorDeltaX = 0.0;
    app->input_.cursorDeltaY = 0.0;
    app->input_.hasCursorPosition = true;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  } else if (action == GLFW_RELEASE) {
    app->input_.rightMouseCaptured = false;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
  }
}

void Application::cursorPositionCallback(GLFWwindow *window, double xpos,
                                         double ypos) {
  if (ImGui::GetCurrentContext())
    ImGui_ImplGlfw_CursorPosCallback(window, xpos, ypos);

  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app == nullptr) {
    return;
  }

  if (app->input_.hasCursorPosition) {
    app->input_.cursorDeltaX += xpos - app->input_.cursorX;
    app->input_.cursorDeltaY += ypos - app->input_.cursorY;
  }
  app->input_.cursorX = xpos;
  app->input_.cursorY = ypos;
  app->input_.hasCursorPosition = true;
}

void Application::scrollCallback(GLFWwindow *window, double xoffset,
                                 double yoffset) {
  if (ImGui::GetCurrentContext())
    ImGui_ImplGlfw_ScrollCallback(window, xoffset, yoffset);

  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app == nullptr) {
    return;
  }

  app->input_.scrollDeltaX += xoffset;
  app->input_.scrollDeltaY += yoffset;
}

void Application::keyCallback(GLFWwindow *window, int key, int scancode,
                              int action, int mods) {
  if (ImGui::GetCurrentContext())
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);

  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app == nullptr || key < 0 ||
      key >= static_cast<int>(app->input_.keys.size())) {
    return;
  }

  if (action == GLFW_PRESS) {
    app->input_.keys[key] = true;
  } else if (action == GLFW_RELEASE) {
    app->input_.keys[key] = false;
  }
}

void Application::charCallback(GLFWwindow *window, unsigned int codepoint) {
  if (ImGui::GetCurrentContext())
    ImGui_ImplGlfw_CharCallback(window, codepoint);
}

void Application::createInstance() {
  constexpr vk::ApplicationInfo appInfo{
      .pApplicationName = "Hello Triangle",
      .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
      .pEngineName = "Fool Engine",
      .apiVersion = vk::ApiVersion13,
  };

  std::vector<char const *> requiredLayers;
  validationLayersEnabled_ = kEnableValidationLayers || options_.validation;
  if (validationLayersEnabled_) {
    requiredLayers.assign(kValidationLayers.begin(), kValidationLayers.end());
  }

  auto layerProperties = context_.enumerateInstanceLayerProperties();
  auto unsupportedLayerIt = std::ranges::find_if(
      requiredLayers, [&layerProperties](auto const &requiredLayer) {
        return std::ranges::none_of(
            layerProperties, [requiredLayer](auto const &layerProperty) {
              return std::strcmp(layerProperty.layerName, requiredLayer) == 0;
            });
      });

  if (unsupportedLayerIt != requiredLayers.end()) {
    std::cerr << "Validation layer not supported (" << *unsupportedLayerIt
              << "), continuing without validation layers.\n";
    validationLayersEnabled_ = false;
    requiredLayers.clear();
  }

  auto requiredExtensions = getRequiredInstanceExtensions();
  auto extensionProperties = context_.enumerateInstanceExtensionProperties();
  auto unsupportedExtensionIt = std::ranges::find_if(
      requiredExtensions,
      [&extensionProperties](auto const &requiredExtension) {
        return std::ranges::none_of(
            extensionProperties,
            [requiredExtension](auto const &extensionProperty) {
              return std::strcmp(extensionProperty.extensionName,
                                 requiredExtension) == 0;
            });
      });

  if (unsupportedExtensionIt != requiredExtensions.end()) {
    throw std::runtime_error("Required extension not supported: " +
                             std::string(*unsupportedExtensionIt));
  }

  vk::InstanceCreateInfo createInfo{
      .pApplicationInfo = &appInfo,
      .enabledLayerCount = static_cast<std::uint32_t>(requiredLayers.size()),
      .ppEnabledLayerNames = requiredLayers.data(),
      .enabledExtensionCount =
          static_cast<std::uint32_t>(requiredExtensions.size()),
      .ppEnabledExtensionNames = requiredExtensions.data(),
  };

  instance_ = vk::raii::Instance(context_, createInfo);
}

void Application::setupDebugMessenger() {
  if (!validationLayersEnabled_ || !debugUtilsEnabled_) {
    return;
  }

  vk::DebugUtilsMessageSeverityFlagsEXT severityFlags(
      vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
      vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);
  vk::DebugUtilsMessageTypeFlagsEXT messageTypeFlags(
      vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
      vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance |
      vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation);

  vk::DebugUtilsMessengerCreateInfoEXT createInfo{
      .messageSeverity = severityFlags,
      .messageType = messageTypeFlags,
      .pfnUserCallback = &debugCallback,
      .pUserData = this,
  };

  debugMessenger_ = instance_.createDebugUtilsMessengerEXT(createInfo);
}

void Application::createSurface() {
  VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
  if (glfwCreateWindowSurface(*instance_, window_, nullptr, &rawSurface) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create window surface.");
  }

  surface_ = vk::raii::SurfaceKHR(instance_, rawSurface);
}

void Application::updateScene() {
  auto const now = std::chrono::steady_clock::now();
  float const elapsedSeconds =
      std::chrono::duration<float>(now - animationStartTime_).count();

  sceneEcs_.forEachSceneObject(
      [&](std::size_t objectIndex, Entity, TransformComponent &transform,
          RenderableComponent const &renderable, BoundsComponent &bounds) {
        if (animateScene_ && objectIndex == 1) {
          transform.transform.rotation.z = glm::radians(45.0f) * elapsedSeconds;
        }
        if (renderable.meshId >= assets_.meshes.size()) {
          return;
        }
        Mesh &mesh = assets_.meshes[renderable.meshId];
        if (!mesh.localBounds.valid) {
          mesh.localBounds = computeMeshBounds(mesh);
        }
        bounds.worldBounds =
            transformBounds(mesh.localBounds, transform.transform.matrix());
      });
}

void Application::createScene() {
  browserDirectory_ = std::filesystem::absolute(browserDirectory_);
  scene_.cameras = {Camera{}};
  scene_.activeCameraIndex = 0;
  scene_.lighting = LightingSettings{};
  orbitCameraController_.attach(scene_.cameras.front());
}

void Application::loadScene(std::filesystem::path const &path) {
  if (sceneLoad_)
    throw std::runtime_error("A scene preparation is already in progress.");
  auto job = std::make_unique<SceneLoad>();
  job->path = std::filesystem::absolute(path).lexically_normal();
  auto *renderer = renderer_.get();
  job->preparation = std::async(std::launch::async, [renderer, path = job->path] {
    if (!std::filesystem::is_regular_file(path))
      throw std::runtime_error("Scene file not found: " + path.string());
    ImportedScene loaded = loadStaticModelScene(path, {});
    if (loaded.meshes.empty() || loaded.materials.empty() || loaded.objects.empty())
      throw std::runtime_error("Imported scene has no drawable geometry.");
    Aabb bounds{};
    for (auto const &object : loaded.objects) {
      if (!object.worldBounds.valid)
        continue;
      if (!bounds.valid)
        bounds = object.worldBounds;
      else {
        bounds.min = glm::min(bounds.min, object.worldBounds.min);
        bounds.max = glm::max(bounds.max, object.worldBounds.max);
      }
    }
    SceneCandidate result;
    result.center = bounds.valid ? (bounds.min + bounds.max) * 0.5f : glm::vec3{0};
    result.radius = bounds.valid ? std::max(glm::length(bounds.max - bounds.min) * 0.5f, 0.1f) : 1.0f;
    result.camera.target = result.center;
    result.camera.position = result.center + glm::normalize(glm::vec3{1.0f, 0.6f, 1.0f}) * result.radius * 2.8f;
    result.camera.nearPlane = std::max(result.radius * 0.001f, 0.001f);
    result.camera.farPlane = std::max(result.radius * 20.0f, 10.0f);
    result.ecs.rebuildFromSceneObjects(loaded.objects);
    result.assets = AssetLibrary{.meshes = std::move(loaded.meshes), .materials = std::move(loaded.materials)};
    result.cameras = {result.camera};
    result.path = path;
    result.pathText = path.string();
    result.warnings = std::move(loaded.warnings);
    result.gpu = renderer->prepareScene(result.assets);
    return result;
  });
  sceneLoad_ = std::move(job);
  sceneLoadError_.clear();
}

void Application::advanceSceneLoad(bool waitForStartup) {
  if (!sceneLoad_)
    return;
  auto &job = *sceneLoad_;
  if (!job.candidate) {
    if (!waitForStartup && job.preparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return;
    if (job.discard) {
      // A superseded/explicitly canceled error must not overwrite live UI state.
      try { (void)job.preparation.get(); } catch (...) {}
      sceneLoad_.reset();
      return;
    }
    job.candidate.emplace(job.preparation.get());
  }
  if (job.discard) {
    sceneLoad_.reset(); // No worker remains; Renderer retains any submitted upload.
    return;
  }
  auto &next = *job.candidate;
  if (waitForStartup)
    renderer_->waitSceneUpload(next.gpu);
  // Snapshot descriptor IDs before commit. No allocation can occur after the
  // GPU state changes. Detach containers immediately, remove IDs only at retirement.
  std::function<void()> retireUi;
  if (!materialAlbedoPreviewTextures_.empty() || !materialAlphaPreviewTextures_.empty()) {
    retireUi = [albedo = materialAlbedoPreviewTextures_, alpha = materialAlphaPreviewTextures_] {
      for (auto id : albedo)
        if (id) ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)id);
      for (auto id : alpha)
        if (id) ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)id);
    };
  }
  auto [usage, budget] = device_->memoryUsageBudget();
  benchmarkMetadata_.sampledPeakHeapUsage = std::max(benchmarkMetadata_.sampledPeakHeapUsage, usage);
  benchmarkMetadata_.lastHeapBudget = budget;
  if (!renderer_->commitScene(next.gpu, std::move(retireUi)))
    return;
  materialAlbedoPreviewTextures_.clear();
  materialAlphaPreviewTextures_.clear();
  static_assert(std::is_nothrow_move_assignable_v<AssetLibrary>);
  static_assert(std::is_nothrow_move_assignable_v<SceneEcs>);
  assets_ = std::move(next.assets);
  sceneEcs_ = std::move(next.ecs);
  scene_.cameras.swap(next.cameras);
  sceneCamera_ = next.camera;
  scene_.activeCameraIndex = 0;
  orbitCameraController_.attach(scene_.cameras.front());
  orbitCameraController_.setMoveSpeed(std::max(next.radius * 0.5f, 0.1f));
  animateScene_ = false;
  selectedMaterialIndex_ = 0;
  scene_.lighting.shadowTarget = next.center;
  scene_.lighting.shadowOrthoExtent = next.radius * 1.1f;
  scene_.lighting.shadowLightDistance = next.radius * 2.5f;
  scene_.lighting.shadowNearPlane = next.camera.nearPlane;
  scene_.lighting.shadowFarPlane = next.radius * 5.0f;
  scene_.lighting.pbrDebugMode = 0;
  scene_.lighting.shadowDebugMode = 1;
  loadedScenePath_.swap(next.path);
  sceneLoadWarnings_.swap(next.warnings);
  std::snprintf(scenePathInput_.data(), scenePathInput_.size(), "%s", next.pathText.c_str());
  sceneLoadError_.clear();
  std::cerr << "Loaded scene: " << next.pathText << " (" << assets_.meshes.size()
            << " meshes, " << assets_.materials.size() << " materials)\n";
  for (auto const &warning : sceneLoadWarnings_)
    std::cerr << "Scene warning: " << warning << '\n';
  sceneLoad_.reset();
}

void Application::cancelSceneLoad() {
  pendingScenePath_.reset();
  if (sceneLoad_)
    sceneLoad_->discard = true;
}

void Application::processPendingScene() {
  renderer_->collectCompletedWork();
  if (options_.benchmarkDirectory) {
    pendingScenePath_.reset();
    return;
  }
  if (pendingScenePath_ && sceneLoad_)
    sceneLoad_->discard = true; // Latest request wins; never destroy an active future.
  try {
    advanceSceneLoad();
    if (!sceneLoad_ && pendingScenePath_) {
      auto path = std::move(*pendingScenePath_);
      pendingScenePath_.reset();
      loadScene(path);
    }
  } catch (std::exception const &error) {
    sceneLoadError_ = error.what();
    sceneLoad_.reset(); // get() has completed; submitted resources remain retained.
    std::cerr << "Scene load failed: " << sceneLoadError_ << '\n';
  }
}

void Application::dropCallback(GLFWwindow *window, int count,
                               char const **paths) {
  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app != nullptr && count > 0)
    app->pendingScenePath_ = paths[0];
}

void Application::drawSceneBrowser() {
  ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(350, 400), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Scene")) {
    ImGui::TextWrapped("%s", loadedScenePath_.empty()
                                 ? "Choose a model to begin"
                                 : loadedScenePath_.string().c_str());
    bool submit = ImGui::InputText("Model path", scenePathInput_.data(),
                                   scenePathInput_.size(),
                                   ImGuiInputTextFlags_EnterReturnsTrue);
    if ((ImGui::Button("Load Model") || submit) && scenePathInput_[0] != '\0')
      pendingScenePath_ = std::filesystem::path{scenePathInput_.data()};
    ImGui::SameLine();
    if (ImGui::Button("Reload") && !loadedScenePath_.empty())
      pendingScenePath_ = loadedScenePath_;
    if (sceneLoad_) {
      ImGui::TextWrapped("%s: %s", sceneLoad_->discard ? "Canceling" : "Loading",
                         sceneLoad_->path.string().c_str());
      if (ImGui::Button("Cancel load"))
        cancelSceneLoad();
    }
    ImGui::TextUnformatted("glTF / GLB / OBJ; drop a file onto the window");
    if (!sceneLoadError_.empty())
      ImGui::TextWrapped("Load failed: %s", sceneLoadError_.c_str());
    for (auto const &warning : sceneLoadWarnings_)
      ImGui::TextWrapped("Scene warning: %s", warning.c_str());
    ImGui::Separator();
    ImGui::TextWrapped("Browse: %s", browserDirectory_.string().c_str());
    if (ImGui::Button("Parent") && browserDirectory_.has_parent_path())
      browserDirectory_ = browserDirectory_.parent_path();
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> entries;
    for (std::filesystem::directory_iterator it(browserDirectory_, ec), end;
         !ec && it != end; it.increment(ec))
      entries.push_back(*it);
    if (ec)
      ImGui::TextWrapped("Cannot browse: %s", ec.message().c_str());
    std::ranges::sort(entries, {}, [](auto const &entry) {
      return entry.path().filename().string();
    });
    ImGui::BeginChild("Model files", ImVec2(0, 160), ImGuiChildFlags_Borders);
    for (auto const &entry : entries) {
      std::error_code typeError;
      bool const directory = entry.is_directory(typeError);
      auto const extension = lowercase(entry.path().extension().string());
      if (!directory && extension != ".gltf" && extension != ".glb" &&
          extension != ".obj")
        continue;
      std::string const label =
          (directory ? "[Folder] " : "") + entry.path().filename().string();
      if (ImGui::Selectable(label.c_str())) {
        if (directory)
          browserDirectory_ = entry.path();
        else
          pendingScenePath_ = entry.path();
      }
    }
    ImGui::EndChild();
  }
  ImGui::End();
}

std::vector<char const *> Application::getRequiredInstanceExtensions() {
  std::uint32_t glfwExtensionCount = 0;
  auto glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

  std::vector<char const *> extensions(glfwExtensions,
                                       glfwExtensions + glfwExtensionCount);
  auto available = context_.enumerateInstanceExtensionProperties();
  debugUtilsEnabled_ = std::ranges::any_of(available, [](auto const &e) {
    return std::strcmp(e.extensionName, vk::EXTDebugUtilsExtensionName) == 0;
  });
  if (debugUtilsEnabled_)
    extensions.push_back(vk::EXTDebugUtilsExtensionName);

  return extensions;
}

VKAPI_ATTR vk::Bool32 VKAPI_CALL Application::debugCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
    vk::DebugUtilsMessageTypeFlagsEXT type,
    vk::DebugUtilsMessengerCallbackDataEXT const *callbackData, void *user) {
  if (auto *app = static_cast<Application *>(user)) {
    if (severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
      ++app->validationErrors_;
    else if (severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
      ++app->validationWarnings_;
  }
  if (severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError ||
      severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
    std::cerr << "validation layer: type " << vk::to_string(type)
              << " msg: " << callbackData->pMessage << '\n';
  }

  return vk::False;
}

void Application::harvestBenchmarkTimings() {
  if (!options_.benchmarkDirectory)
    return;
  auto const &gpu = renderer_->gpuTimings();
  auto it = benchmarkFrameIndices_.find(gpu.frameId);
  if (it != benchmarkFrameIndices_.end())
    benchmarkFrames_[it->second].gpu = gpu;
}
void Application::finishBenchmark() {
  if (!options_.benchmarkDirectory)
    return;
  benchmarkMetadata_.measuredSeconds =
      std::max(0.0, std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - benchmarkStart_)
                            .count() -
                        options_.warmupSeconds);
  device_->logicalDevice().waitIdle();
  renderer_->collectCompletedWork();
  harvestBenchmarkTimings();
  auto [usage, budget] = device_->memoryUsageBudget();
  benchmarkMetadata_.sampledPeakHeapUsage =
      std::max(benchmarkMetadata_.sampledPeakHeapUsage, usage);
  benchmarkMetadata_.lastHeapBudget = budget;
  benchmarkMetadata_.validationErrors = validationErrors_;
  benchmarkMetadata_.validationWarnings = validationWarnings_;
  benchmarkMetadata_.engineResources = renderer_->resourceSnapshot();
  writeBenchmarkReport(*options_.benchmarkDirectory, benchmarkMetadata_,
                       benchmarkFrames_);
  std::cout << "Benchmark: " << options_.benchmarkDirectory->string() << ", "
            << benchmarkFrames_.size() << " measured frames\n";
}
