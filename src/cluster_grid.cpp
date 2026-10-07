#include "cluster_grid.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

std::size_t clusterListBytes(ClusterGrid const &g) {
  return std::max<std::size_t>(4, std::size_t(g.grid.w) *
                                      (clusterLightLimit + 1) * 4);
}
ClusterGrid makeClusterGrid(glm::mat4 const &vp, glm::vec3 camera,
                            std::uint32_t width, std::uint32_t height,
                            std::size_t maxBytes, std::uint32_t maxGroups,
                            bool requested) {
  ClusterGrid g;
  g.screen = {width, height, 0, clusterLightLimit};
  if (!requested || !width || !height)
    return g;
  for (unsigned c = 0; c < 4; ++c)
    for (unsigned r = 0; r < 4; ++r)
      if (!std::isfinite(vp[c][r]))
        return g;
  for (unsigned c = 0; c < 3; ++c)
    if (!std::isfinite(camera[c]))
      return g;
  auto eye = vp * glm::vec4(camera, 1);
  // The eye maps to clip.w=0 in a perspective projection. Orthographic and
  // inconsistent camera snapshots use the all-light correctness path.
  if (std::abs(eye.w) > 1e-4f || std::abs(eye.x) > 1e-4f ||
      std::abs(eye.y) > 1e-4f)
    return g;
  g.inverseViewProj = glm::inverse(vp);
  for (unsigned c = 0; c < 4; ++c)
    for (unsigned r = 0; r < 4; ++r)
      if (!std::isfinite(g.inverseViewProj[c][r]))
        return ClusterGrid{};
  auto a = g.inverseViewProj * glm::vec4(0, 0, 0, 1);
  auto b = g.inverseViewProj * glm::vec4(0, 0, 1, 1);
  if (std::abs(a.w) < 1e-12f || std::abs(b.w) < 1e-12f)
    return g;
  glm::vec3 nearPoint = glm::vec3(a) / a.w, farPoint = glm::vec3(b) / b.w;
  float near = glm::length(nearPoint - camera);
  glm::vec3 forward = near > 0 ? (nearPoint - camera) / near : glm::vec3(0);
  float far = glm::dot(farPoint - camera, forward);
  if (!std::isfinite(near) || !std::isfinite(far) || near <= 0 || far <= near ||
      !std::isfinite(std::log(far / near)))
    return g;
  // Check the complete near rectangle, not only its center.
  for (float x : {-1.f, 1.f})
    for (float y : {-1.f, 1.f}) {
      auto p = g.inverseViewProj * glm::vec4(x, y, 0, 1);
      if (std::abs(p.w) < 1e-12f)
        return g;
      float depth = glm::dot(glm::vec3(p) / p.w - camera, forward);
      if (!std::isfinite(depth) || depth <= 0 ||
          std::abs(depth - near) > near * .01f)
        return g; // Oblique or unsupported projection.
    }
  std::uint64_t nx =
      (std::uint64_t(width) + clusterTileSize - 1) / clusterTileSize;
  std::uint64_t ny =
      (std::uint64_t(height) + clusterTileSize - 1) / clusterTileSize;
  std::uint64_t cells = nx * ny * clusterDepthSlices;
  if (cells >
          std::numeric_limits<std::uint32_t>::max() / (clusterLightLimit + 1) ||
      cells > maxBytes / ((clusterLightLimit + 1) * 4ull) ||
      (cells + 63) / 64 > maxGroups)
    return g;
  g.cameraNear = glm::vec4(camera, near);
  g.forwardFar = glm::vec4(forward, far);
  g.grid = {nx, ny, clusterDepthSlices, cells};
  g.screen.z = 1;
  return g;
}
