#include "punctual_lights.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
namespace {
[[noreturn]] void invalid(char const *reason) {
  throw std::runtime_error(std::string("Punctual light: ") + reason);
}
bool finite(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
std::size_t maxCount(std::size_t bytes) {
  if (bytes < punctualHeaderBytes + sizeof(GpuPunctualLight))
    invalid("storage buffer limit is too small");
  return std::min((bytes - punctualHeaderBytes) / sizeof(GpuPunctualLight),
                  std::size_t(UINT32_MAX));
}
} // namespace
void validatePunctualLight(PunctualLight const &l) {
  if (!std::isfinite(l.shadowNear) || l.shadowNear < .001f ||
      !std::isfinite(l.shadowDistance) || l.shadowDistance <= l.shadowNear ||
      !std::isfinite(l.shadowBiasSlope) || l.shadowBiasSlope < 0 ||
      !std::isfinite(l.shadowBiasConstant) || l.shadowBiasConstant < 0 ||
      !std::isfinite(l.shadowPcfRadius) || l.shadowPcfRadius < 0 || l.shadowPcfRadius > 4)
    invalid("invalid shadow near/distance/bias/PCF");
  if (l.type != PunctualLightType::Directional &&
      l.type != PunctualLightType::Point && l.type != PunctualLightType::Spot)
    invalid("unknown type");
  if (!finite(l.position) || !finite(l.direction) || !finite(l.color) ||
      !std::isfinite(l.intensity) || l.intensity < 0 ||
      !std::isfinite(l.range) || l.range < 0)
    invalid("non-finite or negative position/color/intensity/range");
  if (glm::any(glm::lessThan(l.color, glm::vec3(0))) ||
      glm::any(glm::greaterThan(l.color, glm::vec3(1))))
    invalid("linear color must be in [0,1]");
  auto length = glm::length(glm::dvec3(l.direction));
  if (l.type != PunctualLightType::Point &&
      (!std::isfinite(length) || length < 1e-12))
    invalid("direction is degenerate");
  if (l.type == PunctualLightType::Spot &&
      (!std::isfinite(l.innerCone) || !std::isfinite(l.outerCone) ||
       l.innerCone < 0 || l.innerCone >= l.outerCone ||
       l.outerCone > std::numbers::pi_v<float> / 2))
    invalid("spot requires 0 <= inner < outer <= pi/2");
}
PackedPunctualLights packPunctualLights(std::span<PunctualLight const> input,
                                        std::size_t maxBytes) {
  auto limit = maxCount(maxBytes);
  PackedPunctualLights result;
  for (auto const &l : input) {
    validatePunctualLight(l);
    if (!l.enabled)
      continue;
    if (result.lights.size() == limit)
      invalid("active count exceeds maxStorageBufferRange; snapshot rejected, "
              "no lights truncated");
    auto direction = l.type == PunctualLightType::Point
                         ? glm::vec3(0, 0, -1)
                         : glm::vec3(glm::normalize(glm::dvec3(l.direction)));
    result.lights.push_back(
        {glm::vec4(l.position,
                   l.type == PunctualLightType::Directional ? 0 : l.range),
         glm::vec4(direction, float(l.type)), glm::vec4(l.color, l.intensity),
         l.type == PunctualLightType::Spot
             ? glm::vec4(std::cos(l.innerCone), std::cos(l.outerCone), -1, 0)
             : glm::vec4(1, 1, -1, 0)});
  }
  result.counts[0] = std::uint32_t(result.lights.size());
  return result;
}
std::size_t punctualCapacity(std::size_t current, std::size_t count,
                             std::size_t maxBytes) {
  auto maximum = maxCount(maxBytes);
  if (count > maximum)
    invalid("active count exceeds storage capacity; no lights truncated");
  auto capacity = std::min(maximum, std::max(current, initialPunctualCapacity));
  while (capacity < count)
    capacity = capacity > maximum / 2 ? maximum : capacity * 2;
  return capacity;
}
float punctualRangeAttenuation(float distance, float range) {
  if (!std::isfinite(distance) || distance < 0 || !std::isfinite(range) ||
      range < 0)
    invalid("invalid attenuation arguments");
  if (distance < .0001f)
    return 0;
  double fade =
      range > 0 ? std::clamp(1 - std::pow(double(distance) / range, 4), 0., 1.)
                : 1;
  return float(fade / (double(distance) * distance));
}
float punctualSpotAttenuation(float cosine, float innerCos, float outerCos) {
  if (innerCos <= outerCos)
    return cosine >= innerCos ? 1 : 0;
  float a = std::clamp((cosine - outerCos) / (innerCos - outerCos), 0.f, 1.f);
  return a * a;
}
