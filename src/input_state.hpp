#pragma once

#include <array>

#include <GLFW/glfw3.h>

struct InputState {
  void clearFrameDeltas() {
    cursorDeltaX = 0.0;
    cursorDeltaY = 0.0;
    scrollDeltaX = 0.0;
    scrollDeltaY = 0.0;
  }

  void clearAll() {
    keys.fill(false);
    mouseButtons.fill(false);
    clearFrameDeltas();
    rightMouseCaptured = false;
  }

  bool isKeyDown(int key) const {
    return key >= 0 && key < static_cast<int>(keys.size()) && keys[key];
  }

  bool isMouseButtonDown(int button) const {
    return button >= 0 && button < static_cast<int>(mouseButtons.size()) &&
           mouseButtons[button];
  }

  std::array<bool, GLFW_KEY_LAST + 1> keys{};
  std::array<bool, GLFW_MOUSE_BUTTON_LAST + 1> mouseButtons{};
  double cursorX = 0.0;
  double cursorY = 0.0;
  double cursorDeltaX = 0.0;
  double cursorDeltaY = 0.0;
  double scrollDeltaX = 0.0;
  double scrollDeltaY = 0.0;
  bool cursorEntered = false;
  bool hasCursorPosition = false;
  bool rightMouseCaptured = false;
};
