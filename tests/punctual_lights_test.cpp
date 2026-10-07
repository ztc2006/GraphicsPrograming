#include "punctual_lights.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, char const *message) {
  if (!value)
    throw std::runtime_error(message);
}
void close(float a, float b) {
  require(std::abs(a - b) < 1e-6f, "Photometric attenuation mismatch");
}
template <class F> void reject(F f) {
  bool failed = false;
  try {
    f();
  } catch (std::runtime_error const &) {
    failed = true;
  }
  require(failed, "Invalid light snapshot was accepted");
}
} // namespace
int main() {
  try {
    close(punctualRangeAttenuation(1, 0), 1);
    close(punctualRangeAttenuation(2, 0), .25f);
    close(punctualRangeAttenuation(2, 4), .234375f);
    close(punctualRangeAttenuation(4, 4), 0);
    close(punctualRangeAttenuation(5, 4), 0);
    close(punctualRangeAttenuation(0, 0), 0);
    close(punctualSpotAttenuation(1, .9f, .5f), 1);
    close(punctualSpotAttenuation(.5f, .9f, .5f), 0);
    close(punctualSpotAttenuation(.7f, .9f, .5f), .25f);
    close(punctualSpotAttenuation(1, 1, 1), 1);
    close(punctualSpotAttenuation(.999f, 1, 1), 0);
    std::vector<PunctualLight> lights(65);
    for (auto &l : lights) {
      l.type = PunctualLightType::Spot;
      l.direction = {0, 0, -4};
      l.intensity = 16;
      l.range = 8;
    }
    auto packed = packPunctualLights(lights, 16 + 128 * 64);
    require(packed.counts == std::array<std::uint32_t, 4>{65, 0, 0, 0} &&
                packed.lights.size() == 65 &&
                packed.lights[0].directionType == glm::vec4(0, 0, -1, 2) &&
                packed.lights[0].positionRange.w == 8 &&
                packed.lights[0].colorIntensity.w == 16,
            "Light ABI/count/direction normalization changed units");
    require(punctualCapacity(64, 65, 16 + 128 * 64) == 128 &&
                punctualCapacity(64, 65, 16 + 65 * 64) == 65,
            "Light capacity growth truncated or exceeded device range");
    reject([&] { (void)packPunctualLights(lights, 16 + 64 * 64); });
    lights.back().enabled = false;
    require(packPunctualLights(lights, 16 + 64 * 64).counts[0] == 64,
            "Disabled light consumes shader work");
    lights.back().intensity =
        -1; // Validate disabled input too; enabling it later must be safe.
    reject([&] { (void)packPunctualLights(lights, 16 + 128 * 64); });
    for (unsigned i = 0; i < 7; ++i) {
      PunctualLight bad;
      bad.type = PunctualLightType::Spot;
      switch (i) {
      case 0:
        bad.color.x = 1.1f;
        break;
      case 1:
        bad.intensity = std::numeric_limits<float>::infinity();
        break;
      case 2:
        bad.range = -1;
        break;
      case 3:
        bad.direction = {0, 0, 0};
        break;
      case 4:
        bad.innerCone = bad.outerCone;
        break;
      case 5:
        bad.outerCone = 2;
        break;
      case 6:
        bad.position.x = std::numeric_limits<float>::quiet_NaN();
        break;
      }
      reject([&] { validatePunctualLight(bad); });
    }
    reject([] { (void)punctualCapacity(0, 1, 16); });
    require(packPunctualLights({}, 4112).counts[0] == 0,
            "Empty snapshot not zeroed");
    std::cout << "PASS photometric attenuation, cone boundaries, 65 lights, "
                 "ABI/capacity and whole-snapshot validation\n";
  } catch (std::exception const &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
