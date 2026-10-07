#include "camera.hpp"
#include "sun_cascades.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, char const *message) {
  if (!value)
    throw std::runtime_error(message);
}
float matrixDifference(glm::mat4 a, glm::mat4 b) {
  float difference = 0;
  for (unsigned c = 0; c < 4; ++c)
    for (unsigned r = 0; r < 4; ++r)
      difference = std::max(difference, std::abs(a[c][r] - b[c][r]));
  return difference;
}
void covered(glm::mat4 m, glm::vec3 p) {
  auto clip = m * glm::vec4(p, 1);
  auto ndc = glm::vec3(clip) / clip.w;
  require(std::abs(ndc.x) <= 1.00001f && std::abs(ndc.y) <= 1.00001f &&
              ndc.z >= -.00001f && ndc.z <= 1.00001f,
          "Cascade clips a receiver/overlap/upstream caster");
}
} // namespace
int main() {
  try {
    Camera camera;
    camera.position = {0, 0, 2};
    camera.target = {0, 0, 0};
    camera.nearPlane = .05f;
    camera.farPlane = 100;
    auto vp = camera.viewProj(16.f / 9);
    SunCascadeSettings settings;
    auto inverse = glm::inverse(vp);
    glm::vec3 direction = glm::normalize(glm::vec3{-.4f, 1, .3f});
    for (unsigned count : {1u, 2u, 4u}) {
      settings.count = count;
      auto g = buildSunCascades(vp, camera.position, direction, settings, 4);
      require(g.params.x == count && g.params.w == settings.distance,
              "Cascade count/distance mismatch");
      require(std::abs(g.forwardNear.w - camera.nearPlane) < .00001f &&
                  glm::length(glm::vec3(g.forwardNear) - glm::vec3{0, 0, -1}) <
                      .00001f,
              "Vulkan depth 0/1 extraction failed");
      float previous = camera.nearPlane;
      for (unsigned i = 0; i < count; ++i) {
        require(g.splits[i] > previous, "Splits are not strictly increasing");
        float start = previous;
        if (i)
          start -= settings.blendFraction *
                   (previous - (i > 1 ? g.splits[i - 2] : camera.nearPlane));
        for (float x : {-1.f, 1.f})
          for (float y : {-1.f, 1.f}) {
            auto h = inverse * glm::vec4(x, y, 0, 1);
            auto ray = glm::vec3(h) / h.w - camera.position;
            ray /= glm::dot(ray, glm::vec3(g.forwardNear));
            covered(g.viewProj[i], camera.position + ray * start);
            covered(g.viewProj[i], camera.position + ray * g.splits[i]);
          }
        auto mid = camera.position +
                   glm::vec3(g.forwardNear) * ((start + g.splits[i]) * .5f);
        covered(g.viewProj[i], mid + direction * settings.casterDistance * .9f);
        require(g.rects[i].x + g.rects[i].z <= .5f &&
                    g.rects[i].y + g.rects[i].w <= 1,
                "Sun tile overlaps spot region");
        require(g.bias[i].w > 0 && g.bias[i].y > 0 && g.bias[i].z == 4,
                "World bias/PCF data missing");
        previous = g.splits[i];
      }
      require(g.splits[count - 1] == 60, "Last split misses bounded far plane");
    }
    settings.count = 4;
    // Pure camera translation under a tenth of the nearest tile's texel must
    // retain the same world grid, rather than shifting every projected pixel.
    auto a = buildSunCascades(vp, camera.position, {0, 0, 1}, settings, 0);
    auto moved = camera;
    moved.position.x += a.bias[0].w * .1f;
    moved.target.x += a.bias[0].w * .1f;
    auto b = buildSunCascades(moved.viewProj(16.f / 9), moved.position,
                              {0, 0, 1}, settings, 4);
    for (unsigned i = 0; i < 4; ++i)
      require(matrixDifference(a.viewProj[i], b.viewProj[i]) < .00001f,
              "Subtexel translation moves stable cascade matrix");
    auto rotated = camera;
    rotated.target = {.3f, .15f, 0};
    b = buildSunCascades(rotated.viewProj(16.f / 9), rotated.position,
                         {0, 1, 0}, settings, 4);
    for (unsigned i = 0; i < 4; ++i)
      require(std::abs(a.bias[i].w - b.bias[i].w) < .00001f,
              "Camera rotation changes cascade radius");
    settings.splitLambda = 0;
    a = buildSunCascades(vp, camera.position, {0, 1, 0}, settings, 1);
    require(std::abs(a.splits[0] - (camera.nearPlane +
                                    (60 - camera.nearPlane) * .25f)) < .0001f,
            "Linear split endpoint failed");
    settings.splitLambda = 1;
    b = buildSunCascades(vp, camera.position, {0, 0, 0}, settings, 1);
    require(b.splits[0] < a.splits[0] && b.params.x == 4,
            "Log split or zero-direction fallback failed");
    settings.distance = 200;
    b = buildSunCascades(vp, camera.position, {0, 1, 0}, settings, 1);
    require(std::abs(b.params.w - camera.farPlane) < .02f,
            "Camera far plane ignored");
    require(
        buildSunCascades(glm::mat4(1), camera.position, {0, 1, 0}, settings, 1)
                .params.x == 0,
        "Unsupported projection did not use legacy map");
    auto badVp = vp;
    badVp[0][0] = std::numeric_limits<float>::quiet_NaN();
    require(buildSunCascades(badVp, camera.position, {0, 1, 0}, settings, 1)
                    .params.x == 0,
            "Nonfinite matrix did not fall back");
    auto rejects = [&](SunCascadeSettings const &s) {
      try {
        (void)buildSunCascades(vp, camera.position, {0, 1, 0}, s, 1);
      } catch (std::runtime_error const &) {
        return;
      }
      throw std::runtime_error("Invalid settings accepted");
    };
    for (unsigned count : {0u, 3u, 5u}) {
      auto bad = settings;
      bad.count = count;
      rejects(bad);
    }
    auto bad = settings;
    bad.distance = 0;
    rejects(bad);
    bad = settings;
    bad.blendFraction = .4f;
    rejects(bad);
    bad = settings;
    bad.casterDistance = -1;
    rejects(bad);
    bad = settings;
    bad.biasSlope = std::numeric_limits<float>::infinity();
    rejects(bad);
    std::cout << "PASS CSM: splits, depth0/1, overlap/caster coverage, world "
                 "grid, rotation, vertical sun, bias, limits/fallback\n";
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
