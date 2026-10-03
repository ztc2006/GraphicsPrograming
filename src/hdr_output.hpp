#pragma once

#include "device.hpp"

struct DisplaySettings {
  float exposureEv = 0.0f;
  bool toneMap = true;
};
void validateDisplaySettings(DisplaySettings settings);
// Only SDR sRGB-nonlinear 8-bit surfaces are currently supported.
bool displayUsesHardwareSrgb(vk::Format format);

// One native-resolution scene target and its display transform. Caller owns
// render-pass boundaries, destination layout and GPU completion before release.
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
  vk::ImageLayout sceneLayout() const { return layout_; }
  void prepareScene(vk::CommandBuffer command);
  // End scene rendering before this call. Leaves destination rendering OPEN
  // for display-referred UI. Uses the constructor's display format and extent.
  void drawDisplay(vk::CommandBuffer command, vk::ImageView destination,
                   DisplaySettings settings);

private:
  void transition(vk::CommandBuffer command, vk::ImageLayout layout,
                  vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
                  vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess);
  vk::raii::Pipeline createPipeline(vk::Format displayFormat) const;
  Device const &device_;
  vk::Extent2D extent_;
  bool hardwareSrgb_ = true;
  vk::ImageLayout layout_ = vk::ImageLayout::eUndefined;
  ResourceLedger::Lease accounting_;
  GpuImage scene_;
  vk::raii::ImageView view_ = nullptr;
  vk::raii::Sampler sampler_ = nullptr;
  vk::raii::DescriptorSetLayout descriptorLayout_ = nullptr;
  vk::raii::DescriptorPool descriptorPool_ = nullptr;
  vk::DescriptorSet descriptor_ = nullptr;
  vk::raii::PipelineLayout pipelineLayout_ = nullptr;
  vk::raii::Pipeline pipeline_ = nullptr;
};
