#include "hdr_ibl.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {
constexpr float kPi = std::numbers::pi_v<float>;

std::array<float, 9> evaluateShBasis(glm::vec3 const &direction) {
  return {
      0.28209479f,
      0.48860251f * direction.y,
      0.48860251f * direction.z,
      0.48860251f * direction.x,
      1.09254843f * direction.x * direction.y,
      1.09254843f * direction.y * direction.z,
      0.31539156f * (3.0f * direction.z * direction.z - 1.0f),
      1.09254843f * direction.x * direction.z,
      0.54627421f * (direction.x * direction.x - direction.y * direction.y),
  };
}
} // namespace

EnvironmentSh projectEquirectangularToSh(HdrImage const &image) {
  std::size_t const expectedValues =
      static_cast<std::size_t>(image.width) * image.height * 4;
  if (image.width == 0 || image.height == 0 ||
      image.rgba.size() != expectedValues) {
    throw std::runtime_error("Cannot project invalid HDR image to SH.");
  }

  EnvironmentSh coefficients{};
  float totalWeight = 0.0f;
  float const longitudeStep = 2.0f * kPi / static_cast<float>(image.width);
  float const latitudeStep = kPi / static_cast<float>(image.height);

  for (std::uint32_t y = 0; y < image.height; ++y) {
    float const v =
        (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height);
    float const latitude = (v - 0.5f) * kPi;
    float const cosLatitude = std::cos(latitude);
    float const rowWeight = cosLatitude * longitudeStep * latitudeStep;

    for (std::uint32_t x = 0; x < image.width; ++x) {
      float const u =
          (static_cast<float>(x) + 0.5f) / static_cast<float>(image.width);
      float const longitude = (u - 0.5f) * 2.0f * kPi;
      glm::vec3 const direction{
          cosLatitude * std::cos(longitude),
          std::sin(latitude),
          cosLatitude * std::sin(longitude),
      };
      auto const basis = evaluateShBasis(direction);
      std::size_t const pixel =
          (static_cast<std::size_t>(y) * image.width + x) * 4;
      glm::vec3 const radiance{image.rgba[pixel], image.rgba[pixel + 1],
                               image.rgba[pixel + 2]};
      for (std::size_t coefficient = 0; coefficient < coefficients.size();
           ++coefficient) {
        coefficients[coefficient] += radiance * basis[coefficient] * rowWeight;
      }
      totalWeight += rowWeight;
    }
  }

  float const normalization =
      totalWeight > 0.0f ? 4.0f * kPi / totalWeight : 1.0f;
  for (glm::vec3 &coefficient : coefficients) {
    coefficient *= normalization;
  }
  return coefficients;
}

glm::vec3 evaluateIrradianceSh(EnvironmentSh const &coefficients,
                               glm::vec3 direction) {
  float const lengthSquared = glm::dot(direction, direction);
  if (lengthSquared <= 0.000001f) {
    return glm::vec3{0.0f};
  }
  direction *= glm::inversesqrt(lengthSquared);
  auto const basis = evaluateShBasis(direction);

  glm::vec3 irradiance = coefficients[0] * basis[0] * kPi;
  for (std::size_t coefficient = 1; coefficient <= 3; ++coefficient) {
    irradiance +=
        coefficients[coefficient] * basis[coefficient] * (2.0f * kPi / 3.0f);
  }
  for (std::size_t coefficient = 4; coefficient < coefficients.size();
       ++coefficient) {
    irradiance += coefficients[coefficient] * basis[coefficient] * (kPi / 4.0f);
  }
  return glm::max(irradiance, glm::vec3{0.0f});
}
