#include <array>
#include <cmath>
#include <iostream>
#include <numbers>

#include "hdr_ibl.hpp"

namespace {
HdrImage makeConstantEnvironment(std::uint32_t width, std::uint32_t height,
                                 glm::vec3 radiance) {
  HdrImage image{
      .width = width,
      .height = height,
      .rgba = std::vector<float>(static_cast<std::size_t>(width) * height * 4),
  };
  for (std::size_t pixel = 0; pixel < image.rgba.size(); pixel += 4) {
    image.rgba[pixel] = radiance.r;
    image.rgba[pixel + 1] = radiance.g;
    image.rgba[pixel + 2] = radiance.b;
    image.rgba[pixel + 3] = 1.0f;
  }
  return image;
}

bool nearlyEqual(glm::vec3 actual, glm::vec3 expected, float tolerance) {
  return glm::all(
      glm::lessThanEqual(glm::abs(actual - expected), glm::vec3{tolerance}));
}
} // namespace

int main() {
  glm::vec3 const radiance{2.0f, 1.0f, 0.5f};
  EnvironmentSh const coefficients =
      projectEquirectangularToSh(makeConstantEnvironment(128, 64, radiance));
  glm::vec3 const expected = radiance * std::numbers::pi_v<float>;

  std::array directions = {
      glm::vec3{1.0f, 0.0f, 0.0f},
      glm::vec3{0.0f, 1.0f, 0.0f},
      glm::vec3{0.0f, 0.0f, 1.0f},
      glm::normalize(glm::vec3{1.0f, 2.0f, 3.0f}),
  };
  for (glm::vec3 const direction : directions) {
    glm::vec3 const actual = evaluateIrradianceSh(coefficients, direction);
    if (!nearlyEqual(actual, expected, 0.015f)) {
      std::cerr << "Constant-environment SH irradiance mismatch: actual=("
                << actual.r << ", " << actual.g << ", " << actual.b
                << ") expected=(" << expected.r << ", " << expected.g << ", "
                << expected.b << ")\n";
      return 1;
    }
  }

  HdrImage verticalGradient = makeConstantEnvironment(128, 64, glm::vec3{0.0f});
  for (std::uint32_t y = 0; y < verticalGradient.height; ++y) {
    float const upperHemisphereWeight =
        (static_cast<float>(y) + 0.5f) /
        static_cast<float>(verticalGradient.height);
    for (std::uint32_t x = 0; x < verticalGradient.width; ++x) {
      std::size_t const pixel =
          (static_cast<std::size_t>(y) * verticalGradient.width + x) * 4;
      verticalGradient.rgba[pixel] = upperHemisphereWeight;
    }
  }
  EnvironmentSh const verticalCoefficients =
      projectEquirectangularToSh(verticalGradient);
  float const upward =
      evaluateIrradianceSh(verticalCoefficients, {0.0f, 1.0f, 0.0f}).r;
  float const downward =
      evaluateIrradianceSh(verticalCoefficients, {0.0f, -1.0f, 0.0f}).r;
  if (upward <= downward + 0.5f) {
    std::cerr << "Equirectangular SH projection inverted the vertical axis: up="
              << upward << " down=" << downward << '\n';
    return 1;
  }
  return 0;
}
