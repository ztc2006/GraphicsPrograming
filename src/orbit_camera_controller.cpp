#include "pch.hpp"

#include "orbit_camera_controller.hpp"

#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>

void OrbitCameraController::attach(Camera const &camera) {
  position_ = camera.position;
  glm::vec3 const direction = glm::normalize(camera.target - camera.position);
  pitchRadians_ = std::asin(glm::clamp(direction.y, -1.0f, 1.0f));
  yawRadians_ = std::atan2(direction.x, direction.z);
  pitchRadians_ = glm::clamp(pitchRadians_, kMinPitch, kMaxPitch);
}

void OrbitCameraController::setMoveSpeed(float speed) {
  moveSpeed_ = glm::clamp(speed, 0.1f, 20.0f);
}

void OrbitCameraController::setRotateSensitivity(float sensitivity) {
  rotateSensitivity_ = glm::clamp(sensitivity, 0.0005f, 0.05f);
}

void OrbitCameraController::update(Camera &camera) const {
  float const cosPitch = std::cos(pitchRadians_);
  glm::vec3 const forward{
      std::sin(yawRadians_) * cosPitch,
      std::sin(pitchRadians_),
      std::cos(yawRadians_) * cosPitch,
  };
  glm::vec3 const right = glm::normalize(glm::cross(forward, {0.0f, 1.0f, 0.0f}));
  glm::vec3 const up = glm::normalize(glm::cross(right, forward));

  camera.position = position_;
  camera.target = position_ + forward;
  camera.up = up;
}

void OrbitCameraController::move(float forwardAmount, float rightAmount,
                                 float deltaSeconds) {
  float const cosPitch = std::cos(pitchRadians_);
  glm::vec3 const forward{
      std::sin(yawRadians_) * cosPitch,
      0.0f,
      std::cos(yawRadians_) * cosPitch,
  };
  glm::vec3 const right = glm::normalize(glm::cross(forward, {0.0f, 1.0f, 0.0f}));
  position_ += forward * (forwardAmount * moveSpeed_ * deltaSeconds);
  position_ += right * (rightAmount * moveSpeed_ * deltaSeconds);
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
  pitchRadians_ -= static_cast<float>(deltaY) * rotateSensitivity_;
  pitchRadians_ = glm::clamp(pitchRadians_, kMinPitch, kMaxPitch);
}

void OrbitCameraController::endRotate(GLFWwindow *window) {
  rotating_ = false;
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

void OrbitCameraController::zoom(double yOffset) {
  float const scale = 1.0f - static_cast<float>(yOffset) * zoomSensitivity_;
  moveSpeed_ = glm::clamp(moveSpeed_ * scale, 0.1f, 20.0f);
}
