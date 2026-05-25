#pragma once

#include "camera.hpp"

struct GLFWwindow;

class OrbitCameraController {
public:
  void attach(Camera const &camera);
  void update(Camera &camera) const;

  void beginRotate(GLFWwindow *window, double cursorX, double cursorY);
  void rotate(double cursorX, double cursorY);
  void endRotate(GLFWwindow *window);
  void zoom(double yOffset);

private:
  static constexpr float kMinPitch = -1.45f;
  static constexpr float kMaxPitch = 1.45f;

  float yawRadians_ = 0.0f;
  float pitchRadians_ = 0.0f;
  float distance_ = 2.0f;
  float rotateSensitivity_ = 0.005f;
  float zoomSensitivity_ = 0.15f;
  float minDistance_ = 0.35f;
  float maxDistance_ = 10.0f;
  glm::vec3 target_{0.0f};
  double lastCursorX_ = 0.0;
  double lastCursorY_ = 0.0;
  bool rotating_ = false;
};
