#include "temporal_motion.hpp"
#include <cmath>
#include <stdexcept>

namespace {
float halton(unsigned n, unsigned base) {
  float value = 0, weight = 1;
  while (n) {
    weight /= base;
    value += (n % base) * weight;
    n /= base;
  }
  return value;
}
void finite(glm::mat4 const &m) {
  for (unsigned c = 0; c < 4; ++c)
    for (unsigned r = 0; r < 4; ++r)
      if (!std::isfinite(m[c][r]))
        throw std::runtime_error("Temporal matrix must be finite");
}
} // namespace
glm::vec2 temporalJitterPixels(std::uint64_t sample) {
  unsigned n = unsigned(sample % 8) + 1;
  return {halton(n, 2) - .5f, halton(n, 3) - .5f};
}
glm::mat4 applyTemporalJitter(glm::mat4 const &vp, glm::vec2 jitterUv) {
  auto result = vp;
  for (unsigned c = 0; c < 4; ++c) {
    result[c][0] += 2 * jitterUv.x * vp[c][3];
    result[c][1] += 2 * jitterUv.y * vp[c][3];
  }
  return result;
}
glm::mat4 temporalCullingViewProj(glm::mat4 const &vp, std::uint32_t w,
                                  std::uint32_t h) {
  if (!w || !h)
    throw std::runtime_error("Temporal extent must be nonzero");
  auto result = vp;
  for (unsigned c = 0; c < 4; ++c) {
    result[c][0] /= 1.f + 1.f / w;
    result[c][1] /= 1.f + 1.f / h;
  }
  return result;
}
TemporalSnapshot
TemporalMotionHistory::prepare(glm::mat4 const &vp, glm::vec3 camera,
                               std::uint32_t w, std::uint32_t h, bool jitter,
                               std::span<MotionObject const> objects) const {
  finite(vp);
  auto inverse = glm::inverse(vp);
  finite(inverse);
  for (unsigned i = 0; i < 3; ++i)
    if (!std::isfinite(camera[i]))
      throw std::runtime_error("Temporal camera must be finite");
  if (!w || !h)
    throw std::runtime_error("Temporal extent must be nonzero");
  bool history = valid_ && previous_.width == w && previous_.height == h &&
                 previous_.jitter == jitter;
  TemporalSnapshot out;
  out.width = w;
  out.height = h;
  out.jitter = jitter;
  out.sample = history ? nextSample_ : 0;
  out.position = camera;
  out.camera.currentViewProj = vp;
  out.camera.previousViewProj = history ? previous_.camera.currentViewProj : vp;
  out.camera.previousCamera =
      glm::vec4(history ? previous_.position : camera, history ? 1.f : 0.f);
  glm::vec2 currentJitter =
      jitter ? temporalJitterPixels(out.sample) / glm::vec2(w, h)
             : glm::vec2(0);
  out.camera.jitterUv =
      glm::vec4(currentJitter,
                history ? glm::vec2(previous_.camera.jitterUv) : currentJitter);
  out.camera.rasterViewProj = applyTemporalJitter(vp, currentJitter);
  for (auto const &object : objects) {
    finite(object.model);
    if (object.identity == invalidMotionIdentity)
      continue;
    if (!out.objects.emplace(object.identity, object).second)
      throw std::runtime_error("Duplicate temporal object identity");
    if (out.gpu.size() >=
        std::uint64_t(std::numeric_limits<std::int32_t>::max()))
      throw std::runtime_error(
          "Temporal object index exceeds push constant range");
    MotionObjectGpu gpu;
    auto old = previous_.objects.find(object.identity);
    if (history && old != previous_.objects.end() &&
        old->second.mesh == object.mesh &&
        old->second.material == object.material) {
      gpu.previousModel = old->second.model;
      gpu.flags.x = 1;
    } else
      gpu.previousModel = object.model;
    out.indices.emplace(object.identity, std::uint32_t(out.gpu.size()));
    out.gpu.push_back(gpu);
  }
  return out;
}
void TemporalMotionHistory::commit(TemporalSnapshot &&snapshot) noexcept {
  nextSample_ = snapshot.sample + 1;
  previous_ = std::move(snapshot);
  // The published history only needs objects and camera, not upload records.
  previous_.gpu.clear();
  previous_.indices.clear();
  valid_ = true;
}
