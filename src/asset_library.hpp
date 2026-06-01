#pragma once

#include <vector>

#include "mesh.hpp"
#include "scene.hpp"

struct AssetLibrary {
  std::vector<Mesh> meshes;
  std::vector<Material> materials;
};
