#include "pch.hpp"

#include "orbit_camera_controller.hpp"

#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>

void OrbitCameraController::attach(Camera const &camera) {
  target_ = camera.target;
  glm::vec3 const offset = camera.position - camera.target;
  distance_ = glm::clamp(glm::length(offset), minDistance_, maxDistance_);

  if (distance_ <= 0.0f) {
    yawRadians_ = 0.0f;
    pitchRadians_ = 0.0f;
    distance_ = minDistance_;
    return;
  }

  glm::vec3 const direction = offset / distance_;
  pitchRadians_ = std::asin(glm::clamp(direction.y, -1.0f, 1.0f));
  yawRadians_ = std::atan2(direction.x, direction.z);
  pitchRadians_ = glm::clamp(pitchRadians_, kMinPitch, kMaxPitch);
}

void OrbitCameraController::update(Camera &camera) const {
  float const cosPitch = std::cos(pitchRadians_);
  glm::vec3 const offset{
      distance_ * std::sin(yawRadians_) * cosPitch,
      distance_ * std::sin(pitchRadians_),
      distance_ * std::cos(yawRadians_) * cosPitch,
  };

  camera.target = target_;
  camera.position = target_ + offset;
  camera.up = {0.0f, 1.0f, 0.0f};
}

void OrbitCameraController::beginRotate(GLFWwindow *window, double cursorX,
                                        double cursorY) {
  rotating_ = true;
  lastCursorX_ = cursorX;
  lastCursorY_ = cursorY;
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

void OrbitCameraController::rotate(double cursorX, double cursorY) {
  if (!rotating_) {
    return;
  }

  double const deltaX = cursorX - lastCursorX_;
  double const deltaY = cursorY - lastCursorY_;
  lastCursorX_ = cursorX;
  lastCursorY_ = cursorY;

  yawRadians_ -= static_cast<float>(deltaX) * rotateSensitivity_;
  pitchRadians_ += static_cast<float>(deltaY) * rotateSensitivity_;
  pitchRadians_ = glm::clamp(pitchRadians_, kMinPitch, kMaxPitch);
}

void OrbitCameraController::endRotate(GLFWwindow *window) {
  rotating_ = false;
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

void OrbitCameraController::zoom(double yOffset) {
  float const scale = 1.0f - static_cast<float>(yOffset) * zoomSensitivity_;
  distance_ = glm::clamp(distance_ * scale, minDistance_, maxDistance_);
}
