#pragma once

#include "transform.hpp"
#include <cstdint>

using MeshId = std::uint32_t;

struct SceneObject {
  Transform transform{};
  MeshId meshId = 0;
};
