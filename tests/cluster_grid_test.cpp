#include "cluster_grid.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool value, char const *message) {
  if (!value)
    throw std::runtime_error(message);
}
// Reference points come from analytic view-space perspective rays. Bounds
// come from the packed inverse VP, so this checks coordinate/slice agreement
// independently of the shader's float projection arithmetic.
void checkGeometry(ClusterGrid const &g, glm::mat4 const &view,
                   glm::mat4 const &projection) {
  auto inverseView = glm::inverse(glm::dmat4(view));
  glm::dmat4 inversePacked(g.inverseViewProj);
  glm::dvec3 camera(g.cameraNear), forward(g.forwardFar);
  double near = g.cameraNear.w, far = g.forwardFar.w;
  for (double x :
       {.5, 63.999, 64.001, 127.999, 128.001, double(g.screen.x) - .5})
    for (double y : {.5, 63.999, 64.001, double(g.screen.y) - .5})
      for (unsigned boundary : {1u, 8u, 12u, 23u})
        for (double side : {.99999, 1.00001}) {
          if (x >= g.screen.x || y >= g.screen.y)
            continue;
          double depth =
              near * std::pow(far / near, double(boundary) / g.grid.z) * side;
          glm::dvec2 ndc = glm::dvec2(x, y) / glm::dvec2(g.screen) * 2.0 - 1.0;
          auto world = glm::dvec3(inverseView *
                                  glm::dvec4(ndc.x * depth / projection[0][0],
                                             ndc.y * depth / projection[1][1],
                                             -depth, 1));
          double actualDepth = glm::dot(world - camera, forward);
          auto z =
              unsigned(std::clamp(std::floor(std::log(actualDepth / near) /
                                             std::log(far / near) * g.grid.z),
                                  0.0, double(g.grid.z - 1)));
          glm::dvec2 low{std::floor(x / 64) * 64, std::floor(y / 64) * 64};
          auto high = glm::min(low + 64.0, glm::dvec2(g.screen));
          glm::dvec3 lo{1e100}, hi{-1e100};
          for (double px : {low.x, high.x})
            for (double py : {low.y, high.y}) {
              auto uv = glm::dvec2(px, py) / glm::dvec2(g.screen) * 2.0 - 1.0;
              auto h = inversePacked * glm::dvec4(uv, 0, 1);
              auto ray = glm::dvec3(h) / h.w - camera;
              for (unsigned plane : {z, z + 1}) {
                double d =
                    near * std::pow(far / near, double(plane) / g.grid.z);
                auto corner = camera + ray * (d / glm::dot(ray, forward));
                lo = glm::min(lo, corner);
                hi = glm::max(hi, corner);
              }
            }
          auto epsilon =
              1e-4 * (glm::dvec3(1) + glm::max(glm::abs(lo), glm::abs(hi)));
          require(glm::all(glm::greaterThanEqual(world, lo - epsilon)) &&
                      glm::all(glm::lessThanEqual(world, hi + epsilon)),
                  "Fragment omitted by its tile/depth cell bounds");
        }
}
int main() {
  for (auto camera : {glm::vec3(0, 0, 3), glm::vec3(10, 6, -8)}) {
    auto view = glm::lookAt(camera, glm::vec3(0), glm::vec3(0, 1, 0));
    for (float fov : {30.f, 75.f, 120.f}) {
      auto proj =
          glm::perspective(glm::radians(fov), 1920.f / 1080, .05f, 100.f);
      proj[1][1] *= -1;
      auto g = makeClusterGrid(proj * view, camera, 1920, 1080,
                               128 * 1024 * 1024, 65535);
      require(g.screen.z == 1 && g.grid == glm::uvec4(30, 17, 24, 12240),
              "Perspective grid shape incorrect");
      require(std::abs(g.cameraNear.w - .05f) < .0002f &&
                  std::abs(g.forwardFar.w - 100.f) < 1,
              "Recovered depth is not camera view depth");
      require(glm::dot(glm::vec3(g.forwardFar), glm::normalize(-camera)) >
                  .999f,
              "World view axis incorrect");
      require(clusterListBytes(g) == 12240 * 65 * 4,
              "Cluster index resource budget incorrect");
      auto fallback =
          makeClusterGrid(proj * view, camera, 1920, 1080, 1024, 65535);
      require(!fallback.screen.z && clusterListBytes(fallback) == 4,
              "Storage range fallback failed");
      require(!makeClusterGrid(proj * view, camera, 1920, 1080, SIZE_MAX, 1)
                   .screen.z,
              "Dispatch group budget ignored");
      auto odd = makeClusterGrid(proj * view, camera, 65, 129,
                                 128 * 1024 * 1024, 65535);
      require(odd.grid == glm::uvec4(2, 3, 24, 144), "Partial edge tile lost");
      require(!makeClusterGrid(proj * view, camera, UINT32_MAX, UINT32_MAX,
                               SIZE_MAX, UINT32_MAX)
                   .screen.z,
              "32-bit word addressing overflow accepted");
      require(!makeClusterGrid(proj * view, camera, 1920, 1080, SIZE_MAX, 65535,
                               false)
                   .screen.z,
              "Manual full reference not retained");
    }
  }
  require(!makeClusterGrid(glm::mat4(1), {0, 0, 2}, 1920, 1080, SIZE_MAX, 65535)
               .screen.z,
          "Orthographic/identity projection culled lights");
  require(!makeClusterGrid(glm::mat4(0), {0, 0, 0}, 1920, 1080, SIZE_MAX, 65535)
               .screen.z,
          "Singular matrix did not fall back");
  auto invalid = glm::mat4(1);
  invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
  require(!makeClusterGrid(invalid, {0, 0, 2}, 1920, 1080, SIZE_MAX, 65535)
               .screen.z,
          "Non-finite matrix did not fall back");
  std::cout << "PASS cluster grid: view depth, moved camera, wide FOV, edge "
               "tiles, projection/limits/overflow/manual fallback\n";
}
