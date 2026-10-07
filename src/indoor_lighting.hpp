#pragma once
#include "hdr_ibl.hpp"
#include "punctual_lights.hpp"
#include "sun_cascades.hpp"

struct LocalProbeSettings {
  bool enabled = false;
  glm::vec3 minimum{-1}, maximum{1}, position{0};
};
// std430, sun cascades + four spot tiles + one room probe, vertex/fragment.
struct alignas(16) IndoorLightingGpu {
  std::array<glm::mat4, 4> spotViewProj{};
  std::array<glm::vec4, 4> spotRects{}, spotBias{};
  glm::vec4 sunRect{0, 0, .5f, 1};
  glm::uvec4 counts{0}; // assigned, requested, debug slot, flags: valid/capture
  glm::vec4 probeMin{}, probeMax{}, probePosition{};
  std::array<glm::vec4, 9> probeSh{};
  SunCascadeGpu sun;
};
static_assert(sizeof(IndoorLightingGpu) == 1040);
static_assert(offsetof(IndoorLightingGpu, counts) == 400 &&
              offsetof(IndoorLightingGpu, probeSh) == 464);
static_assert(offsetof(IndoorLightingGpu, sun) == 608);
inline constexpr unsigned spotShadowBudget = 4;
void assignSpotShadows(std::span<PunctualLight const>, PackedPunctualLights &,
                       IndoorLightingGpu &, bool enabled);
void validateLocalProbe(LocalProbeSettings const &);
