#pragma once

#include "vulkan_include.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Non-owning, whole-image, one graphics queue graph. Uploads, allocation,
// presentation semaphores and GPU completion belong to the caller.
class RenderGraph {
public:
  struct ImageId { std::uint32_t value; bool operator==(ImageId const &) const = default; };
  struct PassId { std::uint32_t value; bool operator==(PassId const &) const = default; };
  enum class Usage { ColorAttachment, DepthAttachment, SampledColor, SampledDepth, TransferSource, Present };
  struct State {
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    vk::PipelineStageFlags2 stages{};
    vk::AccessFlags2 access{};
    bool defined = false;
    bool operator==(State const &) const = default;
  };
  struct Image {
    std::string name;
    vk::Image image{};
    vk::ImageView view{};
    vk::Format format = vk::Format::eUndefined;
    vk::Extent2D extent{};
    vk::ImageAspectFlags aspect{};
    vk::ImageUsageFlags usage{};
    bool presentable = false;
    State initial{};
  };
  struct Use {
    ImageId image;
    Usage usage;
    vk::AttachmentLoadOp load = vk::AttachmentLoadOp::eDontCare;
    vk::AttachmentStoreOp store = vk::AttachmentStoreOp::eStore;
    // DONT_CARE + STORE defines contents only with guaranteed full coverage.
    bool fullOverwrite = false;
    vk::ClearValue clear{};
  };
  enum class Event { Begin, Draw, End };
  struct Pass {
    PassId id;
    std::string name;
    std::vector<Use> uses;
    std::vector<PassId> dependencies;
    std::vector<vk::ImageMemoryBarrier2> barriers;
  };
  class Plan {
    friend class RenderGraph;
  public:
    // Once per plan. Barriers and rendering boundaries surround the callback.
    // No external states are changed, even when the callback throws.
    void record(vk::CommandBuffer command,
                std::function<void(Pass const &, Event)> const &callback);
    State const &finalState(ImageId id) const;
    State const &recordedState(ImageId id) const;
    std::vector<Pass> const &passes() const { return passes_; }
    std::vector<vk::ImageMemoryBarrier2> const &exportBarriers() const { return exports_; }
    std::string dump() const;
  private:
    std::vector<Image> images_;
    std::vector<Pass> passes_;
    std::vector<vk::ImageMemoryBarrier2> exports_;
    std::vector<State> final_, recorded_;
    bool started_ = false;
  };
  // IDs are local to this graph. Duplicate underlying images are rejected.
  ImageId importImage(Image image);
  PassId addPass(std::string name, std::vector<Use> uses);
  void dependsOn(PassId pass, PassId predecessor);
  void exportImage(ImageId image, Usage usage);
  // Pure compile: infer RAW/WAR/WAW edges in declaration order, stable sort,
  // reject cycles, invalid use and undefined reads, then plan exact barriers.
  Plan compile() const;
private:
  std::vector<Image> images_;
  std::vector<Pass> passes_;
  std::vector<std::pair<ImageId, Usage>> exports_;
};
