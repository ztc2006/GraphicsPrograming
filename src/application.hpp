#pragma once

#include <chrono>
#include <memory>
#include <vector>

#include "input_state.hpp"
#include "orbit_camera_controller.hpp"
#include "scene.hpp"
#include "vulkan_include.hpp"

class Device;
class Renderer;
class SwapChain;
struct GLFWwindow;

class Application {
public:
  Application();
  ~Application();

  void run();

private:
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
  void initImGui();
  void beginImGuiFrame();
  void drawImGui();
  void cleanupImGui();

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
  bool showAabbDebug_ = true;
  bool shadowDebugEnabled_ = true;
  bool normalMapDebugEnabled_ = true;
  bool parallaxDebugEnabled_ = true;
  bool frustumCullingEnabled_ = false;
  bool animateScene_ = false;
  std::size_t renderQueueItems_ = 0;
  std::size_t visibleRenderQueueItems_ = 0;
  std::size_t culledRenderQueueItems_ = 0;
  std::size_t frameDrawCalls_ = 0;
  std::size_t shadowDrawCalls_ = 0;
  std::size_t mainDrawCalls_ = 0;
  std::size_t debugDrawCalls_ = 0;
  InputState input_{};
  Scene scene_{};
  OrbitCameraController orbitCameraController_{};
  std::chrono::steady_clock::time_point animationStartTime_ =
      std::chrono::steady_clock::now();
  std::size_t selectedMaterialIndex_ = 0;
  float frameTimeMs_ = 0.0f;
  float framesPerSecond_ = 0.0f;
  std::chrono::steady_clock::time_point lastFrameTime_ =
      std::chrono::steady_clock::now();
};
