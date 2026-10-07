#include "sun_cascades.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

void validateSunCascades(SunCascadeSettings const &s) {
  if ((s.count != 1 && s.count != 2 && s.count != 4) ||
      !std::isfinite(s.distance) || s.distance <= .001f ||
      !std::isfinite(s.splitLambda) || s.splitLambda < 0 || s.splitLambda > 1 ||
      !std::isfinite(s.blendFraction) || s.blendFraction < 0 ||
      s.blendFraction > .3f || !std::isfinite(s.casterDistance) ||
      s.casterDistance < 0 || !std::isfinite(s.biasConstant) ||
      s.biasConstant < 0 || !std::isfinite(s.biasSlope) || s.biasSlope < 0 ||
      s.debugIndex > 3)
    throw std::runtime_error("Invalid sun cascade settings");
}

SunCascadeGpu buildSunCascades(glm::mat4 const &vp, glm::vec3 camera,
                               glm::vec3 towardsSun,
                               SunCascadeSettings const &s, float pcfRadius) {
  validateSunCascades(s);
  SunCascadeGpu g;
  if (!s.enabled)
    return g;
  if (!std::isfinite(pcfRadius) || pcfRadius < 0 || pcfRadius > 4)
    throw std::runtime_error("Sun PCF radius must be in [0,4]");
  for (unsigned i = 0; i < 3; ++i)
    if (!std::isfinite(towardsSun[i]))
      throw std::runtime_error("Sun direction must be finite");
  for (unsigned i = 0; i < 3; ++i)
    if (!std::isfinite(camera[i]))
      return g;
  for (unsigned c = 0; c < 4; ++c)
    for (unsigned r = 0; r < 4; ++r)
      if (!std::isfinite(vp[c][r]))
        return g;
  glm::dmat4 matrix(vp);
  auto eye = matrix * glm::dvec4(camera, 1);
  if (std::abs(eye.w) > 1e-4 || std::abs(eye.x) > 1e-4 ||
      std::abs(eye.y) > 1e-4 || std::abs(glm::determinant(matrix)) < 1e-14)
    return g;
  auto inverse = glm::inverse(matrix);
  auto point = [&](double x, double y, double z) {
    auto p = inverse * glm::dvec4(x, y, z, 1);
    return glm::dvec3(p) / p.w;
  };
  glm::dvec3 eyeWorld(camera), nearCenter = point(0, 0, 0);
  double near = glm::length(nearCenter - eyeWorld);
  auto forward = (nearCenter - eyeWorld) / near;
  double far = glm::dot(point(0, 0, 1) - eyeWorld, forward);
  if (!std::isfinite(near) || !std::isfinite(far) || near < 1e-5 ||
      far <= near + .001)
    return g;
  std::array<glm::dvec3, 4> nearCorners, farCorners;
  unsigned k = 0;
  for (double x : {-1., 1.})
    for (double y : {-1., 1.}) {
      auto n = point(x, y, 0), f = point(x, y, 1);
      double nd = glm::dot(n - eyeWorld, forward);
      double fd = glm::dot(f - eyeWorld, forward);
      if (!std::isfinite(nd) || !std::isfinite(fd) ||
          std::abs(nd - near) > near * .001 || std::abs(fd - far) > far * .001)
        return g; // Oblique and reverse-Z cameras are not this contract.
      nearCorners[k] = n;
      farCorners[k++] = f;
    }
  double end = std::min(far, double(s.distance));
  if (end <= near + .001)
    return g;
  if (glm::length(towardsSun) <= .0001f)
    towardsSun = {0, 1, 0};
  auto light = glm::normalize(glm::dvec3(towardsSun));
  glm::dvec3 up =
      std::abs(light.y) > .95 ? glm::dvec3{0, 0, 1} : glm::dvec3{0, 1, 0};
  // Rotation anchored at world origin: snapping a camera-relative basis would
  // cancel its own translation and leave the shadow grid moving continuously.
  auto rotation = glm::lookAt(glm::dvec3(0), -light, up);
  g.splits = glm::vec4(float(end));
  g.forwardNear = glm::vec4(forward, near);
  g.params = {float(s.count), s.blendFraction, float(s.debugIndex), float(end)};
  for (unsigned i = 0; i < s.count; ++i) {
    double fraction = double(i + 1) / s.count;
    g.splits[i] = float(std::lerp(near + (end - near) * fraction,
                                  near * std::pow(end / near, fraction),
                                  double(s.splitLambda)));
    if (i + 1 == s.count)
      g.splits[i] = float(end);
    double start = i ? g.splits[i - 1] : near;
    if (i)
      start -=
          s.blendFraction * (start - (i > 1 ? double(g.splits[i - 2]) : near));
    std::array<glm::dvec3, 8> corners;
    glm::dvec3 center(0);
    for (unsigned j = 0; j < 4; ++j) {
      corners[j] = glm::mix(nearCorners[j], farCorners[j],
                            (start - near) / (far - near));
      corners[j + 4] = glm::mix(nearCorners[j], farCorners[j],
                                (double(g.splits[i]) - near) / (far - near));
      center += corners[j] + corners[j + 4];
    }
    center /= 8.;
    double radius = 0;
    for (auto p : corners)
      radius = std::max(radius, glm::length(p - center));
    // Twelve texels across diameter cover PCF4 plus snapping, independently of
    // the current kernel. Camera rotation does not refit a light-space AABB.
    radius *= double(sunCascadeResolution) / (sunCascadeResolution - 12);
    radius = std::ceil(radius * 16. - 1e-7) / 16.;
    double texel = 2 * radius / sunCascadeResolution;
    auto lightCenter = glm::dvec3(rotation * glm::dvec4(center, 1));
    lightCenter = glm::round(lightCenter / texel) * texel;
    double halfDepth = radius + s.casterDistance;
    auto view =
        glm::translate(glm::dmat4(1), -glm::dvec3(lightCenter.x, lightCenter.y,
                                                  lightCenter.z + halfDepth)) *
        rotation;
    auto projection =
        glm::ortho(-radius, radius, -radius, radius, 0., 2 * halfDepth);
    projection[1][1] *= -1;
    g.viewProj[i] = glm::mat4(projection * view);
    g.rects[i] = {.25f * (i % 2), .5f * (i / 2), .25f, .5f};
    g.bias[i] = {float(s.biasSlope * texel / (2 * halfDepth)),
                 float(s.biasConstant / (2 * halfDepth)), pcfRadius,
                 float(texel)};
  }
  return g;
}
