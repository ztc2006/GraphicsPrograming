#include "pch.hpp"

#include "application.hpp"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>

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
constexpr std::uint32_t kWindowWidth = 800;
constexpr std::uint32_t kWindowHeight = 600;

const std::vector<char const *> kValidationLayers = {
    "VK_LAYER_KHRONOS_validation"};

#ifdef NDEBUG
constexpr bool kEnableValidationLayers = false;
#else
constexpr bool kEnableValidationLayers = true;
#endif

struct StaticModelAsset {
  std::filesystem::path path;
  std::string fallbackAlbedoPath;
  Camera camera;
};

std::string lowercase(std::string value) {
  for (char &c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

LoadedScene loadStaticModelScene(std::filesystem::path const &path,
                                 std::string const &fallbackAlbedoPath) {
  std::string const extension = lowercase(path.extension().string());
  if (extension == ".obj") {
    return loadStaticObjScene(path, fallbackAlbedoPath);
  }
  if (extension == ".gltf") {
    return loadStaticGltfScene(path, fallbackAlbedoPath);
  }
  if (extension == ".glb") {
    throw std::runtime_error(
        "Binary glTF .glb files are not supported yet: " + path.string());
  }
  throw std::runtime_error("Unsupported static model asset extension: " +
                           path.string());
}

void logLoadedScene(std::filesystem::path const &path,
                    LoadedScene const &loaded) {
  std::cerr << "Loaded static scene: " << path.string() << " ("
            << loaded.meshes.size() << " meshes, " << loaded.materials.size()
            << " materials, " << loaded.objects.size() << " objects)\n";
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

Application::Application() = default;

Application::~Application() = default;

void Application::run() {
  initWindow();

  try {
    initVulkan();
    mainLoop();
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
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
  window_ =
      glfwCreateWindow(kWindowWidth, kWindowHeight, "Vulkan", nullptr, nullptr);
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
}

void Application::initVulkan() {
  createInstance();
  setupDebugMessenger();
  createSurface();
  createScene();

  requiredDeviceExtensions_ = {vk::KHRSwapchainExtensionName};
  device_ =
      std::make_unique<Device>(instance_, surface_, requiredDeviceExtensions_);
  swapChain_ = std::make_unique<SwapChain>(*device_, surface_, window_);
  renderer_ = std::make_unique<Renderer>(*device_);
  if (scene_.objects.empty()) {
    throw std::runtime_error("Scene has no objects.");
  }
  if (scene_.meshes.empty()) {
    throw std::runtime_error("Scene has no meshes.");
  }
  renderer_->setMeshes(scene_.meshes);
  if (scene_.materials.empty()) {
    throw std::runtime_error("Scene has no materials.");
  }
  renderer_->setMaterials(scene_.materials);
  renderer_->recreateForSwapChain(*swapChain_);
  initImGui();
}

void Application::mainLoop() {
  while (!glfwWindowShouldClose(window_)) {
    auto const now = std::chrono::steady_clock::now();
    float const deltaSeconds =
        std::chrono::duration<float>(now - lastFrameTime_).count();
    lastFrameTime_ = now;
    frameTimeMs_ = deltaSeconds * 1000.0f;
    framesPerSecond_ = deltaSeconds > 0.0f ? 1.0f / deltaSeconds : 0.0f;

    glfwPollEvents();
    beginImGuiFrame();
    updateScene();
    if (scene_.cameras.empty()) {
      throw std::runtime_error("Scene has no cameras.");
    }

    if (scene_.activeCameraIndex >= scene_.cameras.size()) {
      throw std::runtime_error("Active camera index is out of range.");
    }

    if (scene_.objects.empty()) {
      throw std::runtime_error("Scene has no objects.");
    }

    float aspect = static_cast<float>(swapChain_->extent().width) /
                   static_cast<float>(swapChain_->extent().height);

    auto &camera = scene_.cameras[scene_.activeCameraIndex];
    orbitCameraController_.updateFromInput(input_, deltaSeconds);
    orbitCameraController_.update(camera);
    input_.clearFrameDeltas();
    glm::mat4 viewProjMatrix = camera.viewProj(aspect);

    auto beginResult =
        renderer_->beginFrame(viewProjMatrix, camera.position, scene_.lighting);
    if (beginResult != Renderer::FrameResult::eSuccess) {
      recreateSwapChain();
      continue;
    }

    for (SceneObject const &object : scene_.objects) {
      if (object.meshId >= scene_.meshes.size()) {
        throw std::runtime_error("Scene object mesh id is out of range.");
      }
      if (object.materialId >= scene_.materials.size()) {
        throw std::runtime_error("Scene object material id is out of range.");
      }
      renderer_->drawObject(object.meshId, object.materialId,
                            object.transform.matrix());
    }

    renderer_->beginMainPass();

    for (SceneObject const &object : scene_.objects) {
      renderer_->drawObject(object.meshId, object.materialId,
                            object.transform.matrix());
    }

    if (showAabbDebug_) {
      for (SceneObject const &object : scene_.objects) {
        renderer_->drawAabb(object.worldBounds, {0.1f, 0.95f, 0.65f, 0.95f});
      }
    }

    auto frameResult = renderer_->endFrame();
    if (frameResult != Renderer::FrameResult::eSuccess || framebufferResized_) {
      recreateSwapChain();
    }
  }
}

void Application::cleanup() {
  if (device_) {
    device_->logicalDevice().waitIdle();
  }

  cleanupImGui();

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
  auto newSwapChain = std::make_unique<SwapChain>(*device_, surface_, window_,
                                                  oldSwapChainHandle);
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
  drawImGui();
  ImGui::Render();
}

void Application::drawImGui() {
  ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                               ImGuiDockNodeFlags_PassthruCentralNode);

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
      camera = Camera{};
      orbitCameraController_.reset(camera);
    }

    ImGui::TextUnformatted("Right mouse: look");
    ImGui::TextUnformatted("Right mouse + WASD/Space/Ctrl: fly");
    ImGui::TextUnformatted("Wheel: speed");
  }
  ImGui::End();

  if (ImGui::Begin("Lighting")) {
    ImGui::TextUnformatted("Directional Blinn-Phong");
    ImGui::DragFloat3("Direction", &scene_.lighting.direction.x, 0.01f);
    ImGui::SliderFloat("Intensity", &scene_.lighting.intensity, 0.0f, 4.0f);
    ImGui::ColorEdit3("Color", &scene_.lighting.color.x);
    ImGui::SliderFloat("Ambient", &scene_.lighting.ambientStrength, 0.0f, 0.5f);
    ImGui::SliderFloat("Diffuse", &scene_.lighting.diffuseStrength, 0.0f, 2.0f);
    ImGui::SliderFloat("Specular", &scene_.lighting.specularStrength, 0.0f,
                       4.0f);
    ImGui::SliderFloat("Shininess", &scene_.lighting.shininess, 1.0f, 128.0f);

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
    if (scene_.materials.empty()) {
      ImGui::TextUnformatted("No materials");
    } else {
      if (selectedMaterialIndex_ >= scene_.materials.size()) {
        selectedMaterialIndex_ = 0;
      }

      int selectedMaterial = static_cast<int>(selectedMaterialIndex_);
      int const maxMaterialIndex =
          static_cast<int>(scene_.materials.size() - 1);
      ImGui::SliderInt("Material", &selectedMaterial, 0, maxMaterialIndex);
      selectedMaterialIndex_ = static_cast<std::size_t>(selectedMaterial);

      Material &material = scene_.materials[selectedMaterialIndex_];
      ImGui::Text("Albedo: %s", material.albedoPath.c_str());
      ImGui::Text("Normal: %s",
                  material.normalPath.empty() ? "flat default"
                                              : material.normalPath.c_str());
      ImGui::Text("Height: %s",
                  material.heightPath.empty() ? "flat default"
                                              : material.heightPath.c_str());
      if (ImGui::ColorEdit4("Tint", &material.tint.x)) {
        renderer_->setMaterialTint(
            static_cast<MaterialId>(selectedMaterialIndex_), material.tint);
      }
      bool surfaceChanged = false;
      surfaceChanged |=
          ImGui::SliderFloat("Normal Strength", &material.normalScale, 0.0f,
                             2.0f, "%.2f");
      surfaceChanged |=
          ImGui::SliderFloat("Parallax Scale", &material.parallaxScale, 0.0f,
                             0.12f, "%.3f");
      if (surfaceChanged) {
        renderer_->setMaterialSurfaceParams(
            static_cast<MaterialId>(selectedMaterialIndex_),
            material.normalScale, material.parallaxScale);
      }
    }
  }
  ImGui::End();

  if (ImGui::Begin("Render Debug")) {
    Renderer::RasterizerDebugSettings settings =
        renderer_->rasterizerDebugSettings();

    ImGui::Text("FPS: %.1f", framesPerSecond_);
    ImGui::Text("Frame: %.2f ms", frameTimeMs_);
    ImGui::Separator();
    ImGui::Checkbox("Show AABBs", &showAabbDebug_);
    ImGui::Text("Objects: %zu", scene_.objects.size());
    ImGui::Separator();

    int cullMode = 0;
    if (settings.cullMode == vk::CullModeFlagBits::eBack) {
      cullMode = 1;
    } else if (settings.cullMode == vk::CullModeFlagBits::eFront) {
      cullMode = 2;
    }

    bool changed = false;
    char const *cullLabels[] = {"None", "Back", "Front"};
    if (ImGui::Combo("Cull Mode", &cullMode, cullLabels, 3)) {
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

  renderer_->setUiDrawCallback({});
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  imguiInitialized_ = false;
}

void Application::framebufferResizeCallback(GLFWwindow *window, int width,
                                            int height) {
  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app != nullptr) {
    app->framebufferResized_ = true;
  }
}

void Application::windowFocusCallback(GLFWwindow *window, int focused) {
  ImGui_ImplGlfw_WindowFocusCallback(window, focused);

  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app != nullptr && focused == GLFW_FALSE) {
    app->input_.clearAll();
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
  }
}

void Application::cursorEnterCallback(GLFWwindow *window, int entered) {
  ImGui_ImplGlfw_CursorEnterCallback(window, entered);

  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app != nullptr) {
    app->input_.cursorEntered = entered == GLFW_TRUE;
  }
}

void Application::mouseButtonCallback(GLFWwindow *window, int button,
                                      int action, int mods) {
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
  ImGui_ImplGlfw_CharCallback(window, codepoint);
}

void Application::createInstance() {
  constexpr vk::ApplicationInfo appInfo{
      .pApplicationName = "Hello Triangle",
      .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
      .pEngineName = "Fool Engine",
      .apiVersion = vk::ApiVersion14,
  };

  std::vector<char const *> requiredLayers;
  validationLayersEnabled_ = kEnableValidationLayers;
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
  if (!validationLayersEnabled_) {
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
  if (animateScene_ && scene_.objects.size() >= 2) {
    auto const now = std::chrono::steady_clock::now();
    float const elapsedSeconds =
        std::chrono::duration<float>(now - animationStartTime_).count();
    scene_.objects[1].transform.rotation.z =
        glm::radians(45.0f) * elapsedSeconds;
  }

  for (SceneObject &object : scene_.objects) {
    if (object.meshId >= scene_.meshes.size()) {
      continue;
    }
    Mesh &mesh = scene_.meshes[object.meshId];
    if (!mesh.localBounds.valid) {
      mesh.localBounds = computeMeshBounds(mesh);
    }
    object.worldBounds =
        transformBounds(mesh.localBounds, object.transform.matrix());
  }
}

void Application::createScene() {
  scene_.meshes.clear();
  scene_.materials.clear();
  scene_.objects.clear();
  scene_.cameras.clear();
  animateScene_ = false;

  auto finalizeLoadedScene = [this](LoadedScene loaded, Camera camera) {
    scene_.meshes = std::move(loaded.meshes);
    scene_.materials = std::move(loaded.materials);
    scene_.objects = std::move(loaded.objects);
    scene_.cameras.push_back(camera);
    scene_.activeCameraIndex = 0;
    orbitCameraController_.attach(scene_.cameras[scene_.activeCameraIndex]);
    scene_.lighting = LightingSettings{};
  };

  Camera sponzaCamera{};
  sponzaCamera.position = {0.0f, 1.4f, 6.0f};
  sponzaCamera.target = {0.0f, 1.2f, 0.0f};
  sponzaCamera.farPlane = 100.0f;

  std::array defaultAssets = {
      StaticModelAsset{
          .path = "assets/models/sponza_2/sponza.obj",
          .fallbackAlbedoPath = "texture/image.jpg",
          .camera = sponzaCamera,
      },
  };

  char const *requestedScenePath = std::getenv("FOOL_ENGINE_SCENE");
  if (requestedScenePath != nullptr && std::strlen(requestedScenePath) > 0) {
    StaticModelAsset requestedAsset{
        .path = requestedScenePath,
        .fallbackAlbedoPath = "texture/image.jpg",
        .camera = sponzaCamera,
    };
    if (!std::filesystem::exists(requestedAsset.path)) {
      throw std::runtime_error("Requested scene asset does not exist: " +
                               requestedAsset.path.string());
    }

    LoadedScene loaded = loadStaticModelScene(requestedAsset.path,
                                             requestedAsset.fallbackAlbedoPath);
    logLoadedScene(requestedAsset.path, loaded);
    finalizeLoadedScene(std::move(loaded), requestedAsset.camera);
    return;
  }

  for (StaticModelAsset const &asset : defaultAssets) {
    if (!std::filesystem::exists(asset.path)) {
      continue;
    }

    LoadedScene loaded =
        loadStaticModelScene(asset.path, asset.fallbackAlbedoPath);
    logLoadedScene(asset.path, loaded);
    finalizeLoadedScene(std::move(loaded), asset.camera);
    return;
  }

  std::filesystem::path const debugGltfPath{"assets/debug_scene.gltf"};
  if (std::filesystem::exists(debugGltfPath)) {
    LoadedScene loaded =
        loadStaticModelScene(debugGltfPath, "texture/image.jpg");
    logLoadedScene(debugGltfPath, loaded);
    finalizeLoadedScene(std::move(loaded), Camera{});
    return;
  }

  animateScene_ = true;

  scene_.meshes.push_back(Mesh{
      .vertices =
          {
              {{-0.5f, -0.5f, 0.0f},
               {0.95f, 0.30f, 0.25f},
               {0.0f, 0.0f, 1.0f},
               {0.0f, 1.0f}},
              {{0.5f, -0.5f, 0.0f},
               {0.20f, 0.75f, 0.35f},
               {0.0f, 0.0f, 1.0f},
               {1.0f, 1.0f}},
              {{0.5f, 0.5f, 0.0f},
               {0.15f, 0.45f, 0.95f},
               {0.0f, 0.0f, 1.0f},
               {1.0f, 0.0f}},
              {{-0.5f, 0.5f, 0.0f},
               {0.98f, 0.82f, 0.20f},
               {0.0f, 0.0f, 1.0f},
               {0.0f, 0.0f}},
          },
      .indices = {0, 1, 2, 2, 3, 0},
  });

  scene_.meshes.push_back(Mesh{
      .vertices =
          {
              {{0.0f, -0.55f, 0.0f},
               {0.95f, 0.40f, 0.20f},
               {0.0f, 0.0f, 1.0f},
               {0.5f, 1.0f}},
              {{0.55f, 0.45f, 0.0f},
               {0.20f, 0.85f, 0.35f},
               {0.0f, 0.0f, 1.0f},
               {1.0f, 0.0f}},
              {{-0.55f, 0.45f, 0.0f},
               {0.20f, 0.45f, 0.95f},
               {0.0f, 0.0f, 1.0f},
               {0.0f, 0.0f}},
          },
      .indices = {0, 1, 2},
  });

  scene_.materials.push_back(Material{
      .albedoPath = "texture/image.jpg",
      .tint = {1.0f, 0.85f, 0.85f, 1.0f},
  });
  scene_.materials.push_back(Material{
      .albedoPath = "texture/smile.png",
      .tint = {0.85f, 1.0f, 0.85f, 1.0f},
  });

  auto makeObject = [](float x, MeshId meshId, MaterialId materialId) {
    SceneObject object{};
    object.transform.translation = {x, 0.0f, 0.0f};
    object.meshId = meshId;
    object.materialId = materialId;
    return object;
  };

  scene_.objects.push_back(makeObject(-0.25f, 0, 0));
  scene_.objects.back().transform.translation.z = -0.35f;
  scene_.objects.push_back(makeObject(0.0f, 1, 1));
  scene_.objects.back().transform.translation.z = 0.15f;
  scene_.objects.push_back(makeObject(0.25f, 0, 0));
  scene_.objects.back().transform.translation.z = -0.15f;

  scene_.cameras.push_back(Camera{});
  scene_.activeCameraIndex = 0;
  orbitCameraController_.attach(scene_.cameras[scene_.activeCameraIndex]);
  scene_.lighting = LightingSettings{};
}

std::vector<char const *> Application::getRequiredInstanceExtensions() {
  std::uint32_t glfwExtensionCount = 0;
  auto glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

  std::vector<char const *> extensions(glfwExtensions,
                                       glfwExtensions + glfwExtensionCount);
  if (validationLayersEnabled_) {
    extensions.push_back(vk::EXTDebugUtilsExtensionName);
  }

  return extensions;
}

VKAPI_ATTR vk::Bool32 VKAPI_CALL Application::debugCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
    vk::DebugUtilsMessageTypeFlagsEXT type,
    vk::DebugUtilsMessengerCallbackDataEXT const *callbackData, void *) {
  if (severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError ||
      severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
    std::cerr << "validation layer: type " << vk::to_string(type)
              << " msg: " << callbackData->pMessage << '\n';
  }

  return vk::False;
}
