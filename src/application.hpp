#pragma once

#include <chrono>
#include <memory>
#include <vector>

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

  std::vector<char const *> getRequiredInstanceExtensions();

  static void framebufferResizeCallback(GLFWwindow *window, int width,
                                        int height);

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
  Scene scene_{};
  std::chrono::steady_clock::time_point animationStartTime_ =
      std::chrono::steady_clock::now();
};
