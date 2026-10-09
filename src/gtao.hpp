#pragma once
#include "device.hpp"
#include "glm_include.hpp"
#include "render_graph.hpp"
#include <array>

enum class AoDebug : unsigned { None, Raw, Filtered };
struct AoSettings {
  bool enabled = false;
  float radius = .5f, strength = 1.f;
  AoDebug debug = AoDebug::None;
  bool operator==(AoSettings const &) const = default;
};
void validateAoSettings(AoSettings const &);
struct GtaoPush {
  glm::mat4 inverseRaster{1};
  glm::vec4 camera{};
  glm::vec4 settings{.5f, 1.f, 0.f,
                     128.f}; // world radius, strength, debug, pixel cap
  glm::vec4 screen{};        // native width/height
};
static_assert(sizeof(GtaoPush) == 112);

// Immutable descriptors, single-queue targets. Caller owns graph submission
// transactions and drains all users before replacing this extent.
class Gtao {
public:
  enum Target : unsigned { Diffuse, Raw, Filtered, Composite, Count };
  Gtao(Device const &, vk::Extent2D, vk::ImageView hdr, vk::ImageView depth);
  struct Frame {
    RenderGraph::ImageId images[Count];
    RenderGraph::PassId horizon, filter, composite;
  };
  RenderGraph::ImageId importDiffuse(RenderGraph &) const;
  Frame addPasses(RenderGraph &, RenderGraph::ImageId hdr,
                  RenderGraph::ImageId depth,
                  RenderGraph::ImageId diffuse) const;
  void draw(vk::CommandBuffer, unsigned pass, GtaoPush const &) const;
  void submitted(RenderGraph::Plan const &, RenderGraph::ImageId diffuse,
                 Frame const *);
  vk::Image image(Target i) const { return *targets_[i].image.image; }
  vk::ImageView view(Target i) const { return *targets_[i].view; }
  RenderGraph::State const &state(Target i) const { return states_[i]; }
  static vk::Format format(Target i) {
    return i == Raw || i == Filtered ? vk::Format::eR16Sfloat
                                     : vk::Format::eR16G16B16A16Sfloat;
  }

private:
  struct Image {
    GpuImage image;
    vk::raii::ImageView view = nullptr;
  };
  Device const &device_;
  vk::Extent2D extent_;
  ResourceLedger::Lease accounting_;
  std::array<Image, Count> targets_;
  std::array<RenderGraph::State, Count> states_;
  vk::raii::Sampler sampler_ = nullptr;
  vk::raii::DescriptorSetLayout setLayout_ = nullptr;
  vk::raii::DescriptorPool pool_ = nullptr;
  vk::DescriptorSet set_{};
  vk::raii::PipelineLayout layout_ = nullptr;
  std::array<vk::raii::Pipeline, 3> pipelines_{nullptr, nullptr, nullptr};
};
