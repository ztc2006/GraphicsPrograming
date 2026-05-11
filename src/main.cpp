#include <algorithm>
#include <array>
#include <assert.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <vector>

#if defined(__INTELLISENSE__) || !defined(USE_CPP20_MODULES)
#include <vulkan/vulkan_raii.hpp>
#else
import vulkan_hpp;
#endif

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

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

class Device {
public:
  Device(vk::raii::Instance const &instance,
         vk::raii::SurfaceKHR const &surface,
         std::vector<const char *> requiredDeviceExtensions)
      : instance_(instance), surface_(surface),
        requiredDeviceExtensions_(std::move(requiredDeviceExtensions)) {};

  vk::raii::Device const &logicalDeviced() const { return device_; };
  vk::raii::PhysicalDevice const &physicalDevice() const {
    return physicalDevice_;
  };
  vk::raii::Queue const &graphicsQueue() const { return queue_; };
  uint32_t graphicsQueueFamilyIndex() const { return queueFamilyIndex_; };

private:
  bool isDeviceSuitable(vk::raii::PhysicalDevice const &physicalDevice) {
    const bool supportsVulkan13 =
        physicalDevice.getProperties().apiVersion >= VK_API_VERSION_1_3;

    auto queueFamilies = physicalDevice.getQueueFamilyProperties();
    const bool supportsGraphics =
        std::ranges::any_of(queueFamilies, [](auto const &queueFamily) {
          return !!(queueFamily.queueFlags & vk::QueueFlagBits::eGraphics);
        });

    auto availableDeviceExtensions =
        physicalDevice.enumerateDeviceExtensionProperties();
    const bool supportsAllRequiredExtensions = std::ranges::all_of(
        requiredDeviceExtensions_,
        [&availableDeviceExtensions](auto const &requiredExtension) {
          return std::ranges::any_of(
              availableDeviceExtensions,
              [requiredExtension](auto const &availableExtension) {
                return std::strcmp(availableExtension.extensionName,
                                   requiredExtension) == 0;
              });
        });

    auto features = physicalDevice.template getFeatures2<
        vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features>();

    const bool supportsRequiredFeatures =
        features.template get<vk::PhysicalDeviceVulkan13Features>()
            .dynamicRendering &&
        features.template get<vk::PhysicalDeviceVulkan13Features>()
            .synchronization2;

    return supportsVulkan13 && supportsGraphics && supportsRequiredFeatures &&
           supportsAllRequiredExtensions;
  }

  void pickPhysicalDevice() {
    std::vector<vk::raii::PhysicalDevice> physicalDevices =
        instance_.enumeratePhysicalDevices();
    auto deviceIt =
        std::ranges::find_if(physicalDevices, [&](auto const &physicalDevice) {
          return isDeviceSuitable(physicalDevice);
        });

    if (deviceIt == physicalDevices.end()) {
      throw std::runtime_error("Failed to find a suitable GPU.");
    }

    physicalDevice_ = *deviceIt;
  }

  void createLogicalDevice() {
    std::vector<vk::QueueFamilyProperties> queueFamilyProperties =
        physicalDevice_.getQueueFamilyProperties();

    queueFamilyIndex_ = ~0u;
    for (std::uint32_t queueFamilyIndex = 0;
         queueFamilyIndex < queueFamilyProperties.size(); ++queueFamilyIndex) {
      const bool supportsGraphics =
          !!(queueFamilyProperties[queueFamilyIndex].queueFlags &
             vk::QueueFlagBits::eGraphics);
      const bool supportsPresent =
          physicalDevice_.getSurfaceSupportKHR(queueFamilyIndex, *surface_);
      if (supportsGraphics && supportsPresent) {
        queueFamilyIndex_ = queueFamilyIndex;
        break;
      }
    }

    if (queueFamilyIndex_ == ~0u) {
      throw std::runtime_error(
          "Could not find a queue family for graphics and present.");
    }

    vk::StructureChain<vk::PhysicalDeviceFeatures2,
                       vk::PhysicalDeviceVulkan13Features>
        featureChain = {
            {},
            {
                .synchronization2 = true,
                .dynamicRendering = true,
            },
        };

    float queuePriority = 1.0f;
    vk::DeviceQueueCreateInfo deviceQueueCreateInfo{
        .queueFamilyIndex = queueFamilyIndex_,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };
    vk::DeviceCreateInfo deviceCreateInfo{
        .pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &deviceQueueCreateInfo,
        .enabledExtensionCount =
            static_cast<std::uint32_t>(requiredDeviceExtensions_.size()),
        .ppEnabledExtensionNames = requiredDeviceExtensions_.data(),
    };

    device_ = vk::raii::Device(physicalDevice_, deviceCreateInfo);
    queue_ = vk::raii::Queue(device_, queueFamilyIndex_, 0);
  }

private:
  vk::raii::Instance const &instance_;
  vk::raii::SurfaceKHR const &surface_;
  std::vector<const char *> requiredDeviceExtensions_;

  vk::raii::PhysicalDevice physicalDevice_ = nullptr;
  vk::raii::Device device_ = nullptr;
  vk::raii::Queue queue_ = nullptr;
  std::uint32_t queueFamilyIndex_ = ~0u;
};

class SwapChain {
public:
  SwapChain(Device const &device, vk::raii::SurfaceKHR const &surface,
            GLFWwindow *window);

  vk::raii::SurfaceKHR const &handel() const;
  vk::Format imageFormat() const;
  vk::Extent2D extent() const;
  std::vector<vk::Image> const &images() const;
  std::vector<vk::raii::ImageView> const &imageViews() const;

private:
  void createSwapChain();
  void createImageViews();

private:
  vk::raii::SwapchainKHR swapChain_ = nullptr;
  std::vector<vk::Image> images_;
  std::vector<vk::raii::ImageView> imageViews_;
  vk::SurfaceFormatKHR surfaceFormat_{};
  vk::Extent2D extent_{};
};

class Renderer {
public:
  Renderer(Device const &device, SwapChain const &swapChain);

  void drawFrame();

private:
  void createCommandPool();
  void createPipeline();
  void createCommandBuffers();
  void createSyncObjects();
  void recordCommandBuffer(uint32_t imageIndex);

private:
  vk::raii::PipelineLayout pipelineLayout_ = nullptr;
  vk::raii::Pipeline pipeline_ = nullptr;
  vk::raii::CommandPool commandPool_ = nullptr;
  vk::raii::CommandBuffers commandBuffers = nullptr;
};

class Application {
public:
  void run();

private:
  void initWindow();
  void initVulkan();
  void mainLoop();
};

class HelloTriangleApplication {
public:
  void run() {
    initWindow();
    initVulkan();
    mainLoop();
    cleanup();
  }

private:
  GLFWwindow *window_ = nullptr;
  vk::raii::Context context_;
  vk::raii::Instance instance_ = nullptr;
  vk::raii::DebugUtilsMessengerEXT debugMessenger_ = nullptr;
  vk::raii::SurfaceKHR surface_ = nullptr;
  std::unique_ptr<Device> device_;

  vk::raii::SwapchainKHR swapChain_ = nullptr;
  vk::raii::PipelineLayout pipelineLayout_ = nullptr;
  vk::raii::Pipeline graphicsPipeline_ = nullptr;
  vk::raii::CommandPool commandPool_ = nullptr;
  vk::raii::CommandBuffers commandBuffers_ = nullptr;
  vk::raii::Semaphore imageAvailableSemaphore_ = nullptr;
  vk::raii::Fence inFlightFence_ = nullptr;
  std::vector<vk::Image> swapChainImages_;
  std::vector<vk::ImageLayout> swapChainImageLayouts_;
  std::vector<vk::raii::Semaphore> renderFinishedSemaphores_;
  vk::SurfaceFormatKHR swapChainSurfaceFormat_{};
  vk::Extent2D swapChainExtent_{};
  std::vector<vk::raii::ImageView> swapChainImageViews_;

  std::vector<const char *> requiredDeviceExtensions_ = {
      vk::KHRSwapchainExtensionName};

  void initWindow() {
    if (glfwInit() != GLFW_TRUE) {
      throw std::runtime_error("Failed to initialize GLFW.");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    window_ = glfwCreateWindow(kWindowWidth, kWindowHeight, "Vulkan", nullptr,
                               nullptr);
    if (window_ == nullptr) {
      throw std::runtime_error("Failed to create GLFW window.");
    }
  }

  void initVulkan() {
    createInstance();
    setupDebugMessenger();
    createSurface();
    device_ = std::make_unique<Device>(instance_, surface_,
                                       requiredDeviceExtensions_);
    createSwapChain();
    createSwapChainImageViews();
    createCommandPool();
    createGraphicsPipeline();
    createCommandBuffers();
    createSyncObjects();
  }

  void mainLoop() {
    while (!glfwWindowShouldClose(window_)) {
      glfwPollEvents();
      drawFrame();
    }
  }

  void cleanup() {
    if (device_) {
      device_->logicalDeviced().waitIdle();
    }

    if (window_ != nullptr) {
      glfwDestroyWindow(window_);
      window_ = nullptr;
    }
    glfwTerminate();
  }

  void createInstance() {
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

  void setupDebugMessenger() {
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

  void createSurface() {
    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(*instance_, window_, nullptr, &rawSurface) !=
        VK_SUCCESS) {
      throw std::runtime_error("Failed to create window surface.");
    }
    surface_ = vk::raii::SurfaceKHR(instance_, rawSurface);
  }

  void createSwapChain() {
    vk::SurfaceCapabilitiesKHR surfaceCapabilities =
        device_->physicalDevice().getSurfaceCapabilitiesKHR(*surface_);
    swapChainExtent_ = chooseSwapExtent(surfaceCapabilities);
    std::uint32_t minImageCount = chooseSwapMinImageCount(surfaceCapabilities);

    std::vector<vk::SurfaceFormatKHR> availableFormats =
        device_->physicalDevice().getSurfaceFormatsKHR(*surface_);
    swapChainSurfaceFormat_ = chooseSwapSurfaceFormat(availableFormats);

    std::vector<vk::PresentModeKHR> availablePresentModes =
        device_->physicalDevice().getSurfacePresentModesKHR(*surface_);
    vk::PresentModeKHR presentMode =
        chooseSwapPresentMode(availablePresentModes);

    vk::SwapchainCreateInfoKHR swapChainCreateInfo{
        .surface = *surface_,
        .minImageCount = minImageCount,
        .imageFormat = swapChainSurfaceFormat_.format,
        .imageColorSpace = swapChainSurfaceFormat_.colorSpace,
        .imageExtent = swapChainExtent_,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .preTransform = surfaceCapabilities.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .presentMode = presentMode,
        .clipped = true,
    };

    swapChain_ =
        vk::raii::SwapchainKHR(device_->logicalDeviced(), swapChainCreateInfo);
    swapChainImages_ = swapChain_.getImages();
    swapChainImageLayouts_.assign(swapChainImages_.size(),
                                  vk::ImageLayout::eUndefined);
  }

  void createSwapChainImageViews() {
    swapChainImageViews_.clear();
    swapChainImageViews_.reserve(swapChainImages_.size());

    for (vk::Image image : swapChainImages_) {
      vk::ImageViewCreateInfo createInfo{
          .image = image,
          .viewType = vk::ImageViewType::e2D,
          .format = swapChainSurfaceFormat_.format,
          .subresourceRange =
              {
                  .aspectMask = vk::ImageAspectFlagBits::eColor,
                  .baseMipLevel = 0,
                  .levelCount = 1,
                  .baseArrayLayer = 0,
                  .layerCount = 1,
              },
      };

      swapChainImageViews_.emplace_back(device_->logicalDeviced(), createInfo);
    }
  }

  void createCommandPool() {
    vk::CommandPoolCreateInfo createInfo{
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = device_->graphicsQueueFamilyIndex(),
    };
    commandPool_ = vk::raii::CommandPool(device_->logicalDeviced(), createInfo);
  }

  void createGraphicsPipeline() {
    std::vector<char> vertCode = readBinaryFile("shaders/triangle.vert.spv");
    std::vector<char> fragCode = readBinaryFile("shaders/triangle.frag.spv");

    vk::ShaderModuleCreateInfo vertexShaderCreateInfo{
        .codeSize = vertCode.size(),
        .pCode = reinterpret_cast<std::uint32_t const *>(vertCode.data()),
    };
    vk::ShaderModuleCreateInfo fragmentShaderCreateInfo{
        .codeSize = fragCode.size(),
        .pCode = reinterpret_cast<std::uint32_t const *>(fragCode.data()),
    };

    vk::raii::ShaderModule vertexShaderModule(device_->logicalDeviced(),
                                              vertexShaderCreateInfo);
    vk::raii::ShaderModule fragmentShaderModule(device_->logicalDeviced(),
                                                fragmentShaderCreateInfo);

    std::array shaderStages = {
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = *vertexShaderModule,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = *fragmentShaderModule,
            .pName = "main",
        },
    };

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo{};
    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
        .primitiveRestartEnable = false,
    };
    vk::PipelineViewportStateCreateInfo viewportState{
        .viewportCount = 1,
        .scissorCount = 1,
    };
    vk::PipelineRasterizationStateCreateInfo rasterizer{
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };
    vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = false,
    };

    vk::PipelineColorBlendAttachmentState colorBlendAttachment{
        .blendEnable = false,
        .colorWriteMask =
            vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    vk::PipelineColorBlendStateCreateInfo colorBlending{
        .logicOpEnable = false,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment,
    };

    std::array dynamicStates = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };
    vk::PipelineDynamicStateCreateInfo dynamicState{
        .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };

    pipelineLayout_ = vk::raii::PipelineLayout(device_->logicalDeviced(),
                                               vk::PipelineLayoutCreateInfo{});

    vk::Format colorAttachmentFormat = swapChainSurfaceFormat_.format;
    vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &colorAttachmentFormat,
    };

    vk::GraphicsPipelineCreateInfo pipelineCreateInfo{
        .pNext = &pipelineRenderingCreateInfo,
        .stageCount = static_cast<std::uint32_t>(shaderStages.size()),
        .pStages = shaderStages.data(),
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &colorBlending,
        .pDynamicState = &dynamicState,
        .layout = *pipelineLayout_,
    };

    graphicsPipeline_ = vk::raii::Pipeline(device_->logicalDeviced(), nullptr,
                                           pipelineCreateInfo);
  }

  void createCommandBuffers() {
    vk::CommandBufferAllocateInfo allocateInfo{
        .commandPool = *commandPool_,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    commandBuffers_ =
        vk::raii::CommandBuffers(device_->logicalDeviced(), allocateInfo);
  }

  void createSyncObjects() {
    imageAvailableSemaphore_ = vk::raii::Semaphore(device_->logicalDeviced(),
                                                   vk::SemaphoreCreateInfo{});
    renderFinishedSemaphores_.clear();
    renderFinishedSemaphores_.reserve(swapChainImages_.size());
    for (std::size_t index = 0; index < swapChainImages_.size(); ++index) {
      renderFinishedSemaphores_.emplace_back(device_->logicalDeviced(),
                                             vk::SemaphoreCreateInfo{});
    }
    inFlightFence_ = vk::raii::Fence(
        device_->logicalDeviced(),
        vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
  }

  void drawFrame() {
    (void)device_->logicalDeviced().waitForFences(
        {*inFlightFence_}, true, std::numeric_limits<std::uint64_t>::max());
    device_->logicalDeviced().resetFences({*inFlightFence_});

    auto [acquireResult, imageIndex] =
        swapChain_.acquireNextImage(std::numeric_limits<std::uint64_t>::max(),
                                    *imageAvailableSemaphore_, nullptr);

    if (acquireResult != vk::Result::eSuccess &&
        acquireResult != vk::Result::eSuboptimalKHR) {
      throw std::runtime_error("Failed to acquire swapchain image.");
    }

    commandBuffers_[0].reset();
    recordCommandBuffer(commandBuffers_[0], imageIndex);

    vk::Semaphore waitSemaphore = *imageAvailableSemaphore_;
    vk::PipelineStageFlags waitStage =
        vk::PipelineStageFlagBits::eColorAttachmentOutput;
    vk::CommandBuffer commandBuffer = *commandBuffers_[0];
    vk::Semaphore signalSemaphore = *renderFinishedSemaphores_[imageIndex];

    vk::SubmitInfo submitInfo{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &waitSemaphore,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &signalSemaphore,
    };

    device_->graphicsQueue().submit({submitInfo}, *inFlightFence_);

    vk::SwapchainKHR swapChainHandle = *swapChain_;
    vk::PresentInfoKHR presentInfo{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &signalSemaphore,
        .swapchainCount = 1,
        .pSwapchains = &swapChainHandle,
        .pImageIndices = &imageIndex,
    };

    vk::Result presentResult = device_->graphicsQueue().presentKHR(presentInfo);
    if (presentResult != vk::Result::eSuccess &&
        presentResult != vk::Result::eSuboptimalKHR) {
      throw std::runtime_error("Failed to present swapchain image.");
    }
  }

  void recordCommandBuffer(vk::raii::CommandBuffer const &commandBuffer,
                           std::uint32_t imageIndex) {
    commandBuffer.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });

    transitionSwapChainImage(commandBuffer, imageIndex,
                             vk::ImageLayout::eColorAttachmentOptimal,
                             vk::PipelineStageFlagBits2::eAllCommands,
                             vk::AccessFlagBits2::eMemoryRead,
                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                             vk::AccessFlagBits2::eColorAttachmentWrite);

    vk::ClearValue clearValue{
        .color = vk::ClearColorValue(
            std::array<float, 4>{0.05f, 0.07f, 0.10f, 1.0f}),
    };

    vk::RenderingAttachmentInfo colorAttachment{
        .imageView = *swapChainImageViews_[imageIndex],
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = clearValue,
    };
    vk::RenderingInfo renderingInfo{
        .renderArea =
            {
                .offset = {0, 0},
                .extent = swapChainExtent_,
            },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
    };

    commandBuffer.beginRendering(renderingInfo);
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                               *graphicsPipeline_);

    vk::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(swapChainExtent_.width),
        .height = static_cast<float>(swapChainExtent_.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    vk::Rect2D scissor{
        .offset = {0, 0},
        .extent = swapChainExtent_,
    };
    commandBuffer.setViewport(0, {viewport});
    commandBuffer.setScissor(0, {scissor});
    commandBuffer.draw(3, 1, 0, 0);
    commandBuffer.endRendering();

    transitionSwapChainImage(commandBuffer, imageIndex,
                             vk::ImageLayout::ePresentSrcKHR,
                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                             vk::AccessFlagBits2::eColorAttachmentWrite,
                             vk::PipelineStageFlagBits2::eAllCommands,
                             vk::AccessFlagBits2::eMemoryRead);

    commandBuffer.end();
  }

  void transitionSwapChainImage(vk::raii::CommandBuffer const &commandBuffer,
                                std::uint32_t imageIndex,
                                vk::ImageLayout newLayout,
                                vk::PipelineStageFlags2 srcStageMask,
                                vk::AccessFlags2 srcAccessMask,
                                vk::PipelineStageFlags2 dstStageMask,
                                vk::AccessFlags2 dstAccessMask) {
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = srcStageMask,
        .srcAccessMask = srcAccessMask,
        .dstStageMask = dstStageMask,
        .dstAccessMask = dstAccessMask,
        .oldLayout = swapChainImageLayouts_[imageIndex],
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapChainImages_[imageIndex],
        .subresourceRange =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    vk::DependencyInfo dependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };

    commandBuffer.pipelineBarrier2(dependencyInfo);
    swapChainImageLayouts_[imageIndex] = newLayout;
  }

  static std::vector<char> readBinaryFile(char const *path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
      throw std::runtime_error("Failed to open shader file: " +
                               std::string(path));
    }

    std::streamsize size = file.tellg();
    if (size <= 0) {
      throw std::runtime_error("Shader file is empty: " + std::string(path));
    }

    std::vector<char> buffer(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
  }

  static std::uint32_t chooseSwapMinImageCount(
      vk::SurfaceCapabilitiesKHR const &surfaceCapabilities) {
    auto minImageCount = std::max(3u, surfaceCapabilities.minImageCount);
    if ((0 < surfaceCapabilities.maxImageCount) &&
        (surfaceCapabilities.maxImageCount < minImageCount)) {
      minImageCount = surfaceCapabilities.maxImageCount;
    }
    return minImageCount;
  }

  static vk::SurfaceFormatKHR chooseSwapSurfaceFormat(
      std::vector<vk::SurfaceFormatKHR> const &availableFormats) {
    assert(!availableFormats.empty());
    auto formatIt =
        std::ranges::find_if(availableFormats, [](auto const &format) {
          return format.format == vk::Format::eB8G8R8A8Srgb &&
                 format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
        });
    return formatIt != availableFormats.end() ? *formatIt : availableFormats[0];
  }

  static vk::PresentModeKHR chooseSwapPresentMode(
      std::vector<vk::PresentModeKHR> const &availablePresentModes) {
    assert(std::ranges::any_of(availablePresentModes, [](auto presentMode) {
      return presentMode == vk::PresentModeKHR::eFifo;
    }));

    return std::ranges::any_of(availablePresentModes,
                               [](vk::PresentModeKHR presentMode) {
                                 return presentMode ==
                                        vk::PresentModeKHR::eMailbox;
                               })
               ? vk::PresentModeKHR::eMailbox
               : vk::PresentModeKHR::eFifo;
  }

  vk::Extent2D
  chooseSwapExtent(vk::SurfaceCapabilitiesKHR const &capabilities) {
    if (capabilities.currentExtent.width !=
        std::numeric_limits<std::uint32_t>::max()) {
      return capabilities.currentExtent;
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);

    return {
        std::clamp<std::uint32_t>(width, capabilities.minImageExtent.width,
                                  capabilities.maxImageExtent.width),
        std::clamp<std::uint32_t>(height, capabilities.minImageExtent.height,
                                  capabilities.maxImageExtent.height),
    };
  }

  std::vector<const char *> getRequiredInstanceExtensions() {
    std::uint32_t glfwExtensionCount = 0;
    auto glfwExtensions =
        glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::vector<const char *> extensions(glfwExtensions,
                                         glfwExtensions + glfwExtensionCount);
    if (kEnableValidationLayers) {
      extensions.push_back(vk::EXTDebugUtilsExtensionName);
    }
    return extensions;
  }

  static VKAPI_ATTR vk::Bool32 VKAPI_CALL debugCallback(
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
};
} // namespace

int main() {
  try {
    HelloTriangleApplication app;
    app.run();
  } catch (std::exception const &exception) {
    std::cerr << exception.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
