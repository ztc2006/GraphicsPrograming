#include "pch.hpp"

#include "renderer.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stb_image.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
struct PushConstants {
  glm::mat4 transform{1.0f};
  glm::vec4 materialTint{1.0f};
};

struct FrameUniformBufferObject {
  glm::mat4 viewProj{1.0f};
  glm::vec4 cameraPosition{0.0f};
  glm::vec4 lightDirection{0.0f, 1.0f, 0.3f, 0.0f};
  glm::vec4 lightColor{1.0f, 0.98f, 0.92f, 1.0f};
  glm::vec4 ambientColor{0.08f, 0.08f, 0.1f, 1.0f};
  glm::vec4 lightingParams{1.0f, 0.35f, 32.0f, 0.0f};
};
} // namespace

Renderer::Renderer(Device const &device) : device_(device) {
  createPersistentResources();
}

void Renderer::recreateForSwapChain(SwapChain const &swapChain) {
  if (activeFrame_.has_value()) {
    throw std::runtime_error("Cannot recreate renderer swapchain resources "
                             "while a frame is in progress.");
  }

  validateSwapChainCandidate(swapChain);

  vk::PushConstantRange pushConstantRange{
      .stageFlags =
          vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
      .offset = 0,
      .size = sizeof(PushConstants),
  };

  std::array layouts = {
      *frameDescriptorSetLayout_,
      *materialDescriptorSetLayout_,
  };

  vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo{
      .setLayoutCount = static_cast<std::uint32_t>(layouts.size()),
      .pSetLayouts = layouts.data(),
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pushConstantRange,
  };
  vk::raii::PipelineLayout newPipelineLayout(device_.logicalDevice(),
                                             pipelineLayoutCreateInfo);

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
  DepthResources newDepthResource = createDepthResources(swapChain);

  using std::swap;
  swap(pipelineLayout_, newPipelineLayout);
  swap(graphicsPipeline_, newGraphicsPipeline);
  swap(renderFinishedSemaphores_, newRenderFinishedSemaphores);
  swap(swapChainImageLayouts_, newSwapChainImageLayouts);
  swap(imagesInFlight_, newImagesInFlight);
  swap(swapChain_, newSwapChain);
  swap(depthResources_, newDepthResource);
  currentFrame_ = 0;
  activeFrame_.reset();
}

void Renderer::createPersistentResources() {
  createCommandPool();
  createFrameResources();
  createFrameDescriptorSetLayout();
  createMaterialDescriptorSetLayout();
  createFrameDescriptorPool();
  allocateAndWriteFrameDescriptorSets();
  createCommandBuffers();
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

    auto [uniformBuffer, uniformBufferMemory] =
        device_.createBuffer(sizeof(FrameUniformBufferObject),
                             vk::BufferUsageFlagBits::eUniformBuffer,
                             vk::MemoryPropertyFlagBits::eHostCoherent |
                                 vk::MemoryPropertyFlagBits::eHostVisible);

    frame.uniformBuffer = std::move(uniformBuffer);
    frame.uniformBufferMemory = std::move(uniformBufferMemory);
    frames_.push_back(std::move(frame));
  }
}

Renderer::MeshGpuResources Renderer::createGeometryResources(Mesh const &mesh) {
  auto uploadVectorToDeviceLocalBuffer =
      [this]<typename T>(std::vector<T> const &sourceData,
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

  auto [vertexBuffer, vertexBufferMemory] = uploadVectorToDeviceLocalBuffer(
      mesh.vertices, vk::BufferUsageFlagBits::eVertexBuffer);

  auto [indexBuffer, indexBufferMemory] = uploadVectorToDeviceLocalBuffer(
      mesh.indices, vk::BufferUsageFlagBits::eIndexBuffer);

  MeshGpuResources resources{};
  resources.vertexBuffer = std::move(vertexBuffer);
  resources.vertexBufferMemory = std::move(vertexBufferMemory);
  resources.indexBuffer = std::move(indexBuffer);
  resources.indexBufferMemory = std::move(indexBufferMemory);
  resources.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
  return resources;
}

Renderer::DepthResources
Renderer::createDepthResources(SwapChain const &swapChain) const {
  vk::ImageCreateInfo imageCreateInfo{
      .imageType = vk::ImageType::e2D,
      .format = kDepthFormat,
      .extent =
          {
              .width = swapChain.extent().width,
              .height = swapChain.extent().height,
              .depth = 1,
          },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = vk::SampleCountFlagBits::e1,
      .tiling = vk::ImageTiling::eOptimal,
      .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
      .sharingMode = vk::SharingMode::eExclusive,
      .initialLayout = vk::ImageLayout::eUndefined,
  };

  vk::raii::Image image(device_.logicalDevice(), imageCreateInfo);
  auto memoryRequirements = image.getMemoryRequirements();

  vk::MemoryAllocateInfo allocateInfo{
      .allocationSize = memoryRequirements.size,
      .memoryTypeIndex =
          device_.findMemoryType(memoryRequirements.memoryTypeBits,
                                 vk::MemoryPropertyFlagBits::eDeviceLocal),
  };

  vk::raii::DeviceMemory memory(device_.logicalDevice(), allocateInfo);
  image.bindMemory(*memory, 0);

  vk::ImageViewCreateInfo imageViewCreateInfo{
      .image = *image,
      .viewType = vk::ImageViewType::e2D,
      .format = kDepthFormat,
      .subresourceRange =
          {
              .aspectMask = vk::ImageAspectFlagBits::eDepth,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };
  DepthResources resources{};
  resources.image = std::move(image);
  resources.memory = std::move(memory);
  resources.imageView =
      vk::raii::ImageView(device_.logicalDevice(), imageViewCreateInfo);
  resources.layout = vk::ImageLayout::eUndefined;
  return resources;
}

Renderer::TextureResources Renderer::createCheckerTextureResources() {
  constexpr std::uint32_t width = 64;
  constexpr std::uint32_t height = 64;
  std::vector<std::uint32_t> pixels(width * height);

  for (std::uint32_t y = 0; y < height; y++) {
    for (std::uint32_t x = 0; x < width; x++) {
      bool dark = ((x / 8) + (y / 8)) % 2 == 0;
      pixels[y * width + x] = dark ? 0xff303030u : 0xffd8d8d8u;
    }
  }

  vk::DeviceSize imageSize = sizeof(std::uint32_t) * pixels.size();

  auto [stagingBuffer, stagingMemory] =
      device_.createBuffer(imageSize, vk::BufferUsageFlagBits::eTransferSrc,
                           vk::MemoryPropertyFlagBits::eHostVisible |
                               vk::MemoryPropertyFlagBits::eHostCoherent);

  void *mapped = stagingMemory.mapMemory(0, imageSize);
  std::memcpy(mapped, pixels.data(), static_cast<std::size_t>(imageSize));
  stagingMemory.unmapMemory();

  vk::ImageCreateInfo imageCreateInfo{
      .imageType = vk::ImageType::e2D,
      .format = vk::Format::eR8G8B8A8Unorm,
      .extent =
          {
              .width = width,
              .height = height,
              .depth = 1,
          },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = vk::SampleCountFlagBits::e1,
      .tiling = vk::ImageTiling::eOptimal,
      .usage = vk::ImageUsageFlagBits::eTransferDst |
               vk::ImageUsageFlagBits::eSampled,
      .sharingMode = vk::SharingMode::eExclusive,
      .initialLayout = vk::ImageLayout::eUndefined,
  };
  vk::raii::Image image(device_.logicalDevice(), imageCreateInfo);
  auto memoryRequirements = image.getMemoryRequirements();

  vk::MemoryAllocateInfo allocateInfo{
      .allocationSize = memoryRequirements.size,
      .memoryTypeIndex =
          device_.findMemoryType(memoryRequirements.memoryTypeBits,
                                 vk::MemoryPropertyFlagBits::eDeviceLocal),
  };
  vk::raii::DeviceMemory memory(device_.logicalDevice(), allocateInfo);
  image.bindMemory(*memory, 0);

  TextureResources resources{};
  resources.image = std::move(image);
  resources.memory = std::move(memory);

  transitionTextureImage(resources, vk::ImageLayout::eTransferDstOptimal,
                         vk::PipelineStageFlagBits2::eTopOfPipe, {},
                         vk::PipelineStageFlagBits2::eTransfer,
                         vk::AccessFlagBits2::eTransferWrite);
  copyBufferToImage(*stagingBuffer, *resources.image, width, height);
  transitionTextureImage(resources, vk::ImageLayout::eShaderReadOnlyOptimal,
                         vk::PipelineStageFlagBits2::eTransfer,
                         vk::AccessFlagBits2::eTransferWrite,
                         vk::PipelineStageFlagBits2::eFragmentShader,
                         vk::AccessFlagBits2::eShaderSampledRead);

  vk::ImageViewCreateInfo imageViewCreateInfo{
      .image = *resources.image,
      .viewType = vk::ImageViewType::e2D,
      .format = vk::Format::eR8G8B8A8Unorm,
      .subresourceRange =
          {
              .aspectMask = vk::ImageAspectFlagBits::eColor,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };
  resources.imageView =
      vk::raii::ImageView(device_.logicalDevice(), imageViewCreateInfo);

  vk::SamplerCreateInfo samplerCreateInfo{
      .magFilter = vk::Filter::eNearest,
      .minFilter = vk::Filter::eNearest,
      .mipmapMode = vk::SamplerMipmapMode::eNearest,
      .addressModeU = vk::SamplerAddressMode::eRepeat,
      .addressModeV = vk::SamplerAddressMode::eRepeat,
      .addressModeW = vk::SamplerAddressMode::eRepeat,
      .mipLodBias = 0.0f,
      .anisotropyEnable = false,
      .maxAnisotropy = 1.0f,
      .compareEnable = false,
      .compareOp = vk::CompareOp::eAlways,
      .minLod = 0.0f,
      .maxLod = 0.0f,
      .borderColor = vk::BorderColor::eIntOpaqueBlack,
      .unnormalizedCoordinates = false,
  };
  resources.sampler =
      vk::raii::Sampler(device_.logicalDevice(), samplerCreateInfo);

  return resources;
}

void Renderer::setMeshes(std::vector<Mesh> const &meshes) {
  if (activeFrame_.has_value()) {
    throw std::runtime_error(
        "Cannot replace renderer meshes while a frame is in progress");
  }

  if (meshes.empty()) {
    throw std::runtime_error("Renderer requires at least one mesh.");
  }

  std::vector<MeshGpuResources> newMeshGpuResources;
  newMeshGpuResources.reserve(meshes.size());

  for (Mesh const &mesh : meshes) {
    if (mesh.vertices.empty()) {
      throw std::runtime_error("Mesh has no vertices.");
    }
    if (mesh.indices.empty()) {
      throw std::runtime_error("Mesh has no indices.");
    }
    newMeshGpuResources.push_back(createGeometryResources(mesh));
  }
  meshGpuResources_.swap(newMeshGpuResources);
}

void Renderer::setMaterials(std::vector<Material> const &materials) {
  if (activeFrame_.has_value()) {
    throw std::runtime_error(
        "Cannot replace renderer materials while a frame is in progress.");
  }
  if (materials.empty()) {
    throw std::runtime_error("Renderer requires at least one material.");
  }

  std::vector<MaterialGpuResources> newMaterials;
  newMaterials.reserve(materials.size());

  for (Material const &material : materials) {
    if (material.albedoPath.empty()) {
      throw std::runtime_error("Material albedo path is empty.");
    }

    MaterialGpuResources resources{};
    resources.albedoTexture =
        createTextureResourcesFromFile(material.albedoPath);
    resources.tint = material.tint;
    newMaterials.push_back(std::move(resources));
  }

  vk::raii::DescriptorPool newPool = createMaterialDescriptorPool(
      static_cast<std::uint32_t>(newMaterials.size()));
  materialGpuResources_.swap(newMaterials);
  materialDescriptorPool_ = std::move(newPool);
  writeMaterialDescriptorSets();
}

void Renderer::setMaterialTint(MaterialId materialId, glm::vec4 const &tint) {
  if (materialId >= materialGpuResources_.size()) {
    throw std::runtime_error("Renderer material id is out of range.");
  }

  materialGpuResources_[materialId].tint = tint;
}

void Renderer::setUiDrawCallback(
    std::function<void(vk::CommandBuffer)> callback) {
  uiDrawCallback_ = std::move(callback);
}

Renderer::TextureResources
Renderer::createTextureResourcesFromFile(std::string const &path) {
  int width = 0;
  int height = 0;
  int channels = 0;

  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
      stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha),
      stbi_image_free);

  if (!pixels) {
    throw std::runtime_error("Failed to load texture: " + path + " (" +
                             stbi_failure_reason() + ")");
  }
  if (width <= 0 || height <= 0) {
    throw std::runtime_error("Texture has invalid dimensions: " + path);
  }

  vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(width) *
                             static_cast<vk::DeviceSize>(height) * 4;

  auto [stagingBuffer, stagingMemory] =
      device_.createBuffer(imageSize, vk::BufferUsageFlagBits::eTransferSrc,
                           vk::MemoryPropertyFlagBits::eHostVisible |
                               vk::MemoryPropertyFlagBits::eHostCoherent);

  void *mapped = stagingMemory.mapMemory(0, imageSize);
  std::memcpy(mapped, pixels.get(), static_cast<std::size_t>(imageSize));
  stagingMemory.unmapMemory();

  vk::ImageCreateInfo imageCreateInfo{
      .imageType = vk::ImageType::e2D,
      .format = vk::Format::eR8G8B8A8Unorm,
      .extent = {static_cast<std::uint32_t>(width),
                 static_cast<std::uint32_t>(height), 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = vk::SampleCountFlagBits::e1,
      .tiling = vk::ImageTiling::eOptimal,
      .usage = vk::ImageUsageFlagBits::eTransferDst |
               vk::ImageUsageFlagBits::eSampled,
      .sharingMode = vk::SharingMode::eExclusive,
      .initialLayout = vk::ImageLayout::eUndefined,
  };

  vk::raii::Image image(device_.logicalDevice(), imageCreateInfo);
  auto memoryRequirements = image.getMemoryRequirements();

  vk::MemoryAllocateInfo allocteInfo{
      .allocationSize = memoryRequirements.size,
      .memoryTypeIndex =
          device_.findMemoryType(memoryRequirements.memoryTypeBits,
                                 vk::MemoryPropertyFlagBits::eDeviceLocal),
  };

  vk::raii::DeviceMemory memory(device_.logicalDevice(), allocteInfo);
  image.bindMemory(*memory, 0);

  TextureResources resources{};
  resources.image = std::move(image);
  resources.memory = std::move(memory);

  transitionTextureImage(resources, vk::ImageLayout::eTransferDstOptimal,
                         vk::PipelineStageFlagBits2::eTopOfPipe, {},
                         vk::PipelineStageFlagBits2::eTransfer,
                         vk::AccessFlagBits2::eTransferWrite);

  copyBufferToImage(*stagingBuffer, *resources.image,
                    static_cast<std::uint32_t>(width),
                    static_cast<std::uint32_t>(height));

  transitionTextureImage(resources, vk::ImageLayout::eShaderReadOnlyOptimal,
                         vk::PipelineStageFlagBits2::eTransfer,
                         vk::AccessFlagBits2::eTransferWrite,
                         vk::PipelineStageFlagBits2::eFragmentShader,
                         vk::AccessFlagBits2::eShaderSampledRead);

  vk::ImageViewCreateInfo imageViewCreateInfo{
      .image = *resources.image,
      .viewType = vk::ImageViewType::e2D,
      .format = vk::Format::eR8G8B8A8Unorm,
      .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
  };
  resources.imageView =
      vk::raii::ImageView(device_.logicalDevice(), imageViewCreateInfo);

  vk::SamplerCreateInfo samplerCreateInfo{
      .magFilter = vk::Filter::eLinear,
      .minFilter = vk::Filter::eLinear,
      .mipmapMode = vk::SamplerMipmapMode::eNearest,
      .addressModeU = vk::SamplerAddressMode::eRepeat,
      .addressModeV = vk::SamplerAddressMode::eRepeat,
      .addressModeW = vk::SamplerAddressMode::eRepeat,
      .maxAnisotropy = 1.0f,
      .compareOp = vk::CompareOp::eAlways,
      .borderColor = vk::BorderColor::eIntOpaqueBlack,
  };
  resources.sampler =
      vk::raii::Sampler(device_.logicalDevice(), samplerCreateInfo);

  return resources;
}
void Renderer::createFrameDescriptorSetLayout() {
  vk::DescriptorSetLayoutBinding binding{
      .binding = 0,
      .descriptorType = vk::DescriptorType::eUniformBuffer,
      .descriptorCount = 1,
      .stageFlags =
          vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
  };

  vk::DescriptorSetLayoutCreateInfo createInfo{
      .bindingCount = 1,
      .pBindings = &binding,
  };
  frameDescriptorSetLayout_ =
      vk::raii::DescriptorSetLayout(device_.logicalDevice(), createInfo);
}

void Renderer::createMaterialDescriptorSetLayout() {
  vk::DescriptorSetLayoutBinding binding{
      .binding = 0,
      .descriptorType = vk::DescriptorType::eCombinedImageSampler,
      .descriptorCount = 1,
      .stageFlags = vk::ShaderStageFlagBits::eFragment,
  };

  vk::DescriptorSetLayoutCreateInfo createInfo{
      .bindingCount = 1,
      .pBindings = &binding,
  };
  materialDescriptorSetLayout_ =
      vk::raii::DescriptorSetLayout(device_.logicalDevice(), createInfo);
}

void Renderer::createFrameDescriptorPool() {
  vk::DescriptorPoolSize poolSize{
      .type = vk::DescriptorType::eUniformBuffer,
      .descriptorCount = kFramesInFlight,
  };

  vk::DescriptorPoolCreateInfo createInfo{
      .maxSets = kFramesInFlight,
      .poolSizeCount = 1,
      .pPoolSizes = &poolSize,
  };
  frameDescriptorPool_ =
      vk::raii::DescriptorPool(device_.logicalDevice(), createInfo);
}

vk::raii::DescriptorPool
Renderer::createMaterialDescriptorPool(std::uint32_t materialCount) const {
  vk::DescriptorPoolSize poolSize{
      .type = vk::DescriptorType::eCombinedImageSampler,
      .descriptorCount = materialCount,
  };

  vk::DescriptorPoolCreateInfo createInfo{
      .maxSets = materialCount,
      .poolSizeCount = 1,
      .pPoolSizes = &poolSize,
  };
  return vk::raii::DescriptorPool(device_.logicalDevice(), createInfo);
}

void Renderer::allocateAndWriteFrameDescriptorSets() {
  std::vector<vk::DescriptorSetLayout> layouts(frames_.size(),
                                               *frameDescriptorSetLayout_);
  vk::DescriptorSetAllocateInfo allocateInfo{
      .descriptorPool = *frameDescriptorPool_,
      .descriptorSetCount = static_cast<std::uint32_t>(layouts.size()),
      .pSetLayouts = layouts.data(),
  };

  auto descriptorSets =
      (*device_.logicalDevice()).allocateDescriptorSets(allocateInfo);

  for (std::size_t index = 0; index < frames_.size(); ++index) {
    frames_[index].descriptorSet = descriptorSets[index];

    vk::DescriptorBufferInfo bufferInfo{
        .buffer = *frames_[index].uniformBuffer,
        .offset = 0,
        .range = sizeof(FrameUniformBufferObject),
    };

    vk::WriteDescriptorSet write{
        .dstSet = frames_[index].descriptorSet,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eUniformBuffer,
        .pBufferInfo = &bufferInfo,
    };

    device_.logicalDevice().updateDescriptorSets({write}, {});
  }
}

void Renderer::writeMaterialDescriptorSets() {
  std::vector<vk::DescriptorSetLayout> layouts(materialGpuResources_.size(),
                                               *materialDescriptorSetLayout_);

  vk::DescriptorSetAllocateInfo allocateInfo{
      .descriptorPool = *materialDescriptorPool_,
      .descriptorSetCount = static_cast<std::uint32_t>(layouts.size()),
      .pSetLayouts = layouts.data(),
  };

  auto descriptorSets =
      (*device_.logicalDevice()).allocateDescriptorSets(allocateInfo);

  for (std::size_t index = 0; index < materialGpuResources_.size(); ++index) {
    auto &material = materialGpuResources_[index];
    material.descriptorSet = descriptorSets[index];

    vk::DescriptorImageInfo imageInfo{
        .sampler = *material.albedoTexture.sampler,
        .imageView = *material.albedoTexture.imageView,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };

    vk::WriteDescriptorSet write{
        .dstSet = material.descriptorSet,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &imageInfo,
    };

    device_.logicalDevice().updateDescriptorSets({write}, {});
  }
}

Renderer::FrameResult Renderer::beginFrame(glm::mat4 const &viewProjMatrix,
                                           glm::vec3 const &cameraPosition,
                                           LightingSettings const &lighting) {
  validateSwapChainState();

  if (activeFrame_.has_value()) {
    throw std::runtime_error(
        "Cannot begin a new frame while another frame is in progress.");
  }

  std::uint32_t const frameIndex = currentFrame_;
  auto &frame = frames_[frameIndex];
  auto &commandBuffer = commandBuffers_[frameIndex];

  (void)device_.logicalDevice().waitForFences(
      {*frame.inFlightFence}, true, std::numeric_limits<std::uint64_t>::max());

  vk::Result acquireResult = vk::Result::eSuccess;
  std::uint32_t imageIndex = 0;

  try {
    auto acquire = swapChain_->handle().acquireNextImage(
        std::numeric_limits<std::uint64_t>::max(),
        *frame.imageAvailableSemaphore, nullptr);
    acquireResult = acquire.result;
    imageIndex = acquire.value;
  } catch (vk::OutOfDateKHRError const &) {
    return FrameResult::eSwapChainOutOfDate;
  }

  if (acquireResult != vk::Result::eSuccess &&
      acquireResult != vk::Result::eSuboptimalKHR) {
    throw std::runtime_error("Failed to acquire swapchain image.");
  }

  if (imageIndex >= renderFinishedSemaphores_.size()) {
    throw std::runtime_error("Acquire swapchain image index is out of range");
  }

  if (imagesInFlight_[imageIndex]) {
    (void)device_.logicalDevice().waitForFences(
        {imagesInFlight_[imageIndex]}, true,
        std::numeric_limits<std::uint64_t>::max());
  }

  imagesInFlight_[imageIndex] = *frame.inFlightFence;

  device_.logicalDevice().resetFences({*frame.inFlightFence});

  commandBuffer.reset();
  updateFrameUniformBuffer(frame, viewProjMatrix, cameraPosition, lighting);
  beginCommandBuffer(commandBuffer, frame, imageIndex);

  activeFrame_ = ActiveFrameState{
      .frameIndex = frameIndex,
      .imageIndex = imageIndex,
      .acquireResult = acquireResult,
  };

  return FrameResult::eSuccess;
}

void Renderer::drawObject(MeshId meshId, MaterialId materialId,
                          glm::mat4 const &modelMatrix) {
  if (!activeFrame_.has_value()) {
    throw std::runtime_error("Cannot draw without an active frame.");
  }

  if (meshId >= meshGpuResources_.size()) {
    throw std::runtime_error("Renderer mesh id is out of range.");
  }

  if (materialId >= materialGpuResources_.size()) {
    throw std::runtime_error("Renderer material id is out of range.");
  }

  MeshGpuResources const &meshResources = meshGpuResources_[meshId];
  MaterialGpuResources const &materialResource =
      materialGpuResources_[materialId];

  auto const &frameState = *activeFrame_;
  auto &commandBuffer = commandBuffers_[frameState.frameIndex];

  vk::Buffer vertexBuffer = *meshResources.vertexBuffer;
  vk::DeviceSize vertexOffset = 0;
  commandBuffer.bindVertexBuffers(0, {vertexBuffer}, {vertexOffset});
  commandBuffer.bindIndexBuffer(*meshResources.indexBuffer, 0,
                                vk::IndexType::eUint32);

  commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   *pipelineLayout_, 1,
                                   {materialResource.descriptorSet}, {});

  PushConstants pushConstants{
      .transform = modelMatrix,
      .materialTint = materialResource.tint,
  };

  commandBuffer.pushConstants<PushConstants>(
      *pipelineLayout_,
      vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
      pushConstants);

  commandBuffer.drawIndexed(meshResources.indexCount, 1, 0, 0, 0);
}

Renderer::FrameResult Renderer::endFrame() {
  if (!activeFrame_.has_value()) {
    throw std::runtime_error(
        "Cannot end a frame when no frame is in progress.");
  }

  ActiveFrameState const frameState = *activeFrame_;
  auto &frame = frames_[frameState.frameIndex];
  auto &commandBuffer = commandBuffers_[frameState.frameIndex];

  endCommandBuffer(commandBuffer, frameState.imageIndex);

  vk::Semaphore waitSemaphore = *frame.imageAvailableSemaphore;
  vk::PipelineStageFlags waitStage =
      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  vk::CommandBuffer rawCommandBuffer = *commandBuffer;
  vk::Semaphore signalSemaphore =
      *renderFinishedSemaphores_[frameState.imageIndex];

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
      .pImageIndices = &frameState.imageIndex,
  };

  auto advanceFrame = [this]() {
    currentFrame_ = (currentFrame_ + 1) % kFramesInFlight;
  };

  vk::Result presentResult = vk::Result::eSuccess;
  try {
    presentResult = device_.presentQueue().presentKHR(presentInfo);
  } catch (vk::OutOfDateKHRError const &) {
    activeFrame_.reset();
    advanceFrame();
    return FrameResult::eSwapChainOutOfDate;
  }

  if (presentResult != vk::Result::eSuccess &&
      presentResult != vk::Result::eSuboptimalKHR) {
    throw std::runtime_error("Failed to present swapchain image.");
  }

  activeFrame_.reset();

  if (frameState.acquireResult == vk::Result::eSuboptimalKHR ||
      presentResult == vk::Result::eSuboptimalKHR) {
    advanceFrame();
    return FrameResult::eSwapChainSuboptimal;
  }

  advanceFrame();
  return FrameResult::eSuccess;
}

Renderer::FrameResult Renderer::drawFrame(MeshId meshId, MaterialId materialId,
                                          glm::mat4 const &modelMatrix,
                                          glm::mat4 const &viewProjMatrix,
                                          glm::vec3 const &cameraPosition,
                                          LightingSettings const &lighting) {
  FrameResult beginResult =
      beginFrame(viewProjMatrix, cameraPosition, lighting);
  if (beginResult != FrameResult::eSuccess) {
    return beginResult;
  }

  drawObject(meshId, materialId, modelMatrix);
  return endFrame();
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

  auto const imageCount = swapChain_->images().size();
  if (imageCount == 0) {
    throw std::runtime_error("Renderer swapchain has no images.");
  }

  if (activeFrame_.has_value()) {
    if (activeFrame_->frameIndex >= frames_.size()) {
      throw std::runtime_error("Renderer active frame index is out of range.");
    }

    if (activeFrame_->imageIndex >= imageCount) {
      throw std::runtime_error(
          "Renderer active swapchain image index is out of range.");
    }
  }

  if (pipelineLayout_ == nullptr) {
    throw std::runtime_error("Renderer pipeline layout is not initialized.");
  }

  if (graphicsPipeline_ == nullptr) {
    throw std::runtime_error("Renderer graphics pipeline is not initialized.");
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

  for (auto const &frame : frames_) {
    if (frame.uniformBuffer == nullptr ||
        frame.uniformBufferMemory == nullptr ||
        frame.descriptorSet == nullptr) {
      throw std::runtime_error(
          "Renderer frame uniform resources are not initialized.");
    }
  }

  if (meshGpuResources_.empty()) {
    throw std::runtime_error(
        "Renderer mesh GPU resources are not initialized.");
  }

  if (materialGpuResources_.empty()) {
    throw std::runtime_error(
        "Renderer material GPU resources are not initialized.");
  }

  if (depthResources_.image == nullptr || depthResources_.memory == nullptr ||
      depthResources_.imageView == nullptr) {
    throw std::runtime_error("Renderer depth resources are not initialized.");
  }

  for (auto const &meshResources : meshGpuResources_) {
    if (meshResources.vertexBuffer == nullptr ||
        meshResources.vertexBufferMemory == nullptr ||
        meshResources.indexBuffer == nullptr ||
        meshResources.indexBufferMemory == nullptr ||
        meshResources.indexCount == 0) {
      throw std::runtime_error(
          "Renderer mesh GPU resources are not initialized.");
    }
  }

  for (auto const &materialResources : materialGpuResources_) {
    if (materialResources.albedoTexture.image == nullptr ||
        materialResources.albedoTexture.memory == nullptr ||
        materialResources.albedoTexture.imageView == nullptr ||
        materialResources.albedoTexture.sampler == nullptr ||
        materialResources.descriptorSet == nullptr) {
      throw std::runtime_error(
          "Renderer material GPU resources are not initialized.");
    }
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
      .frontFace = vk::FrontFace::eCounterClockwise,
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

  vk::PipelineDepthStencilStateCreateInfo depthStencil{
      .depthTestEnable = true,
      .depthWriteEnable = true,
      .depthCompareOp = vk::CompareOp::eLess,
      .depthBoundsTestEnable = false,
      .stencilTestEnable = false,
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
      .depthAttachmentFormat = kDepthFormat,
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
      .pDepthStencilState = &depthStencil,
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

void Renderer::updateFrameUniformBuffer(FrameContext &frame,
                                        glm::mat4 const &viewProjMatrix,
                                        glm::vec3 const &cameraPosition,
                                        LightingSettings const &lighting) const {
  FrameUniformBufferObject ubo{};
  ubo.viewProj = viewProjMatrix;
  ubo.cameraPosition = glm::vec4(cameraPosition, 1.0f);
  glm::vec3 lightDirection = lighting.direction;
  if (glm::length(lightDirection) <= 0.0001f) {
    lightDirection = {0.0f, 1.0f, 0.0f};
  }
  ubo.lightDirection = glm::vec4(glm::normalize(lightDirection), 0.0f);
  glm::vec3 const lightColor = lighting.color * lighting.intensity;
  ubo.lightColor = glm::vec4(lightColor, 1.0f);
  ubo.ambientColor =
      glm::vec4(lightColor * lighting.ambientStrength, 1.0f);
  ubo.lightingParams = glm::vec4(lighting.diffuseStrength,
                                 lighting.specularStrength,
                                 lighting.shininess, 0.0f);

  void *mapped = frame.uniformBufferMemory.mapMemory(0, sizeof(ubo));
  std::memcpy(mapped, &ubo, sizeof(ubo));
  frame.uniformBufferMemory.unmapMemory();
}

void Renderer::beginCommandBuffer(vk::raii::CommandBuffer const &commandBuffer,
                                  FrameContext const &frame,
                                  std::uint32_t imageIndex) {
  commandBuffer.begin(vk::CommandBufferBeginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
  });

  vk::ClearValue depthClearValue{
      .depthStencil =
          vk::ClearDepthStencilValue{
              .depth = 1.0f,
              .stencil = 0,
          },
  };

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

  transitionDepthImage(commandBuffer, vk::ImageLayout::eDepthAttachmentOptimal,
                       vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests,
                       {},
                       vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests,
                       vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                           vk::AccessFlagBits2::eDepthStencilAttachmentWrite);

  vk::RenderingAttachmentInfo depthAttachment{
      .imageView = *depthResources_.imageView,
      .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
      .loadOp = vk::AttachmentLoadOp::eClear,
      .storeOp = vk::AttachmentStoreOp::eDontCare,
      .clearValue = depthClearValue,
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
      .pDepthAttachment = &depthAttachment,
  };

  commandBuffer.beginRendering(renderingInfo);

  commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                             *graphicsPipeline_);
  commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   *pipelineLayout_, 0, {frame.descriptorSet},
                                   {});

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
}

void Renderer::endCommandBuffer(vk::raii::CommandBuffer const &commandBuffer,
                                std::uint32_t imageIndex) {
  if (uiDrawCallback_) {
    uiDrawCallback_(*commandBuffer);
  }

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

void Renderer::transitionDepthImage(
    vk::raii::CommandBuffer const &commanderBuffer, vk::ImageLayout newLayout,
    vk::PipelineStageFlags2 srcStageMask, vk::AccessFlags2 srcAccessMask,
    vk::PipelineStageFlags2 dstStageMask, vk::AccessFlags2 dstAccessMask) {
  vk::ImageMemoryBarrier2 barrier{
      .srcStageMask = srcStageMask,
      .srcAccessMask = srcAccessMask,
      .dstStageMask = dstStageMask,
      .dstAccessMask = dstAccessMask,
      .oldLayout = depthResources_.layout,
      .newLayout = newLayout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = *depthResources_.image,
      .subresourceRange =
          {
              .aspectMask = vk::ImageAspectFlagBits::eDepth,
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

  commanderBuffer.pipelineBarrier2(dependencyInfo);
  depthResources_.layout = newLayout;
}

void Renderer::transitionTextureImage(TextureResources &texture,
                                      vk::ImageLayout newLayout,
                                      vk::PipelineStageFlags2 srcStageMask,
                                      vk::AccessFlags2 srcAccessMask,
                                      vk::PipelineStageFlags2 dstStageMask,
                                      vk::AccessFlags2 dstAccessMask) {
  vk::CommandPoolCreateInfo commandPoolCreateInfo{
      .flags = vk::CommandPoolCreateFlagBits::eTransient,
      .queueFamilyIndex = device_.graphicsQueueFamilyIndex(),
  };
  vk::raii::CommandPool commandPool(device_.logicalDevice(),
                                    commandPoolCreateInfo);

  vk::CommandBufferAllocateInfo allocateInfo{
      .commandPool = *commandPool,
      .level = vk::CommandBufferLevel::ePrimary,
      .commandBufferCount = 1,
  };
  vk::raii::CommandBuffers commandBuffers(device_.logicalDevice(),
                                          allocateInfo);
  auto const &commandBuffer = commandBuffers.front();

  commandBuffer.begin(vk::CommandBufferBeginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
  });

  vk::ImageMemoryBarrier2 barrier{
      .srcStageMask = srcStageMask,
      .srcAccessMask = srcAccessMask,
      .dstStageMask = dstStageMask,
      .dstAccessMask = dstAccessMask,
      .oldLayout = texture.layout,
      .newLayout = newLayout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = *texture.image,
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
  commandBuffer.end();

  vk::CommandBuffer rawCommandBuffer = *commandBuffer;
  vk::SubmitInfo submitInfo{
      .commandBufferCount = 1,
      .pCommandBuffers = &rawCommandBuffer,
  };
  device_.graphicsQueue().submit({submitInfo}, nullptr);
  device_.graphicsQueue().waitIdle();

  texture.layout = newLayout;
}

void Renderer::copyBufferToImage(vk::Buffer sourceBuffer,
                                 vk::Image destinationImage,
                                 std::uint32_t width, std::uint32_t height) {
  vk::CommandPoolCreateInfo commandPoolCreateInfo{
      .flags = vk::CommandPoolCreateFlagBits::eTransient,
      .queueFamilyIndex = device_.graphicsQueueFamilyIndex(),
  };
  vk::raii::CommandPool commandPool(device_.logicalDevice(),
                                    commandPoolCreateInfo);

  vk::CommandBufferAllocateInfo allocateInfo{
      .commandPool = *commandPool,
      .level = vk::CommandBufferLevel::ePrimary,
      .commandBufferCount = 1,
  };
  vk::raii::CommandBuffers commandBuffers(device_.logicalDevice(),
                                          allocateInfo);
  auto const &commandBuffer = commandBuffers.front();

  commandBuffer.begin(vk::CommandBufferBeginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
  });

  vk::BufferImageCopy copyRegion{
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource =
          {
              .aspectMask = vk::ImageAspectFlagBits::eColor,
              .mipLevel = 0,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
      .imageOffset = {0, 0, 0},
      .imageExtent = {width, height, 1},
  };
  commandBuffer.copyBufferToImage(sourceBuffer, destinationImage,
                                  vk::ImageLayout::eTransferDstOptimal,
                                  {copyRegion});
  commandBuffer.end();

  vk::CommandBuffer rawCommandBuffer = *commandBuffer;
  vk::SubmitInfo submitInfo{
      .commandBufferCount = 1,
      .pCommandBuffers = &rawCommandBuffer,
  };
  device_.graphicsQueue().submit({submitInfo}, nullptr);
  device_.graphicsQueue().waitIdle();
}
