#include "pch.hpp"

#include "texture.hpp"

#include <cstring>
#include <memory>
#include <stb_image.h>
#include <stdexcept>

TextureResources TextureLoader::createFromFile(std::string const &path) const {
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

  transitionImage(resources, vk::ImageLayout::eTransferDstOptimal,
                  vk::PipelineStageFlagBits2::eTopOfPipe, {},
                  vk::PipelineStageFlagBits2::eTransfer,
                  vk::AccessFlagBits2::eTransferWrite);

  copyBufferToImage(*stagingBuffer, *resources.image,
                    static_cast<std::uint32_t>(width),
                    static_cast<std::uint32_t>(height));

  transitionImage(resources, vk::ImageLayout::eShaderReadOnlyOptimal,
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

void TextureLoader::transitionImage(TextureResources &texture,
                                    vk::ImageLayout newLayout,
                                    vk::PipelineStageFlags2 srcStageMask,
                                    vk::AccessFlags2 srcAccessMask,
                                    vk::PipelineStageFlags2 dstStageMask,
                                    vk::AccessFlags2 dstAccessMask) const {
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

void TextureLoader::copyBufferToImage(vk::Buffer sourceBuffer,
                                      vk::Image destinationImage,
                                      std::uint32_t width,
                                      std::uint32_t height) const {
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
