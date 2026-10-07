#include "temporal_motion.hpp"
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>
void require(bool ok, char const *why) {
  if (!ok)
    throw std::runtime_error(why);
}
int main() {
  TemporalMotionHistory history;
  MotionObject a{7, 0, 0, glm::mat4(1)}, b{9, 0, 0, glm::mat4(1)};
  auto first = history.prepare(glm::mat4(1), {0, 0, 2}, 1920, 1080, true,
                               std::array{a, b});
  require(first.camera.previousCamera.w == 0 && first.gpu[1].flags.x == 0,
          "First frame has history");
  auto again = history.prepare(glm::mat4(1), {0, 0, 2}, 1920, 1080, true,
                               std::array{a, b});
  require(again.sample == 0 && history.nextSample() == 0,
          "Unsubmitted snapshot advanced sequence");
  history.commit(std::move(first));
  a.model = glm::translate(glm::mat4(1), {.2f, 0, 0});
  auto next = history.prepare(glm::mat4(1), {0, 0, 2}, 1920, 1080, true,
                              std::array{b, a});
  require(next.sample == 1 && next.camera.previousCamera.w == 1 &&
              next.gpu[next.indices.at(7)].flags.x == 1,
          "Submitted frame missing history");
  require(next.gpu[next.indices.at(7)].previousModel[3].x == 0 &&
              next.gpu[next.indices.at(9)].previousModel[3].x == 0,
          "Instances or list order mixed");
  auto projected = next.camera.rasterViewProj * glm::vec4(.1f, .2f, .5f, 1);
  require(std::abs(projected.x - .1f - 2 * next.camera.jitterUv.x) < 1e-7f &&
              std::abs(projected.y - .2f - 2 * next.camera.jitterUv.y) <
                  1e-7f &&
              projected.z == .5f,
          "Jitter UV convention incorrect");
  require(glm::vec2(next.camera.jitterUv.z, next.camera.jitterUv.w) ==
              glm::vec2(again.camera.jitterUv),
          "Previous jitter missing");
  history.commit(std::move(next));
  auto removed =
      history.prepare(glm::mat4(1), {0, 0, 2}, 1920, 1080, true, std::array{b});
  history.commit(std::move(removed));
  auto returned = history.prepare(glm::mat4(1), {0, 0, 2}, 1920, 1080, true,
                                  std::array{a, b});
  require(returned.gpu[returned.indices.at(7)].flags.x == 0 &&
              returned.gpu[returned.indices.at(9)].flags.x == 1,
          "Removed identity reused stale transform");
  b.material = 1;
  auto replaced =
      history.prepare(glm::mat4(1), {0, 0, 2}, 1920, 1080, true, std::array{b});
  require(replaced.gpu[1].flags.x == 0,
          "Changed material accepted stale history");
  b.material = 0;
  b.mesh = 1;
  auto remeshed =
      history.prepare(glm::mat4(1), {0, 0, 2}, 1920, 1080, true, std::array{b});
  require(remeshed.gpu[1].flags.x == 0,
          "Changed mesh reused old geometry history");
  auto resized =
      history.prepare(glm::mat4(1), {0, 0, 2}, 1280, 720, true, std::array{b});
  require(resized.camera.previousCamera.w == 0 && resized.sample == 0,
          "Resize alone failed reset");
  auto jitterSwitch = history.prepare(glm::mat4(1), {0, 0, 2}, 1920, 1080,
                                      false, std::array{b});
  require(jitterSwitch.camera.previousCamera.w == 0 && jitterSwitch.sample == 0,
          "Jitter switch alone failed reset");
  auto culling = temporalCullingViewProj(glm::mat4(1), 100, 100) *
                 glm::vec4(1.005f, 0, .5f, 1);
  require(culling.x < 1, "Jitter culling guard omitted edge");
  for (unsigned i = 0; i < 8; ++i) {
    auto j = temporalJitterPixels(i);
    require(std::abs(j.x) <= .5 && std::abs(j.y) <= .5 &&
                j == temporalJitterPixels(i + 8),
            "Halton bound/period incorrect");
  }
  bool rejected = false;
  try {
    history.prepare(glm::mat4(1), {0, 0, 2}, 1920, 1080, true,
                    std::array{a, a});
  } catch (std::runtime_error const &) {
    rejected = true;
  }
  require(rejected, "Duplicate identity accepted");
  rejected = false;
  try {
    history.prepare(glm::mat4(0), {0, 0, 2}, 1920, 1080, true, {});
  } catch (std::runtime_error const &) {
    rejected = true;
  }
  require(rejected, "Singular temporal camera accepted");
  history.reset();
  require(history.nextSample() == 0, "Explicit reset failed");
  std::cout << "PASS temporal CPU: submission-only history, reordered "
               "instances, removal/reuse, jitter UV/period, culling guard, "
               "resets and invalid camera/identity\n";
}
