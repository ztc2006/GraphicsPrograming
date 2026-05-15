#include "renderer.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <glm/glm.hpp>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
struct Vertex {
  glm::vec2 position;
  glm::vec3 color;

  static vk::VertexInputBindingDescription bindingDescription() {
    return vk::VertexInputBindingDescription{
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };
  }

  static std::array<vk::VertexInputAttributeDescription, 2>
  attributeDescriptions() {
    return {
        vk::VertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(Vertex, position),
        },
        vk::VertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = vk::Format::eR32G32B32Sfloat,
            .offset = offsetof(Vertex, color),
        },
    };
  }
};

struct PushConstants {
  glm::mat4 transform{1.0f};
};

const std::array<Vertex, 4> kVertices = {
    Vertex{{-0.5f, -0.5f}, {0.95f, 0.30f, 0.25f}},
    Vertex{{0.5f, -0.5f}, {0.20f, 0.75f, 0.35f}},
    Vertex{{0.5f, 0.5f}, {0.15f, 0.45f, 0.95f}},
    Vertex{{-0.5f, 0.5f}, {0.98f, 0.82f, 0.20f}},
};

const std::array<std::uint32_t, 6> kIndices = {0, 1, 2, 2, 3, 0};
} // namespace

Renderer::Renderer(Device const &device) : device_(device) {
  createPersistentResources();
}

void Renderer::recreateForSwapChain(SwapChain const &swapChain) {
  validateSwapChainCandidate(swapChain);

  vk::PushConstantRange pushConstantRange{
      .stageFlags = vk::ShaderStageFlagBits::eVertex,
      .offset = 0,
      .size = sizeof(PushConstants),
  };
  vk::PipelineLayoutCreateInfo PipelineLayoutCreateInfo{
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pushConstantRange,
  };
  vk::raii::PipelineLayout newPipelineLayout(device_.logicalDevice(),
                                             PipelineLayoutCreateInfo);

  vk::raii::Pipeline newGraphicsPipeline =
      createGraphicsPipeline(swapChain, newPipelineLayout);

  std::vector<vk::raii::Semaphore> newRenderFinishedSemaphores;
  newRenderFinishedSemaphores.reserve(swapChain.images().size());
  for (std::size_t index = 0; index < swapChain.images().size(); ++index) {
    newRenderFinishedSemaphores.emplace_back(device_.logicalDevice(),
                                             vk::SemaphoreCreateInfo{});
  }

  std::vector<vk::ImageLayout> newSwapChainImageLayouts(
      swapChain.images().size(), vk::ImageLayout::eUndefined);
  std::vector<vk::Fence> newImagesInFlight(swapChain.images().size(),
                                           vk::Fence{});
  SwapChain const *newSwapChain = &swapChain;

  using std::swap;
  swap(pipelineLayout_, newPipelineLayout);
  swap(graphicsPipeline_, newGraphicsPipeline);
  swap(renderFinishedSemaphores_, newRenderFinishedSemaphores);
  swap(swapChainImageLayouts_, newSwapChainImageLayouts);
  swap(imagesInFlight_, newImagesInFlight);
  swap(swapChain_, newSwapChain);
  currentFrame_ = 0;
}

void Renderer::createPersistentResources() {
  createCommandPool();
  createFrameResources();
  createCommandBuffers();
  createGeometryResources();
}

void Renderer::createFrameResources() {
  frames_.clear();
  frames_.reserve(kFramesInFlight);

  for (std::uint32_t index = 0; index < kFramesInFlight; ++index) {
    FrameContext frame{};
    frame.imageAvailableSemaphore =
        vk::raii::Semaphore(device_.logicalDevice(), vk::SemaphoreCreateInfo{});
    frame.inFlightFence = vk::raii::Fence(
        device_.logicalDevice(),
        vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
    frames_.push_back(std::move(frame));
  }
}

void Renderer::createGeometryResources() {
  auto uploadArryToDeviceLocalBuffer =
      [this]<typename T, std::size_t N>(std::array<T, N> const &sourceData,
                                        vk::BufferUsageFlags finalUsage)
      -> std::pair<vk::raii::Buffer, vk::raii::DeviceMemory> {
    vk::DeviceSize const bufferSize = sizeof(T) * sourceData.size();

    auto [stagingBuffer, stagingMemory] =
        device_.createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferSrc,
                             vk::MemoryPropertyFlagBits::eHostVisible |
                                 vk::MemoryPropertyFlagBits::eHostCoherent);

    void *mappedMemory = stagingMemory.mapMemory(0, bufferSize);
    std::memcpy(mappedMemory, sourceData.data(),
                static_cast<std::size_t>(bufferSize));
    stagingMemory.unmapMemory();

    auto [deviceBuffer, deviceMemory] = device_.createBuffer(
        bufferSize, vk::BufferUsageFlagBits::eTransferDst | finalUsage,
        vk::MemoryPropertyFlagBits::eDeviceLocal);

    device_.copyBuffer(*stagingBuffer, *deviceBuffer, bufferSize);
    return {std::move(deviceBuffer), std::move(deviceMemory)};
  };

  auto [newVertexBuffer, newVertexBufferMemory] = uploadArryToDeviceLocalBuffer(
      kVertices, vk::BufferUsageFlagBits::eVertexBuffer);

  auto [newIndexBuffer, newIndexBufferMemory] = uploadArryToDeviceLocalBuffer(
      kIndices, vk::BufferUsageFlagBits::eIndexBuffer);

  std::uint32_t newIndexCount = static_cast<std::uint32_t>(kIndices.size());

  using std::swap;
  swap(vertexBuffer_, newVertexBuffer);
  swap(vertexBufferMemory_, newVertexBufferMemory);
  swap(indexBuffer_, newIndexBuffer);
  swap(indexBufferMemory_, newIndexBufferMemory);
  swap(indexCount_, newIndexCount);
}

Renderer::FrameResult Renderer::drawFrame() {
  validateSwapChainState();

  auto &frame = frames_[currentFrame_];
  auto &commandBuffer = commandBuffers_[currentFrame_];

  (void)device_.logicalDevice().waitForFences(
      {*frame.inFlightFence}, true, std::numeric_limits<std::uint64_t>::max());

  vk::Result acquireResult = vk::Result::eSuccess;
  std::uint32_t imageIndex = 0;

  try {
    auto acquire = swapChain_->handle().acquireNextImage(
        std::numeric_limits<std::uint64_t>::max(),
        *frame.imageAvailableSemaphore, nullptr);
    acquireResult = acquire.first;
    imageIndex = acquire.second;
  } catch (vk::OutOfDateKHRError const &) {
    return FrameResult::eSwapChainOutOfDate;
  }

  if (acquireResult != vk::Result::eSuccess &&
      acquireResult != vk::Result::eSuboptimalKHR) {
    throw std::runtime_error("Failed to acquire swapchain image.");
  }

  if (imageIndex >= renderFinishedSemaphores_.size()) {
    throw std::runtime_error("Acquired swapchain image index is out of range.");
  }

  if (imagesInFlight_[imageIndex]) {
    (void)device_.logicalDevice().waitForFences(
        {imagesInFlight_[imageIndex]}, true,
        std::numeric_limits<std::uint64_t>::max());
  }

  imagesInFlight_[imageIndex] = *frame.inFlightFence;

  device_.logicalDevice().resetFences({*frame.inFlightFence});

  commandBuffer.reset();
  recordCommandBuffer(commandBuffer, imageIndex);

  vk::Semaphore waitSemaphore = *frame.imageAvailableSemaphore;
  vk::PipelineStageFlags waitStage =
      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  vk::CommandBuffer rawCommandBuffer = *commandBuffer;
  vk::Semaphore signalSemaphore = *renderFinishedSemaphores_[imageIndex];

  vk::SubmitInfo submitInfo{
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &waitSemaphore,
      .pWaitDstStageMask = &waitStage,
      .commandBufferCount = 1,
      .pCommandBuffers = &rawCommandBuffer,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &signalSemaphore,
  };

  device_.graphicsQueue().submit({submitInfo}, *frame.inFlightFence);

  vk::SwapchainKHR swapChainHandle = *swapChain_->handle();
  vk::PresentInfoKHR presentInfo{
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &signalSemaphore,
      .swapchainCount = 1,
      .pSwapchains = &swapChainHandle,
      .pImageIndices = &imageIndex,
  };

  auto advanceFrame = [this]() {
    currentFrame_ = (currentFrame_ + 1) % kFramesInFlight;
  };

  vk::Result presentResult = vk::Result::eSuccess;
  try {
    presentResult = device_.presentQueue().presentKHR(presentInfo);
  } catch (vk::OutOfDateKHRError const &) {
    advanceFrame();
    return FrameResult::eSwapChainOutOfDate;
  }

  if (presentResult != vk::Result::eSuccess &&
      presentResult != vk::Result::eSuboptimalKHR) {
    throw std::runtime_error("Failed to present swapchain image.");
  }

  if (acquireResult == vk::Result::eSuboptimalKHR ||
      presentResult == vk::Result::eSuboptimalKHR) {
    advanceFrame();
    return FrameResult::eSwapChainSuboptimal;
  }

  advanceFrame();
  return FrameResult::eSuccess;
}

std::vector<char> Renderer::readBinaryFile(char const *path) {
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

void Renderer::validateSwapChainCandidate(SwapChain const &swapChain) const {
  if (swapChain.images().empty()) {
    throw std::runtime_error("Swapchain candidate has no images.");
  }

  if (swapChain.imageViews().size() != swapChain.images().size()) {
    throw std::runtime_error(
        "Swapchain candidate image view count does not match image count.");
  }

  if (swapChain.imageFormat() == vk::Format::eUndefined) {
    throw std::runtime_error("Swapchain candidate has an undefined format.");
  }

  if (swapChain.extent().width == 0 || swapChain.extent().height == 0) {
    throw std::runtime_error("Swapchain candidate has an invalid extent.");
  }
}

void Renderer::validateSwapChainState() const {
  if (swapChain_ == nullptr) {
    throw std::runtime_error("Renderer is not initialized with a swapchain.");
  }

  if (frames_.size() != kFramesInFlight) {
    throw std::runtime_error("Renderer frame resource count is invalid.");
  }

  if (commandBuffers_.size() != kFramesInFlight) {
    throw std::runtime_error("Renderer command buffer count is invalid.");
  }

  if (currentFrame_ >= frames_.size()) {
    throw std::runtime_error("Renderer current frame index is out of range.");
  }

  if (pipelineLayout_ == nullptr) {
    throw std::runtime_error("Renderer pipeline layout is not initialized.");
  }

  if (graphicsPipeline_ == nullptr) {
    throw std::runtime_error("Renderer graphics pipeline is not initialized.");
  }

  auto const imageCount = swapChain_->images().size();
  if (imageCount == 0) {
    throw std::runtime_error("Renderer swapchain has no images.");
  }

  if (swapChain_->imageViews().size() != imageCount) {
    throw std::runtime_error(
        "Renderer swapchain image view count does not match image count.");
  }

  if (renderFinishedSemaphores_.size() != imageCount ||
      imagesInFlight_.size() != imageCount ||
      swapChainImageLayouts_.size() != imageCount) {
    throw std::runtime_error(
        "Renderer swapchain-dependent resource counts are inconsistent.");
  }
  if (vertexBuffer_ == nullptr || indexBuffer_ == nullptr || indexCount_ == 0) {
    throw std::runtime_error(
        "Renderer geometry resources are not initialized.");
  }
}

void Renderer::createCommandPool() {
  vk::CommandPoolCreateInfo createInfo{
      .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
      .queueFamilyIndex = device_.graphicsQueueFamilyIndex(),
  };
  commandPool_ = vk::raii::CommandPool(device_.logicalDevice(), createInfo);
}

vk::raii::Pipeline Renderer::createGraphicsPipeline(
    SwapChain const &swapChain,
    vk::raii::PipelineLayout const &pipelineLayout) const {
  auto vertCode = readBinaryFile("shaders/triangle.vert.spv");
  auto fragCode = readBinaryFile("shaders/triangle.frag.spv");

  vk::ShaderModuleCreateInfo vertexShaderCreateInfo{
      .codeSize = vertCode.size(),
      .pCode = reinterpret_cast<std::uint32_t const *>(vertCode.data()),
  };
  vk::ShaderModuleCreateInfo fragmentShaderCreateInfo{
      .codeSize = fragCode.size(),
      .pCode = reinterpret_cast<std::uint32_t const *>(fragCode.data()),
  };

  vk::raii::ShaderModule vertexShaderModule(device_.logicalDevice(),
                                            vertexShaderCreateInfo);
  vk::raii::ShaderModule fragmentShaderModule(device_.logicalDevice(),
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

  auto bindingDescription = Vertex::bindingDescription();
  auto attributeDescriptions = Vertex::attributeDescriptions();

  vk::PipelineVertexInputStateCreateInfo vertexInputInfo{
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &bindingDescription,
      .vertexAttributeDescriptionCount =
          static_cast<std::uint32_t>(attributeDescriptions.size()),
      .pVertexAttributeDescriptions = attributeDescriptions.data(),
  };
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

  vk::Format colorAttachmentFormat = swapChain.imageFormat();
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
      .layout = *pipelineLayout,
  };

  return vk::raii::Pipeline(device_.logicalDevice(), nullptr,
                            pipelineCreateInfo);
}

void Renderer::createCommandBuffers() {
  vk::CommandBufferAllocateInfo allocateInfo{
      .commandPool = *commandPool_,
      .level = vk::CommandBufferLevel::ePrimary,
      .commandBufferCount = kFramesInFlight,
  };
  commandBuffers_ =
      vk::raii::CommandBuffers(device_.logicalDevice(), allocateInfo);
}

void Renderer::recordCommandBuffer(vk::raii::CommandBuffer const &commandBuffer,
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
      .color =
          vk::ClearColorValue(std::array<float, 4>{0.05f, 0.07f, 0.10f, 1.0f}),
  };

  vk::RenderingAttachmentInfo colorAttachment{
      .imageView = *swapChain_->imageViews()[imageIndex],
      .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
      .loadOp = vk::AttachmentLoadOp::eClear,
      .storeOp = vk::AttachmentStoreOp::eStore,
      .clearValue = clearValue,
  };
  vk::RenderingInfo renderingInfo{
      .renderArea =
          {
              .offset = {0, 0},
              .extent = swapChain_->extent(),
          },
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &colorAttachment,
  };

  commandBuffer.beginRendering(renderingInfo);

  commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                             *graphicsPipeline_);

  vk::Buffer vertexBuffer = *vertexBuffer_;
  vk::DeviceSize vertexOffset = 0;
  commandBuffer.bindVertexBuffers(0, {vertexBuffer}, {vertexOffset});
  commandBuffer.bindIndexBuffer(*indexBuffer_, 0, vk::IndexType::eUint32);

  vk::Viewport viewport{
      .x = 0.0f,
      .y = 0.0f,
      .width = static_cast<float>(swapChain_->extent().width),
      .height = static_cast<float>(swapChain_->extent().height),
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  vk::Rect2D scissor{
      .offset = {0, 0},
      .extent = swapChain_->extent(),
  };
  commandBuffer.setViewport(0, {viewport});
  commandBuffer.setScissor(0, {scissor});
  PushConstants pushConstants{};
  commandBuffer.pushConstants<PushConstants>(
      *pipelineLayout_, vk::ShaderStageFlagBits::eVertex, 0, pushConstants);
  commandBuffer.drawIndexed(indexCount_, 1, 0, 0, 0);

  commandBuffer.endRendering();

  transitionSwapChainImage(commandBuffer, imageIndex,
                           vk::ImageLayout::ePresentSrcKHR,
                           vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                           vk::AccessFlagBits2::eColorAttachmentWrite,
                           vk::PipelineStageFlagBits2::eAllCommands,
                           vk::AccessFlagBits2::eMemoryRead);

  commandBuffer.end();
}

void Renderer::transitionSwapChainImage(
    vk::raii::CommandBuffer const &commandBuffer, std::uint32_t imageIndex,
    vk::ImageLayout newLayout, vk::PipelineStageFlags2 srcStageMask,
    vk::AccessFlags2 srcAccessMask, vk::PipelineStageFlags2 dstStageMask,
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
      .image = swapChain_->images()[imageIndex],
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
