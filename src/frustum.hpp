#pragma once

#include <array>
#include "mesh.hpp"

// Vulkan clip depth is [0,w]. Bounds and planes are both in world space.
struct Frustum {
  std::array<glm::dvec4, 6> planes{};
};
Frustum extractFrustum(glm::mat4 const &, glm::vec2 guardNdc = glm::vec2{0.0f});
// Missing/invalid bounds or planes stay visible. Boundary contact is inclusive.
bool intersectsFrustum(Frustum const &, Aabb const &);
bool intersectsDepthRange(Aabb const &, glm::vec3 origin, glm::vec3 direction,
                          float minimum, float maximum);
