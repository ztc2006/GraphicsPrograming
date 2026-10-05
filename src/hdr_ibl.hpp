#pragma once

#include <array>
#include <vector>

#include "glm_include.hpp"
#include "hdr_image.hpp"

using EnvironmentSh = std::array<glm::vec3, 9>;

EnvironmentSh projectEquirectangularToSh(HdrImage const &image);
glm::vec3 evaluateIrradianceSh(EnvironmentSh const &coefficients,
                               glm::vec3 direction);

// Perceptual roughness r, GGX alpha=r*r. All values are scene-linear radiance.
struct EnvironmentBakeSettings {
  std::uint32_t faceSize = 128;
  std::uint32_t prefilterSamples = 512;
  std::uint32_t lutSize = 128;
  std::uint32_t lutSamples = 1024;
  bool operator==(EnvironmentBakeSettings const &) const = default;
};
inline constexpr std::uint32_t environmentBakeVersion = 1;

struct EnvironmentCubeLevel {
  std::uint32_t size = 0;
  std::size_t offset =
      0; // Float values, mip-major then six tightly packed faces.
};
struct BakedEnvironment {
  EnvironmentSh sh{};
  std::vector<EnvironmentCubeLevel> levels;
  std::vector<float> cubeRgba;
  HdrImage brdfLut; // RG=A/B; BA=0/1. x=NoV, y=perceptual roughness.
};

void validateEnvironmentImage(HdrImage const &);
void validateEnvironmentBakeSettings(EnvironmentBakeSettings const &);
glm::vec3 environmentCubeDirection(unsigned face, float s, float t);
glm::vec2 integrateEnvironmentBrdf(float noV, float roughness,
                                   std::uint32_t samples);
BakedEnvironment bakeEnvironment(HdrImage const &,
                                 EnvironmentBakeSettings const & = {});
