#pragma once

#include "glm_include.hpp"
#include <array>
#include <cstddef>

struct SunCascadeSettings {
  bool enabled = true;
  unsigned count = 4;
  float distance = 60;
  float splitLambda = .65f;
  float blendFraction = .1f;
  float casterDistance = 50;
  float biasConstant = .003f; // world units
  float biasSlope = 1.5f;     // world texels
  unsigned debugIndex = 0;
};

// Appended to the existing per-frame shadow/probe SSBO, std430.
struct alignas(16) SunCascadeGpu {
  std::array<glm::mat4, 4> viewProj{};
  std::array<glm::vec4, 4> rects{}, bias{};
  glm::vec4 splits{};
  glm::vec4 forwardNear{};
  glm::vec4 params{}; // count, blend fraction, debug tile, far distance
};
static_assert(sizeof(SunCascadeGpu) == 432);
static_assert(offsetof(SunCascadeGpu, splits) == 384);
inline constexpr unsigned sunCascadeResolution = 1024;

void validateSunCascades(SunCascadeSettings const &);
// Unsupported camera projections return count=0 for the legacy single map.
// Settings errors throw before acquiring a frame; no history is required.
SunCascadeGpu buildSunCascades(glm::mat4 const &viewProj, glm::vec3 camera,
                               glm::vec3 towardsSun, SunCascadeSettings const &,
                               float pcfRadius);
