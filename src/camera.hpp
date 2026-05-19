#pragma once

#include "glm_include.hpp"

struct Camera {
  glm::vec3 position{0.0f, 0.0f, 2.0f};
  glm::vec3 target{0.0f, 0.0f, 0.0f};
  glm::vec3 up{0.0f, 1.0f, 0.0f};
  float fovRadians = glm::radians(45.0f);
  float nearPlane = 0.1f;
  float farPlane = 10.0f;

  glm::mat4 viewMatrix() const { return glm::lookAt(position, target, up); }

  glm::mat4 projectionMatrix(float aspect) const {
    glm::mat4 proj = glm::perspective(fovRadians, aspect, nearPlane, farPlane);
    proj[1][1] *= -1.0f;
    return proj;
  }

  glm::mat4 viewProj(float aspect) const {
    return projectionMatrix(aspect) * viewMatrix();
  }
};
