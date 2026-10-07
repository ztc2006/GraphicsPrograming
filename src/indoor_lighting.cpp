#include "indoor_lighting.hpp"
#include <cmath>
#include <stdexcept>

void validateLocalProbe(LocalProbeSettings const &p) {
  for (unsigned i = 0; i < 3; ++i)
    if (!std::isfinite(p.minimum[i]) || !std::isfinite(p.maximum[i]) ||
        !std::isfinite(p.position[i]) || p.maximum[i] - p.minimum[i] < .001f ||
        p.position[i] <= p.minimum[i] || p.position[i] >= p.maximum[i])
      throw std::runtime_error(
          "Probe requires finite bounds and a position strictly inside");
}
void assignSpotShadows(std::span<PunctualLight const> lights,
                       PackedPunctualLights &packed, IndoorLightingGpu &g,
                       bool enabled) {
  g.counts.x = g.counts.y = 0;
  std::size_t index = 0;
  for (auto const &light : lights) {
    if (!light.enabled)
      continue;
    auto &gpu = packed.lights.at(index++);
    gpu.cones.z = -1;
    if (!light.castsShadow)
      continue;
    ++g.counts.y;
    // Unsupported kinds/wide or narrow cones stay lit and are reported as
    // unassigned; no light is truncated and no singular projection is formed.
    if (!enabled || light.type != PunctualLightType::Spot ||
        light.outerCone < .001f || light.outerCone >= glm::radians(89.5f) ||
        g.counts.x == spotShadowBudget)
      continue;
    auto d = glm::normalize(light.direction);
    glm::vec3 up =
        std::abs(d.y) > .95f ? glm::vec3{0, 0, 1} : glm::vec3{0, 1, 0};
    float far = light.range > 0 ? light.range : light.shadowDistance;
    if (far <= light.shadowNear + .001f)
      continue;
    unsigned slot = g.counts.x++;
    auto projection =
        glm::perspective(2 * light.outerCone, 1.f, light.shadowNear, far);
    projection[1][1] *= -1;
    g.spotViewProj[slot] =
        projection * glm::lookAt(light.position, light.position + d, up);
    g.spotRects[slot] = {.5f + .25f * (slot % 2), .5f * (slot / 2), .25f, .5f};
    g.spotBias[slot] = {light.shadowBiasSlope, light.shadowBiasConstant,
                        light.shadowPcfRadius, 0};
    gpu.cones.z = float(slot);
  }
}
