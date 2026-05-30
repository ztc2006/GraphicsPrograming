#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "mesh.hpp"
#include "scene.hpp"
#include "scene_object.hpp"

struct LoadedGltfScene {
  std::vector<Mesh> meshes;
  std::vector<Material> materials;
  std::vector<SceneObject> objects;
};

LoadedGltfScene loadStaticGltfScene(std::filesystem::path const &path,
                                    std::string fallbackAlbedoPath);
