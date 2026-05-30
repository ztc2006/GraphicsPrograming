#pragma once

#include <vector>

#include "mesh.hpp"
#include "scene.hpp"
#include "scene_object.hpp"

struct LoadedScene {
  std::vector<Mesh> meshes;
  std::vector<Material> materials;
  std::vector<SceneObject> objects;
};
