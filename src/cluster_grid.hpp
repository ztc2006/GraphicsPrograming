#pragma once
#include "glm_include.hpp"
#include <cstddef>
#include <cstdint>

inline constexpr std::uint32_t clusterTileSize = 64, clusterDepthSlices = 24,
                               clusterLightLimit = 64;
struct alignas(16) ClusterGrid {
  glm::mat4 inverseViewProj{1};
  glm::vec4 cameraNear{0}, forwardFar{0};
  glm::uvec4 grid{0}, screen{0};
};
static_assert(sizeof(ClusterGrid) == 128 &&
              offsetof(ClusterGrid, cameraNear) == 64 &&
              offsetof(ClusterGrid, forwardFar) == 80 &&
              offsetof(ClusterGrid, grid) == 96 &&
              offsetof(ClusterGrid, screen) == 112);
// Disabled grids select the complete forward path; malformed projections and
// resource limits must never yield a partial light list.
ClusterGrid makeClusterGrid(glm::mat4 const &viewProj, glm::vec3 camera,
                            std::uint32_t width, std::uint32_t height,
                            std::size_t maxBytes,
                            std::uint32_t maxDispatchGroups,
                            bool requested = true);
std::size_t clusterListBytes(ClusterGrid const &grid);
