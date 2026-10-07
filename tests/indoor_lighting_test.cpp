#include "indoor_lighting.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool b, char const *message) {
  if (!b)
    throw std::runtime_error(message);
}
int main() {
  try {
    PunctualLight spot;
    spot.type = PunctualLightType::Spot;
    spot.position = {2, 3, 4};
    spot.direction = {0, -1, 0};
    spot.castsShadow = true;
    spot.range = 8;
    auto point = spot;
    point.type = PunctualLightType::Point;
    auto off = spot;
    off.enabled = false;
    std::vector<PunctualLight> lights{off, point, spot, spot, spot, spot, spot};
    auto packed = packPunctualLights(lights, 65536);
    IndoorLightingGpu g;
    assignSpotShadows(lights, packed, g, true);
    require(g.counts.x == 4 && g.counts.y == 6 && packed.lights.size() == 6,
            "Shadow budget lost a light or counted disabled lights");
    require(packed.lights[0].cones.z == -1 &&
                packed.lights.back().cones.z == -1,
            "Unsupported/budget-overflow light acquired an invalid shadow");
    for (unsigned i = 0; i < 4; ++i) {
      require(packed.lights[i + 1].cones.z == float(i),
              "Shadow index mismatches packed source order");
      auto near =
          g.spotViewProj[i] *
          glm::vec4(spot.position + spot.direction * spot.shadowNear, 1);
      auto far = g.spotViewProj[i] *
                 glm::vec4(spot.position + spot.direction * spot.range, 1);
      require(std::abs(near.z / near.w) < .0001f &&
                  std::abs(far.z / far.w - 1) < .0001f,
              "Shadow projection has wrong Vulkan depth interval");
      require(std::abs(near.x / near.w) < .0001f &&
                  std::abs(near.y / near.w) < .0001f,
              "Vertical spot projection points away from the emission axis");
    }
    assignSpotShadows(lights, packed, g, false);
    require(g.counts.x == 0 && g.counts.y == 6,
            "Global shadow disable changed light snapshot");
    for (auto const &l : packed.lights)
      require(l.cones.z == -1, "Disabled atlas retained stale indices");
    spot.outerCone = glm::half_pi<float>();
    lights = {spot};
    packed = packPunctualLights(lights, 65536);
    assignSpotShadows(lights, packed, g, true);
    require(g.counts.x == 0 && g.counts.y == 1,
            "180 degree perspective should be explicitly unassigned");
    LocalProbeSettings p;
    validateLocalProbe(p);
    p.position = p.maximum;
    bool failed = false;
    try {
      validateLocalProbe(p);
    } catch (...) {
      failed = true;
    }
    require(failed, "Probe position on the wall accepted");
    // Known directional radiance makes every axis/orientation independently
    // observable after cube conversion, rather than testing a constant image.
    unsigned size = 32;
    std::vector<float> cube(size * size * 6 * 4);
    for (unsigned f = 0; f < 6; ++f)
      for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x) {
          auto d = environmentCubeDirection(f, 2 * (x + .5f) / size - 1,
                                            2 * (y + .5f) / size - 1);
          auto i = (f * size * size + y * size + x) * 4;
          for (unsigned c = 0; c < 3; ++c)
            cube[i + c] = d[c] * .5f + .5f;
          cube[i + 3] = 1;
        }
    auto e = environmentCubeToEquirectangular(cube, size);
    for (unsigned y = 0; y < e.height; ++y)
      for (unsigned x = 0; x < e.width; ++x) {
        float lon =
            (x + .5f) / e.width * glm::two_pi<float>() - glm::pi<float>();
        float lat =
            (y + .5f) / e.height * glm::pi<float>() - glm::half_pi<float>();
        glm::vec3 d{std::cos(lat) * std::cos(lon), std::sin(lat),
                    std::cos(lat) * std::sin(lon)};
        for (unsigned c = 0; c < 3; ++c)
          require(std::abs(e.rgba[(y * e.width + x) * 4 + c] -
                           (d[c] * .5f + .5f)) < .004f,
                  "Captured cube conversion flipped or permuted an axis");
      }
    std::cout << "Indoor shadow budget, projection, probe bounds and cube "
                 "orientation passed\n";
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
