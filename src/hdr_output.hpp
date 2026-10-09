#pragma once

#include "device.hpp"

struct DisplaySettings {
  float exposureEv = 0.0f;
  bool toneMap = true;
};
void validateDisplaySettings(DisplaySettings settings);
// Only SDR sRGB-nonlinear 8-bit surfaces are currently supported.
bool displayUsesHardwareSrgb(vk::Format format);

// One native-resolution scene target and its display transform. The graph owns
// all image states and rendering boundaries; caller ensures GPU completion.
// Construct a replacement fully, then swap it only after old frame users
// finish.
class HdrOutput {
public:
  static constexpr vk::Format sceneFormat = vk::Format::eR16G16B16A16Sfloat;
  HdrOutput(Device const &device, vk::Extent2D extent,
            vk::Format displayFormat);
  HdrOutput(HdrOutput const &) = delete;
  HdrOutput &operator=(HdrOutput const &) = delete;
  vk::Image sceneImage() const { return *scene_.image; }
  vk::ImageView sceneView() const { return *view_; }
  vk::Extent2D extent() const { return extent_; }
  // Called inside a graph rendering pass with its target format and extent.
  // Scene image must already be in shader-read-only layout.
  void configureRayTracingView(vk::ImageView);
  void configureTemporalViews(std::array<vk::ImageView, 3> views);
  void drawDisplay(vk::CommandBuffer command, DisplaySettings settings, int temporalIndex=-1);

private:
  vk::raii::Pipeline createPipeline(vk::Format displayFormat) const;
  Device const &device_;
  vk::Extent2D extent_;
  bool hardwareSrgb_ = true;
  ResourceLedger::Lease accounting_;
  GpuImage scene_;
  vk::raii::ImageView view_ = nullptr;
  vk::raii::Sampler sampler_ = nullptr;
  vk::raii::DescriptorSetLayout descriptorLayout_ = nullptr;
  vk::raii::DescriptorPool descriptorPool_ = nullptr;
  vk::DescriptorSet descriptor_ = nullptr;
  std::array<vk::DescriptorSet, 4> temporalDescriptors_{};
  ResourceLedger::Lease rtAccounting_;
  vk::raii::DescriptorPool rtPool_=nullptr;
  ResourceLedger::Lease temporalAccounting_;
  vk::raii::DescriptorPool temporalPool_=nullptr;
  vk::raii::PipelineLayout pipelineLayout_ = nullptr;
  vk::raii::Pipeline pipeline_ = nullptr;
};
