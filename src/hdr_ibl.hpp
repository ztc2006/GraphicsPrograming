#pragma once

#include <array>

#include "glm_include.hpp"
#include "hdr_image.hpp"

using EnvironmentSh = std::array<glm::vec3, 9>;

EnvironmentSh projectEquirectangularToSh(HdrImage const &image);
glm::vec3 evaluateIrradianceSh(EnvironmentSh const &coefficients,
                               glm::vec3 direction);
