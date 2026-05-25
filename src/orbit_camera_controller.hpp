#pragma once

#include "camera.hpp"
#include "input_state.hpp"

class OrbitCameraController {
public:
  void attach(Camera const &camera);
  void update(Camera &camera) const;

  void updateFromInput(InputState const &input, float deltaSeconds);
  void reset(Camera const &camera) { attach(camera); }

  float moveSpeed() const { return moveSpeed_; }
  void setMoveSpeed(float speed);
  float rotateSensitivity() const { return rotateSensitivity_; }
  void setRotateSensitivity(float sensitivity);

private:
  static constexpr float kMinPitch = -1.45f;
  static constexpr float kMaxPitch = 1.45f;

  float yawRadians_ = 0.0f;
  float pitchRadians_ = 0.0f;
  glm::vec3 position_{0.0f, 0.0f, 2.0f};
  float moveSpeed_ = 2.5f;
  float rotateSensitivity_ = 0.005f;
  float zoomSensitivity_ = 0.15f;
};
