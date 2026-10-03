#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "asset_ids.hpp"
#include "asset_library.hpp"
#include "benchmark_report.hpp"
#include "input_state.hpp"
#include "orbit_camera_controller.hpp"
#include "scene.hpp"
#include "scene_ecs.hpp"
#include "viewer_options.hpp"
#include "vulkan_include.hpp"
#include <unordered_map>

class Device;
class Renderer;
class SwapChain;
struct GLFWwindow;

class Application {
public:
  explicit Application(ViewerOptions options = {});
  ~Application();

  void run();

private:
  friend class ApplicationSceneLoadTest;
  void initWindow();
  void initVulkan();
  void mainLoop();
  void cleanup();
  void recreateSwapChain();
  void createInstance();
  void setupDebugMessenger();
  void createSurface();
  void updateScene();
  void createScene();
  void loadScene(std::filesystem::path const &path);
  void processPendingScene();
  void cancelSceneLoad();
  void advanceSceneLoad(bool waitForStartup = false);
  struct SceneCandidate;
  struct SceneLoad;
  std::unique_ptr<SceneLoad> sceneLoad_;
  void drawSceneBrowser();
  static void dropCallback(GLFWwindow *window, int count, char const **paths);
  void initImGui();
  void beginImGuiFrame();
  void drawImGui();
  void harvestBenchmarkTimings();
  void finishBenchmark();
  void cleanupImGui();
  void clearMaterialPreviewTextures();
  ImTextureID materialAlbedoPreviewTexture(MaterialId materialId);
  ImTextureID materialAlphaPreviewTexture(MaterialId materialId);

  std::vector<char const *> getRequiredInstanceExtensions();

  static void framebufferResizeCallback(GLFWwindow *window, int width,
                                        int height);
  static void windowFocusCallback(GLFWwindow *window, int focused);
  static void cursorEnterCallback(GLFWwindow *window, int entered);
  static void mouseButtonCallback(GLFWwindow *window, int button, int action,
                                  int mods);
  static void cursorPositionCallback(GLFWwindow *window, double xpos,
                                     double ypos);
  static void scrollCallback(GLFWwindow *window, double xoffset,
                             double yoffset);
  static void keyCallback(GLFWwindow *window, int key, int scancode, int action,
                          int mods);
  static void charCallback(GLFWwindow *window, unsigned int codepoint);

  static VKAPI_ATTR vk::Bool32 VKAPI_CALL debugCallback(
      vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
      vk::DebugUtilsMessageTypeFlagsEXT type,
      vk::DebugUtilsMessengerCallbackDataEXT const *callbackData, void *);

private:
  ViewerOptions options_;
  BenchmarkMetadata benchmarkMetadata_{};
  std::vector<BenchmarkFrame> benchmarkFrames_;
  std::unordered_map<std::uint64_t, std::size_t> benchmarkFrameIndices_;
  Camera benchmarkCamera_{};
  bool debugUtilsEnabled_ = false;
  unsigned validationErrors_ = 0, validationWarnings_ = 0;
  std::chrono::steady_clock::time_point benchmarkStart_{};
  GLFWwindow *window_ = nullptr;
  bool framebufferResized_ = false;

  vk::raii::Context context_;
  vk::raii::Instance instance_ = nullptr;
  vk::raii::DebugUtilsMessengerEXT debugMessenger_ = nullptr;
  vk::raii::SurfaceKHR surface_ = nullptr;
  std::unique_ptr<Device> device_;
  std::unique_ptr<SwapChain> swapChain_;
  std::unique_ptr<Renderer> renderer_;
  std::vector<char const *> requiredDeviceExtensions_;
  bool validationLayersEnabled_ = false;
  bool imguiInitialized_ = false;
  bool showAabbDebug_ = false;
  bool shadowDebugEnabled_ = true;
  bool normalMapDebugEnabled_ = true;
  bool parallaxDebugEnabled_ = true;
  bool frustumCullingEnabled_ = false;
  bool animateScene_ = false;
  std::optional<std::filesystem::path> startupScenePath_;
  std::optional<std::filesystem::path> pendingScenePath_;
  std::filesystem::path loadedScenePath_;
  std::array<char, 4096> scenePathInput_{};
  std::string sceneLoadError_;
  std::vector<std::string> sceneLoadWarnings_;
  std::filesystem::path browserDirectory_{"assets/models"};
  std::size_t renderQueueItems_ = 0;
  std::size_t visibleRenderQueueItems_ = 0;
  std::size_t culledRenderQueueItems_ = 0;
  std::size_t visibleOpaqueItems_ = 0;
  std::size_t visibleMaskItems_ = 0;
  std::size_t visibleTransparentItems_ = 0;
  std::size_t frameDrawCalls_ = 0;
  std::size_t shadowDrawCalls_ = 0;
  std::size_t mainDrawCalls_ = 0;
  std::size_t debugDrawCalls_ = 0;
  InputState input_{};
  AssetLibrary assets_{};
  Scene scene_{};
  Camera sceneCamera_{};
  SceneEcs sceneEcs_{};
  OrbitCameraController orbitCameraController_{};
  std::chrono::steady_clock::time_point animationStartTime_ =
      std::chrono::steady_clock::now();
  std::size_t selectedMaterialIndex_ = 0;
  std::vector<ImTextureID> materialAlbedoPreviewTextures_;
  std::vector<ImTextureID> materialAlphaPreviewTextures_;
  float frameTimeMs_ = 0.0f;
  float framesPerSecond_ = 0.0f;
  std::chrono::steady_clock::time_point lastFrameTime_ =
      std::chrono::steady_clock::now();
};
