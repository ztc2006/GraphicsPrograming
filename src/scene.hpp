#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "camera.hpp"
#include "glm_include.hpp"
#include "mesh.hpp"
#include "scene_object.hpp"

using MeshId = std::uint32_t;
using MaterialId = std::uint32_t;

struct Material {
  std::string albedoPath;
  glm::vec4 tint{1.0f};
};

struct LightingSettings {
  glm::vec3 direction{-0.4f, 1.0f, 0.3f};
  float intensity = 1.0f;
  glm::vec3 color{1.0f, 0.98f, 0.92f};
  float ambientStrength = 0.08f;
  float diffuseStrength = 1.0f;
  float specularStrength = 0.35f;
  float shininess = 32.0f;
  float shadowBiasSlope = 0.0025f;
  float shadowBiasConstant = 0.0007f;
};

struct Scene {
  std::vector<Mesh> meshes;
  std::vector<Material> materials;
  std::vector<SceneObject> objects;
  std::vector<Camera> cameras;
  std::size_t activeCameraIndex = 0;
  LightingSettings lighting{};
};
