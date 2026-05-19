#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "camera.hpp"
#include "mesh.hpp"
#include "scene_object.hpp"

using MeshId = std::uint32_t;

struct Scene {
  std::vector<Mesh> meshes;
  std::vector<SceneObject> objects;
  std::vector<Camera> cameras;
  std::size_t activeCameraIndex = 0;
};
