#pragma once
#include "glm_include.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

enum class PunctualLightType : std::uint32_t {
  Directional = 0,
  Point = 1,
  Spot = 2
};
struct PunctualLight {
  std::string name;
  PunctualLightType type = PunctualLightType::Point;
  glm::vec3 position{0}, direction{0, 0, -1}, color{1};
  float intensity = 1; // cd for point/spot, lux for directional.
  float range = 0;     // 0 = infinite; no scaling by node transform.
  float innerCone = 0, outerCone = .7853981633974483f;
  bool enabled = true;
  bool castsShadow = false;
  float shadowNear = .05f, shadowDistance = 20;
  float shadowBiasSlope = .001f, shadowBiasConstant = .0002f, shadowPcfRadius = 1;
};
struct alignas(16) GpuPunctualLight {
  glm::vec4 positionRange, directionType, colorIntensity, cones;
};
static_assert(sizeof(GpuPunctualLight) == 64);
static_assert(offsetof(GpuPunctualLight, positionRange) == 0 &&
              offsetof(GpuPunctualLight, directionType) == 16 &&
              offsetof(GpuPunctualLight, colorIntensity) == 32 &&
              offsetof(GpuPunctualLight, cones) == 48);
struct PackedPunctualLights {
  std::array<std::uint32_t, 4> counts{};
  std::vector<GpuPunctualLight> lights;
};
inline constexpr std::size_t punctualHeaderBytes = 16,
                             initialPunctualCapacity = 64;
void validatePunctualLight(PunctualLight const &light);
PackedPunctualLights packPunctualLights(std::span<PunctualLight const> lights,
                                        std::size_t maxBufferBytes);
std::size_t punctualCapacity(std::size_t current, std::size_t count,
                             std::size_t maxBufferBytes);
// Numerical CPU counterpart used for controlled lighting/authoring; the GPU
// independently evaluates the same documented physical approximations.
float punctualRangeAttenuation(float distance, float range);
float punctualSpotAttenuation(float cosine, float innerCos, float outerCos);
