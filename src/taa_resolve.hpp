#pragma once
#include "device.hpp"
#include "glm_include.hpp"
#include "render_graph.hpp"
#include <array>

enum class TaaHistoryFilter : unsigned { Bilinear = 0, CatmullRom = 1 };

struct TaaPush {
  glm::mat4 inverseRaster{1};
  glm::vec4 depthRow{};
  glm::vec4 jitterWeight{}; // xy current sampling jitter (diagnostic), z
                            // current weight, w filter
  glm::vec4 options{}; // history ready, depth relative tolerance, max motion
                       // px, variance gamma
};
static_assert(sizeof(TaaPush) == 112);
// Owns immutable sources/sets and single-queue ping-pong targets. Graph states
// and parity publish only after submission; reset does not overwrite in-flight
// data.
class TaaResolve {
public:
  static constexpr vk::Format colorFormat = vk::Format::eR16G16B16A16Sfloat;
  static constexpr vk::Format depthFormat = vk::Format::eR32Sfloat;
  TaaResolve(Device const &, vk::Extent2D, vk::ImageView color,
             vk::ImageView depth, vk::ImageView motion,
             vk::ImageView alternativeColor = {});
  struct Frame {
    RenderGraph::ImageId color[2], depth[2];
    RenderGraph::PassId resolve;
    unsigned write = 0;
  };
  Frame addPass(RenderGraph &, RenderGraph::ImageId color,
                RenderGraph::ImageId depth, RenderGraph::ImageId motion) const;
  void draw(vk::CommandBuffer, TaaPush, bool alternative = false) const;
  void submitted(RenderGraph::Plan const &, Frame const &);
  void reset() { ready_ = false; }
  bool ready() const { return ready_; }
  unsigned writeIndex() const { return write_; }
  vk::ImageView colorView(unsigned i) const { return *targets_[i].colorView; }
  vk::Image colorImage(unsigned i) const { return *targets_[i].color.image; }
  RenderGraph::State const &colorState(unsigned i) const {
    return colorStates_[i];
  }

private:
  struct Target {
    GpuImage color, depth;
    vk::raii::ImageView colorView = nullptr, depthView = nullptr;
  };
  Device const &device_;
  vk::Extent2D extent_;
  ResourceLedger::Lease accounting_;
  std::array<Target, 2> targets_;
  vk::raii::Sampler nearest_ = nullptr, linear_ = nullptr;
  vk::raii::DescriptorSetLayout setLayout_ = nullptr;
  vk::raii::DescriptorPool pool_ = nullptr;
  std::array<vk::DescriptorSet, 4> sets_;
  vk::raii::PipelineLayout layout_ = nullptr;
  vk::raii::Pipeline pipeline_ = nullptr;
  std::array<RenderGraph::State, 2> colorStates_, depthStates_;
  unsigned write_ = 0;
  bool ready_ = false;
};
