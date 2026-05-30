#pragma once

#include "mesh.hpp"
#include "transform.hpp"
#include <cstdint>

using MeshId = std::uint32_t;
using MaterialId = std::uint32_t;

struct SceneObject {
  Transform transform{};
  MeshId meshId = 0;
  MaterialId materialId = 0;
  Aabb worldBounds{};
};
