#include "hdr_ibl.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
constexpr float kPi = std::numbers::pi_v<float>;

std::array<float, 9> evaluateShBasis(glm::vec3 const &direction) {
  return {
      0.28209479f,
      0.48860251f * direction.y,
      0.48860251f * direction.z,
      0.48860251f * direction.x,
      1.09254843f * direction.x * direction.y,
      1.09254843f * direction.y * direction.z,
      0.31539156f * (3.0f * direction.z * direction.z - 1.0f),
      1.09254843f * direction.x * direction.z,
      0.54627421f * (direction.x * direction.x - direction.y * direction.y),
  };
}
} // namespace

EnvironmentSh projectEquirectangularToSh(HdrImage const &image) {
  validateEnvironmentImage(image);
  // Integrate each basis over the texel's exact spherical rectangle. This
  // removes midpoint quadrature's spurious bands even for a tiny constant map.
  std::array<glm::dvec3, 9> sums{};
  double const pi = std::numbers::pi;
  for (std::uint32_t y = 0; y < image.height; ++y) {
    double a = (double(y) / image.height - .5) * pi;
    double b = (double(y + 1) / image.height - .5) * pi;
    double sa = std::sin(a), sb = std::sin(b);
    double lat0 = sb - sa;
    double latY = (sb * sb - sa * sa) / 2;
    double latC2 = (b - a) / 2 + (std::sin(2 * b) - std::sin(2 * a)) / 4;
    double latYC2 = (std::pow(std::cos(a), 3) - std::pow(std::cos(b), 3)) / 3;
    double latC3 = sb - sb * sb * sb / 3 - sa + sa * sa * sa / 3;
    double latY2 = (sb * sb * sb - sa * sa * sa) / 3;
    for (std::uint32_t x = 0; x < image.width; ++x) {
      double u = (double(x) / image.width - .5) * 2 * pi;
      double v = (double(x + 1) / image.width - .5) * 2 * pi;
      double lon0 = v - u, lonC = std::sin(v) - std::sin(u);
      double lonS = std::cos(u) - std::cos(v);
      double lonC2 = (v - u) / 2 + (std::sin(2 * v) - std::sin(2 * u)) / 4;
      double lonS2 = (v - u) - lonC2;
      double lonCS =
          (std::sin(v) * std::sin(v) - std::sin(u) * std::sin(u)) / 2;
      std::array<double, 9> weights{
          .28209479177387814 * lat0 * lon0,
          .4886025119029199 * latY * lon0,
          .4886025119029199 * latC2 * lonS,
          .4886025119029199 * latC2 * lonC,
          1.0925484305920792 * latYC2 * lonC,
          1.0925484305920792 * latYC2 * lonS,
          .31539156525252005 * (3 * latC3 * lonS2 - lat0 * lon0),
          1.0925484305920792 * latC3 * lonCS,
          .5462742152960396 * (latC3 * lonC2 - latY2 * lon0)};
      auto i = (std::size_t(y) * image.width + x) * 4;
      glm::dvec3 value{image.rgba[i], image.rgba[i + 1], image.rgba[i + 2]};
      for (unsigned c = 0; c < 9; ++c)
        sums[c] += value * weights[c];
    }
  }
  EnvironmentSh coefficients{};
  for (unsigned c = 0; c < 9; ++c)
    coefficients[c] = glm::vec3(sums[c]);
  return coefficients;
}

glm::vec3 evaluateIrradianceSh(EnvironmentSh const &coefficients,
                               glm::vec3 direction) {
  float const lengthSquared = glm::dot(direction, direction);
  if (lengthSquared <= 0.000001f) {
    return glm::vec3{0.0f};
  }
  direction *= glm::inversesqrt(lengthSquared);
  auto const basis = evaluateShBasis(direction);

  glm::vec3 irradiance = coefficients[0] * basis[0] * kPi;
  for (std::size_t coefficient = 1; coefficient <= 3; ++coefficient) {
    irradiance +=
        coefficients[coefficient] * basis[coefficient] * (2.0f * kPi / 3.0f);
  }
  for (std::size_t coefficient = 4; coefficient < coefficients.size();
       ++coefficient) {
    irradiance += coefficients[coefficient] * basis[coefficient] * (kPi / 4.0f);
  }
  return glm::max(irradiance, glm::vec3{0.0f});
}

void validateEnvironmentImage(HdrImage const &image) {
  if (!image.width || !image.height || image.width > 16384 ||
      image.height > 16384 ||
      image.rgba.size() != std::uint64_t(image.width) * image.height * 4)
    throw std::runtime_error("Invalid environment HDR dimensions/data");
  for (std::size_t i = 0; i < image.rgba.size(); ++i)
    if (!std::isfinite(image.rgba[i]) || image.rgba[i] < 0)
      throw std::runtime_error(
          "Environment HDR must be finite and nonnegative");
}
void validateEnvironmentBakeSettings(EnvironmentBakeSettings const &s) {
  if (s.faceSize < 2 || s.faceSize > 512 || (s.faceSize & (s.faceSize - 1)) ||
      s.lutSize < 2 || s.lutSize > 512 || !s.prefilterSamples ||
      s.prefilterSamples > 16384 || !s.lutSamples || s.lutSamples > 16384)
    throw std::runtime_error("Invalid environment bake size/sample count");
}
glm::vec3 environmentCubeDirection(unsigned face, float s, float t) {
  if (face >= 6 || !std::isfinite(s) || !std::isfinite(t))
    throw std::runtime_error("Invalid environment cube direction");
  std::array<glm::vec3, 6> directions{{{1, -t, -s},
                                       {-1, -t, s},
                                       {s, 1, t},
                                       {s, -1, -t},
                                       {s, -t, 1},
                                       {-s, -t, -1}}};
  return glm::normalize(directions[face]);
}

namespace {
float radicalInverse(std::uint32_t bits) {
  bits = (bits << 16) | (bits >> 16);
  bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
  bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
  bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
  bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
  return float(bits) * 2.3283064365386963e-10f;
}
glm::vec3 ggxHalf(std::uint32_t i, std::uint32_t count, float r) {
  float a = r * r, a2 = a * a, xi = radicalInverse(i);
  float cosine = std::sqrt((1 - xi) / (1 + (a2 - 1) * xi));
  float sine = std::sqrt(std::max(0.f, 1 - cosine * cosine));
  float phi = 2 * kPi * float(i) / float(count);
  return {sine * std::cos(phi), sine * std::sin(phi), cosine};
}
float geometryIbl(float n, float roughness) {
  float k = roughness * roughness / 2;
  return n / (n * (1 - k) + k);
}
glm::vec3 sampleSource(HdrImage const &image, glm::vec3 d) {
  float u = std::atan2(d.z, d.x) / (2 * kPi) + .5f;
  float v = std::asin(std::clamp(d.y, -1.f, 1.f)) / kPi + .5f;
  float px = u * float(image.width) - .5f, py = v * float(image.height) - .5f;
  int x = int(std::floor(px)), y = int(std::floor(py));
  auto fetch = [&](int xx, int yy) {
    xx = (xx % int(image.width) + int(image.width)) % int(image.width);
    yy = std::clamp(yy, 0, int(image.height) - 1);
    auto p = (std::size_t(yy) * image.width + unsigned(xx)) * 4;
    return glm::vec3(image.rgba[p], image.rgba[p + 1], image.rgba[p + 2]);
  };
  return glm::mix(glm::mix(fetch(x, y), fetch(x + 1, y), px - float(x)),
                  glm::mix(fetch(x, y + 1), fetch(x + 1, y + 1), px - float(x)),
                  py - float(y));
}
struct CubeSourceLevel {
  unsigned size = 0;
  std::vector<glm::vec3> pixels;
  std::vector<float> solidAngles; // Face-independent exact texel areas.
};
struct CubeCoordinate {
  unsigned face;
  float s, t;
};
CubeCoordinate cubeCoordinate(glm::vec3 d) {
  auto a = glm::abs(d);
  if (a.z >= a.x && a.z >= a.y)
    return d.z >= 0 ? CubeCoordinate{4, d.x / a.z, -d.y / a.z}
                    : CubeCoordinate{5, -d.x / a.z, -d.y / a.z};
  if (a.y >= a.x)
    return d.y >= 0 ? CubeCoordinate{2, d.x / a.y, d.z / a.y}
                    : CubeCoordinate{3, d.x / a.y, -d.z / a.y};
  return d.x >= 0 ? CubeCoordinate{0, -d.z / a.x, -d.y / a.x}
                  : CubeCoordinate{1, d.z / a.x, -d.y / a.x};
}
double cubeTexelAngle(unsigned size, unsigned x, unsigned y) {
  auto area = [](double s, double t) {
    return std::atan2(s * t, std::sqrt(s * s + t * t + 1));
  };
  double s0 = 2 * double(x) / size - 1, s1 = 2 * double(x + 1) / size - 1;
  double t0 = 2 * double(y) / size - 1, t1 = 2 * double(y + 1) / size - 1;
  return area(s1, t1) - area(s0, t1) - area(s1, t0) + area(s0, t0);
}
glm::vec3 sampleCube(CubeSourceLevel const &level, glm::vec3 direction) {
  auto uv = cubeCoordinate(direction);
  float px = (uv.s * .5f + .5f) * level.size - .5f,
        py = (uv.t * .5f + .5f) * level.size - .5f;
  int x = int(std::floor(px)), y = int(std::floor(py));
  auto fetch = [&](int xx, int yy) {
    unsigned face = uv.face;
    if (xx < 0 || yy < 0 || xx >= int(level.size) || yy >= int(level.size)) {
      auto adjacent = cubeCoordinate(
          environmentCubeDirection(face, 2 * (float(xx) + .5f) / level.size - 1,
                                   2 * (float(yy) + .5f) / level.size - 1));
      face = adjacent.face;
      xx = int((adjacent.s * .5f + .5f) * level.size);
      yy = int((adjacent.t * .5f + .5f) * level.size);
    }
    xx = std::clamp(xx, 0, int(level.size) - 1);
    yy = std::clamp(yy, 0, int(level.size) - 1);
    return level
        .pixels[(std::size_t(face) * level.size + unsigned(yy)) * level.size +
                unsigned(xx)];
  };
  return glm::mix(glm::mix(fetch(x, y), fetch(x + 1, y), px - x),
                  glm::mix(fetch(x, y + 1), fetch(x + 1, y + 1), px - x),
                  py - y);
}
std::vector<CubeSourceLevel> sourcePyramid(HdrImage const &image,
                                           unsigned outputSize) {
  // Convert once to a directional representation. Equirectangular area-driven
  // isotropic mips overblur latitude at the poles; cube footprints avoid that.
  unsigned size = std::bit_ceil(
      std::clamp(std::max({2 * outputSize, image.width / 4, image.height / 2}),
                 2u, 1024u));
  CubeSourceLevel base{size,
                       std::vector<glm::vec3>(std::size_t(size) * size * 6)};
  base.solidAngles.resize(std::size_t(size) * size);
  for (unsigned y = 0; y < size; ++y)
    for (unsigned x = 0; x < size; ++x)
      base.solidAngles[std::size_t(y) * size + x] =
          float(cubeTexelAngle(size, x, y));
  for (unsigned face = 0; face < 6; ++face)
    for (unsigned y = 0; y < size; ++y)
      for (unsigned x = 0; x < size; ++x) {
        glm::dvec3 sum{};
        // Stratified conversion reduces aliasing when shrinking the source. It
        // is finite-resolution filtering, not an exact integral of arbitrary
        // emitters.
        for (float dy : {.25f, .75f})
          for (float dx : {.25f, .75f})
            sum += glm::dvec3(sampleSource(
                image, environmentCubeDirection(face, 2 * (x + dx) / size - 1,
                                                2 * (y + dy) / size - 1)));
        base.pixels[(std::size_t(face) * size + y) * size + x] =
            glm::vec3(sum / 4.0);
      }
  std::vector<CubeSourceLevel> result;
  result.push_back(std::move(base));
  while (result.back().size > 1) {
    auto const &src = result.back();
    unsigned next = src.size / 2;
    CubeSourceLevel dst{next,
                        std::vector<glm::vec3>(std::size_t(next) * next * 6)};
    dst.solidAngles.resize(std::size_t(next) * next);
    for (unsigned face = 0; face < 6; ++face)
      for (unsigned y = 0; y < next; ++y)
        for (unsigned x = 0; x < next; ++x) {
          glm::dvec3 sum{};
          double weight = 0;
          for (unsigned dy = 0; dy < 2; ++dy)
            for (unsigned dx = 0; dx < 2; ++dx) {
              auto xx = x * 2 + dx, yy = y * 2 + dy;
              double w = src.solidAngles[std::size_t(yy) * src.size + xx];
              sum += glm::dvec3(src.pixels[(std::size_t(face) * src.size + yy) *
                                               src.size +
                                           xx]) *
                     w;
              weight += w;
            }
          dst.solidAngles[std::size_t(y) * next + x] = float(weight);
          dst.pixels[(std::size_t(face) * next + y) * next + x] =
              glm::vec3(sum / weight);
        }
    result.push_back(std::move(dst));
  }
  return result;
}
glm::vec3 samplePyramid(std::vector<CubeSourceLevel> const &pyramid,
                        glm::vec3 l, float sampleAngle) {
  unsigned size = pyramid.front().size;
  auto uv = cubeCoordinate(l);
  auto x = std::min(size - 1, unsigned((uv.s * .5f + .5f) * size));
  auto y = std::min(size - 1, unsigned((uv.t * .5f + .5f) * size));
  double texelAngle = pyramid.front().solidAngles[std::size_t(y) * size + x];
  float lod = std::clamp(float(.5 * std::log2(sampleAngle / texelAngle)), 0.f,
                         float(pyramid.size() - 1));
  auto level = unsigned(lod),
       next = std::min(level + 1, unsigned(pyramid.size() - 1));
  return glm::mix(sampleCube(pyramid[level], l), sampleCube(pyramid[next], l),
                  lod - float(level));
}
} // namespace

glm::vec2 integrateEnvironmentBrdf(float noV, float r, std::uint32_t samples) {
  if (!std::isfinite(noV) || noV <= 0 || noV > 1 || !std::isfinite(r) ||
      r < 0 || r > 1 || !samples || samples > 16384)
    throw std::runtime_error("Invalid BRDF integration parameters");
  if (r == 0) {
    float f = std::pow(1 - noV, 5);
    return {1 - f, f};
  }
  glm::vec3 v{std::sqrt(std::max(0.f, 1 - noV * noV)), 0, noV};
  glm::dvec2 sum{};
  for (std::uint32_t i = 0; i < samples; ++i) {
    auto h = ggxHalf(i, samples, r);
    float voH = std::max(glm::dot(v, h), 0.f);
    auto l = 2 * voH * h - v;
    float noL = l.z;
    if (noL <= 0 || voH <= 0)
      continue;
    double g = geometryIbl(noL, r) * geometryIbl(noV, r) * voH / (h.z * noV);
    double fc = std::pow(1 - voH, 5);
    sum += glm::dvec2((1 - fc) * g, fc * g);
  }
  return glm::vec2(sum / double(samples));
}

BakedEnvironment bakeEnvironment(HdrImage const &image,
                                 EnvironmentBakeSettings const &s) {
  validateEnvironmentImage(image);
  validateEnvironmentBakeSettings(s);
  BakedEnvironment result;
  result.sh = projectEquirectangularToSh(image);
  auto pyramid = sourcePyramid(image, s.faceSize);
  for (auto size = s.faceSize;; size /= 2) {
    result.levels.push_back({size, result.cubeRgba.size()});
    result.cubeRgba.resize(result.cubeRgba.size() +
                           std::size_t(size) * size * 6 * 4);
    if (size == 1)
      break;
  }
  for (unsigned mip = 0; mip < result.levels.size(); ++mip) {
    auto level = result.levels[mip];
    float r = float(mip) / float(result.levels.size() - 1), alpha = r * r;
    std::vector<glm::vec3> halves;
    if (mip)
      for (unsigned i = 0; i < s.prefilterSamples; ++i)
        halves.push_back(ggxHalf(i, s.prefilterSamples, r));
    for (unsigned face = 0; face < 6; ++face)
      for (unsigned y = 0; y < level.size; ++y)
        for (unsigned x = 0; x < level.size; ++x) {
          auto n = environmentCubeDirection(
              face, 2 * (float(x) + .5f) / level.size - 1,
              2 * (float(y) + .5f) / level.size - 1);
          glm::dvec3 sum{};
          double weight = 0;
          float texelAngle = float(cubeTexelAngle(level.size, x, y));
          if (!mip) {
            sum = samplePyramid(pyramid, n, texelAngle);
            weight = 1;
          } else {
            auto up =
                std::abs(n.z) < .999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
            auto tangent = glm::normalize(glm::cross(up, n)),
                 bitangent = glm::cross(n, tangent);
            for (auto local : halves) {
              auto h = tangent * local.x + bitangent * local.y + n * local.z;
              auto l = glm::normalize(2 * local.z * h - n);
              float noL = glm::dot(n, l);
              if (noL <= 0)
                continue;
              float denom = local.z * local.z * (alpha * alpha - 1) + 1;
              float d = alpha * alpha / (kPi * denom * denom);
              // N=V: PDF(l)=D(h)*NoH/(4*VoH)=D(h)/4.
              float sampleAngle = 4 / (float(s.prefilterSamples) * d);
              sum += glm::dvec3(samplePyramid(
                         pyramid, l, std::max(sampleAngle, texelAngle))) *
                     double(noL);
              weight += noL;
            }
          }
          auto p = level.offset +
                   ((std::size_t(face) * level.size + y) * level.size + x) * 4;
          for (unsigned c = 0; c < 3; ++c)
            result.cubeRgba[p + c] = float(sum[c] / weight);
          result.cubeRgba[p + 3] = 1;
        }
  }
  result.brdfLut = {
      .width = s.lutSize,
      .height = s.lutSize,
      .rgba = std::vector<float>(std::size_t(s.lutSize) * s.lutSize * 4)};
  for (unsigned y = 0; y < s.lutSize; ++y)
    for (unsigned x = 0; x < s.lutSize; ++x) {
      auto ab =
          integrateEnvironmentBrdf((float(x) + .5f) / s.lutSize,
                                   (float(y) + .5f) / s.lutSize, s.lutSamples);
      auto p = (std::size_t(y) * s.lutSize + x) * 4;
      result.brdfLut.rgba[p] = ab.x;
      result.brdfLut.rgba[p + 1] = ab.y;
      result.brdfLut.rgba[p + 3] = 1;
    }
  return result;
}
