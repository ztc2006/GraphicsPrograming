#pragma once
#include "glm_include.hpp"
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>

inline constexpr std::size_t invalidMotionIdentity =
    std::numeric_limits<std::size_t>::max();
struct MotionObject {
  std::size_t identity = invalidMotionIdentity;
  std::uint32_t mesh = 0, material = 0;
  glm::mat4 model{1};
};
struct alignas(16) MotionObjectGpu {
  glm::mat4 previousModel{1};
  glm::vec4 flags{}; // x: previous object snapshot is valid
};
static_assert(sizeof(MotionObjectGpu) == 80);
struct TemporalCamera {
  glm::mat4 currentViewProj{1}, previousViewProj{1}, rasterViewProj{1};
  glm::vec4 previousCamera{}; // w: camera history valid
  glm::vec4 jitterUv{};       // xy current, zw previous
};
struct TemporalSnapshot {
  TemporalCamera camera;
  glm::vec3 position{};
  std::uint32_t width = 0, height = 0;
  bool jitter = false;
  std::uint64_t sample = 0;
  std::unordered_map<std::size_t, MotionObject> objects;
  std::unordered_map<std::size_t, std::uint32_t> indices;
  std::vector<MotionObjectGpu> gpu{1}; // entry 0: no object history
};
glm::vec2 temporalJitterPixels(std::uint64_t sample);
glm::mat4 applyTemporalJitter(glm::mat4 const &, glm::vec2 jitterUv);
glm::mat4 temporalCullingViewProj(glm::mat4 const &, std::uint32_t width,
                                  std::uint32_t height);
// prepare is read-only. Publish only after successful queue submission, never
// at acquire, fence completion, slot recycling or offline probe capture.
class TemporalMotionHistory {
public:
  TemporalSnapshot prepare(glm::mat4 const &, glm::vec3 camera,
                           std::uint32_t width, std::uint32_t height,
                           bool jitter,
                           std::span<MotionObject const> objects) const;
  void commit(TemporalSnapshot &&) noexcept;
  void reset() noexcept {
    valid_ = false;
    nextSample_ = 0;
    previous_.objects.clear();
    previous_.indices.clear();
    previous_.gpu.clear();
  }
  std::uint64_t nextSample() const { return nextSample_; }

private:
  bool valid_ = false;
  std::uint64_t nextSample_ = 0;
  TemporalSnapshot previous_;
};
