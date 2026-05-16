#pragma once

#include "mesh.hpp"
#include "transform.hpp"

struct SceneObject {
  Transform transform{};
  Mesh const *mesh = nullptr;
};
