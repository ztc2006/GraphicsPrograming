#pragma once

#include <optional>

#include "glm_include.hpp"

struct Transform {
  glm::vec3 translation{0.0f, 0.0f, 0.0f};
  glm::vec3 rotation{0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f, 1.0f, 1.0f};

  // Preserve imported hierarchy matrices, including shear and reflection.
  std::optional<glm::mat4> importedMatrix;

  glm::mat4 matrix() const {
    glm::mat4 transform{1.0f};
    transform = glm::translate(transform, translation);
    transform = glm::rotate(transform, rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
    transform = glm::rotate(transform, rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
    transform = glm::rotate(transform, rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
    transform = glm::scale(transform, scale);
    return transform * importedMatrix.value_or(glm::mat4{1.0f});
  }
};
