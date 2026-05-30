#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "device.hpp"

struct TextureResources {
  vk::raii::Image image = nullptr;
  vk::raii::DeviceMemory memory = nullptr;
  vk::raii::ImageView imageView = nullptr;
  vk::raii::Sampler sampler = nullptr;
  vk::ImageLayout layout = vk::ImageLayout::eUndefined;
};

class TextureLoader {
public:
  explicit TextureLoader(Device const &device) : device_(device) {}

  TextureResources createFromFile(std::string const &path) const;
  TextureResources
  createSolidColor(std::array<std::uint8_t, 4> const &color) const;

private:
  TextureResources createFromPixels(std::uint8_t const *pixels,
                                    std::uint32_t width,
                                    std::uint32_t height) const;
  void transitionImage(TextureResources &texture, vk::ImageLayout newLayout,
                       vk::PipelineStageFlags2 srcStageMask,
                       vk::AccessFlags2 srcAccessMask,
                       vk::PipelineStageFlags2 dstStageMask,
                       vk::AccessFlags2 dstAccessMask) const;
  void copyBufferToImage(vk::Buffer sourceBuffer, vk::Image destinationImage,
                         std::uint32_t width, std::uint32_t height) const;

private:
  Device const &device_;
};
