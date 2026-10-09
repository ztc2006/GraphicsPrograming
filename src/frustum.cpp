#include "frustum.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

Frustum extractFrustum(glm::mat4 const &matrix, glm::vec2 guardNdc) {
  auto row = [&](unsigned i) {
    return glm::dvec4{matrix[0][i], matrix[1][i], matrix[2][i], matrix[3][i]};
  };
  auto x = row(0), y = row(1), z = row(2), w = row(3);
  auto gx = w * (1.0 + std::max(0.0, double(guardNdc.x)));
  auto gy = w * (1.0 + std::max(0.0, double(guardNdc.y)));
  return {{gx + x, gx - x, gy + y, gy - y, z, w - z}};
}

bool intersectsFrustum(Frustum const &frustum, Aabb const &bounds) {
  if (!bounds.valid) return true;
  for (unsigned axis = 0; axis < 3; ++axis)
    if (!std::isfinite(bounds.min[axis]) || !std::isfinite(bounds.max[axis]) ||
        bounds.min[axis] > bounds.max[axis]) return true;
  for (auto const &plane : frustum.planes)
    for (unsigned component = 0; component < 4; ++component)
      if (!std::isfinite(plane[component])) return true;
  for (auto const &plane : frustum.planes) {
    glm::dvec3 positive, magnitude;
    for (unsigned axis = 0; axis < 3; ++axis) {
      positive[axis] = plane[axis] >= 0 ? bounds.max[axis] : bounds.min[axis];
      magnitude[axis] = std::max(std::abs(double(bounds.min[axis])),
                                 std::abs(double(bounds.max[axis])));
    }
    // Account for the float transforms used by rasterization; rounding must
    // create extra candidates rather than clipped silhouettes/shadows.
    double error = 8 * std::numeric_limits<float>::epsilon() *
        (glm::dot(glm::abs(glm::dvec3{plane}), magnitude) + std::abs(plane.w) + 1);
    if (glm::dot(glm::dvec3{plane}, positive) + plane.w < -error) return false;
  }
  return true;
}

bool intersectsDepthRange(Aabb const &bounds, glm::vec3 origin, glm::vec3 direction,
                          float minimum, float maximum) {
  if (!bounds.valid || !std::isfinite(minimum) || !std::isfinite(maximum) ||
      minimum > maximum) return true;
  glm::dvec3 low, high, magnitude;
  for (unsigned axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(bounds.min[axis]) || !std::isfinite(bounds.max[axis]) ||
        !std::isfinite(origin[axis]) || !std::isfinite(direction[axis]) ||
        bounds.min[axis] > bounds.max[axis]) return true;
    low[axis] = direction[axis] >= 0 ? bounds.min[axis] : bounds.max[axis];
    high[axis] = direction[axis] >= 0 ? bounds.max[axis] : bounds.min[axis];
    magnitude[axis] = std::max(std::abs(double(bounds.min[axis])),
                               std::abs(double(bounds.max[axis]))) + std::abs(double(origin[axis]));
  }
  if (glm::dot(direction,direction)==0) return true;
  auto forward=glm::dvec3{direction}, camera=glm::dvec3{origin};
  double error=8*std::numeric_limits<float>::epsilon() *
      (glm::dot(glm::abs(forward),magnitude)+std::abs(double(minimum))+std::abs(double(maximum))+1);
  return glm::dot(forward,high-camera)>=minimum-error &&
         glm::dot(forward,low-camera)<=maximum+error;
}
