#include "pch.hpp"

#include "orbit_camera_controller.hpp"

#include <algorithm>
#include <cmath>

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

void OrbitCameraController::updateFromInput(InputState const &input,
                                            float deltaSeconds) {
  if (!input.rightMouseCaptured) {
    return;
  }

  yawRadians_ -= static_cast<float>(input.cursorDeltaX) * rotateSensitivity_;
  pitchRadians_ -= static_cast<float>(input.cursorDeltaY) * rotateSensitivity_;
  pitchRadians_ = glm::clamp(pitchRadians_, kMinPitch, kMaxPitch);

  float const scale = 1.0f - static_cast<float>(input.scrollDeltaY) *
                                  zoomSensitivity_;
  moveSpeed_ = glm::clamp(moveSpeed_ * scale, 0.1f, 20.0f);

  float forwardAmount = 0.0f;
  float rightAmount = 0.0f;
  if (input.isKeyDown(GLFW_KEY_W)) {
    forwardAmount += 1.0f;
  }
  if (input.isKeyDown(GLFW_KEY_S)) {
    forwardAmount -= 1.0f;
  }
  if (input.isKeyDown(GLFW_KEY_D)) {
    rightAmount += 1.0f;
  }
  if (input.isKeyDown(GLFW_KEY_A)) {
    rightAmount -= 1.0f;
  }

  float const cosPitch = std::cos(pitchRadians_);
  glm::vec3 const forward{
      std::sin(yawRadians_) * cosPitch,
      0.0f,
      std::cos(yawRadians_) * cosPitch,
  };
  glm::vec3 const right =
      glm::normalize(glm::cross(forward, {0.0f, 1.0f, 0.0f}));
  position_ += forward * (forwardAmount * moveSpeed_ * deltaSeconds);
  position_ += right * (rightAmount * moveSpeed_ * deltaSeconds);
}
