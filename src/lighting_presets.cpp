#include "lighting_presets.hpp"
void applyKitchenLightingPreset(LightingSettings &settings, Camera &camera) {
  for (auto position :
       {glm::vec3{.3f, 2.85f, 1.1f}, glm::vec3{.3f, 2.85f, -1.5f}}) {
    PunctualLight light;
    light.name = "Kitchen preview ceiling spot";
    light.type = PunctualLightType::Spot;
    light.position = position;
    light.direction = {0, -1, 0};
    light.color = {1, .88f, .72f};
    light.intensity = 70;
    light.range = 9;
    light.innerCone = glm::radians(32.f);
    light.outerCone = glm::radians(65.f);
    light.castsShadow = true;
    settings.punctualLights.push_back(light);
  }
  settings.sunEnabled = true;
  settings.localProbe = {
      true, {-2.45f, .01f, -2.44f}, {3.5f, 3.21f, 5.35f}, {.6f, 1.8f, 1.8f}};
  camera.position = {1.8f, 1.6f, 2.9f};
  camera.target = {-2.f, 1.2f, 0.f};
  camera.fovRadians = glm::radians(55.f);
  camera.nearPlane = .02f;
}
