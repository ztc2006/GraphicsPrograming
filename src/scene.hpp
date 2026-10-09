#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "camera.hpp"
#include "dielectric.hpp"
#include "glm_include.hpp"
#include "punctual_lights.hpp"
#include "indoor_lighting.hpp"
#include "scene_object.hpp"
#include "texture_sampler.hpp"

enum class AlphaMode : std::uint32_t {
  Opaque = 0,
  Mask = 1,
  Blend = 2,
};

inline char const *alphaModeLabel(AlphaMode mode) {
  switch (mode) {
  case AlphaMode::Opaque:
    return "Opaque";
  case AlphaMode::Mask:
    return "Mask";
  case AlphaMode::Blend:
    return "Blend";
  }
  return "Opaque";
}

struct Material {
  OpticalMaterial optical;
  std::string name;
  std::string albedoPath;
  std::vector<std::byte> albedoBytes;
  std::string normalPath;
  std::vector<std::byte> normalBytes;
  std::string metallicRoughnessPath;
  std::vector<std::byte> metallicRoughnessBytes;
  std::string occlusionPath;
  std::vector<std::byte> occlusionBytes;
  std::string emissivePath;
  std::vector<std::byte> emissiveBytes;
  std::string specularPath, specularColorPath;
  std::vector<std::byte> specularBytes, specularColorBytes;
  TextureSamplerDescription specularSampler, specularColorSampler;
  int specularTexCoord = 0, specularColorTexCoord = 0;
  float specularFactor = 1.0f;
  glm::vec3 specularColorFactor{1.0f};
  std::string heightPath;
  std::string alphaPath;
  TextureSamplerDescription albedoSampler, normalSampler,
      metallicRoughnessSampler;
  TextureSamplerDescription occlusionSampler, emissiveSampler, heightSampler,
      alphaSampler;
  int albedoTexCoord = 0;
  int normalTexCoord = 0;
  int metallicRoughnessTexCoord = 0;
  int occlusionTexCoord = 0;
  int emissiveTexCoord = 0;
  glm::vec4 tint{1.0f};
  glm::vec3 emissiveFactor{0.0f};
  float metallicFactor = 0.0f;
  float roughnessFactor = 1.0f;
  float occlusionStrength = 1.0f;
  float normalScale = 1.0f;
  float parallaxScale = 0.04f;
  AlphaMode alphaMode = AlphaMode::Opaque;
  float alphaCutoff = 0.5f;
  bool doubleSided = false;
  bool sourceAreaLight =
      false; // Explicit imported light-card metadata, not any emissive.
};

struct LightingSettings {
  LocalProbeSettings localProbe;
  LocalProbeSettings
      detailReflectionProbe; // Optional small specular-only region.
  SunCascadeSettings sunCascades;
  unsigned localShadowDebugIndex = 0;
  glm::vec3 direction{-0.4f, 1.0f, 0.3f};
  float intensity = 1.0f;
  bool sunEnabled = true;
  bool clusteredLights = true;
  std::vector<PunctualLight> punctualLights;
  glm::vec3 color{1.0f, 0.98f, 0.92f};
  float ambientStrength = 0.08f;
  float diffuseStrength = 1.0f;
  float specularStrength = 1.0f;
  bool specularAaEnabled = true;
  int pbrDebugMode = 0;
  float shadowBiasSlope = 0.0025f;
  float shadowBiasConstant = 0.0007f;
  float shadowPcfRadius = 1.0f;
  int shadowDebugMode = 1;
  float shadowOrthoExtent = 3.0f;
  float shadowNearPlane = 0.1f;
  float shadowFarPlane = 12.0f;
  float shadowLightDistance = 6.0f;
  glm::vec3 shadowTarget{0.0f};
  float environmentIntensity = 1.0f;
  float exposureEv = 0.0f;
  bool toneMappingEnabled = true;
  float environmentRotation = 0.0f;
  float environmentDiffuseStrength = 1.0f;
  float environmentSpecularStrength = 1.0f;
};

struct Scene {
  std::vector<Camera> cameras;
  std::size_t activeCameraIndex = 0;
  LightingSettings lighting{};
};
