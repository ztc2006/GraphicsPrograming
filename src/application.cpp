#include "application.hpp"

#include <cstring>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>

#include <GLFW/glfw3.h>

#include "device.hpp"
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
    throw std::runtime_error("Failed to initialize GLFW.");
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
}

void Application::initVulkan() {
  createInstance();
  setupDebugMessenger();
  createSurface();

  requiredDeviceExtensions_ = {vk::KHRSwapchainExtensionName};
  device_ =
      std::make_unique<Device>(instance_, surface_, requiredDeviceExtensions_);
  swapChain_ = std::make_unique<SwapChain>(*device_, surface_, window_);
  renderer_ = std::make_unique<Renderer>(*device_);
  renderer_->recreateForSwapChain(*swapChain_);
}

void Application::mainLoop() {
  while (!glfwWindowShouldClose(window_)) {
    glfwPollEvents();

    auto frameResult = renderer_->drawFrame();
    if (frameResult != Renderer::FrameResult::eSuccess || framebufferResized_) {
      recreateSwapChain();
    }
  }
}

void Application::cleanup() {
  if (device_) {
    device_->logicalDevice().waitIdle();
  }

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

void Application::framebufferResizeCallback(GLFWwindow *window, int width,
                                            int height) {
  auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
  if (app != nullptr) {
    app->framebufferResized_ = true;
  }
}

void Application::createInstance() {
  constexpr vk::ApplicationInfo appInfo{
      .pApplicationName = "Hello Triangle",
      .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
      .pEngineName = "Fool Engine",
      .apiVersion = vk::ApiVersion14,
  };

  std::vector<char const *> requiredLayers;
  if (kEnableValidationLayers) {
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
    throw std::runtime_error("Required layer not supported: " +
                             std::string(*unsupportedLayerIt));
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
  if (!kEnableValidationLayers) {
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

std::vector<char const *> Application::getRequiredInstanceExtensions() {
  std::uint32_t glfwExtensionCount = 0;
  auto glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

  std::vector<char const *> extensions(glfwExtensions,
                                       glfwExtensions + glfwExtensionCount);
  if (kEnableValidationLayers) {
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
