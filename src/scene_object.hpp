#pragma once

#include "asset_ids.hpp"
#include "mesh.hpp"
#include "transform.hpp"

// Scene instance that references shared AssetLibrary mesh/material data.
struct SceneObject {
  Transform transform{};
  MeshId meshId = 0;
  MaterialId materialId = 0;
  Aabb worldBounds{};
  bool primaryVisible = true, shadowCaster = true;
};
