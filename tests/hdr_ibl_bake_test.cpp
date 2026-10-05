#include "hdr_ibl_cache.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
constexpr double pi = std::numbers::pi;
void require(bool condition, char const *message) {
  if (!condition)
    throw std::runtime_error(message);
}
HdrImage source(unsigned width, unsigned height, bool gradient = false) {
  HdrImage image{.width = width,
                 .height = height,
                 .rgba = std::vector<float>(std::size_t(width) * height * 4)};
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      double lat = ((y + .5) / height - .5) * pi,
             lon = ((x + .5) / width - .5) * 2 * pi;
      glm::vec3 value = gradient ? glm::vec3(1 + std::cos(lat) * std::cos(lon),
                                             1 + std::sin(lat),
                                             1 + std::cos(lat) * std::sin(lon))
                                 : glm::vec3(4, 1, 12);
      auto p = (std::size_t(y) * width + x) * 4;
      for (unsigned c = 0; c < 3; ++c)
        image.rgba[p + c] = value[c];
      image.rgba[p + 3] = 1;
    }
  return image;
}
// Independent deterministic hemisphere quadrature, not GGX/Hammersley draws.
glm::dvec2 reference(double nv, double r) {
  glm::dvec3 v{std::sqrt(1 - nv * nv), 0, nv};
  auto g = [&](double n) {
    double k = r * r / 2;
    return n / (n * (1 - k) + k);
  };
  glm::dvec2 sum{};
  constexpr unsigned rows = 1024, columns = 512;
  for (unsigned y = 0; y < rows; ++y) {
    double nl = (y + .5) / rows, sinL = std::sqrt(1 - nl * nl);
    for (unsigned x = 0; x < columns; ++x) {
      double phi = (x + .5) / columns * 2 * pi;
      glm::dvec3 l{sinL * std::cos(phi), sinL * std::sin(phi), nl};
      auto h = glm::normalize(l + v);
      double vh = glm::dot(v, h);
      double a = r * r, den = h.z * h.z * (a * a - 1) + 1;
      double d = a * a / (pi * den * den), fc = std::pow(1 - vh, 5);
      double weight = d * g(nv) * g(nl) / (4 * nv) * 2 * pi / (rows * columns);
      sum += glm::dvec2(1 - fc, fc) * weight;
    }
  }
  return sum;
}
void numerics() {
  EnvironmentBakeSettings settings{.faceSize = 8,
                                   .prefilterSamples = 256,
                                   .lutSize = 16,
                                   .lutSamples = 1024};
  auto constant = source(4, 2);
  auto bake = bakeEnvironment(constant, settings);
  for (auto level : bake.levels)
    for (std::size_t i = level.offset;
         i < level.offset + std::size_t(level.size) * level.size * 24; i += 4) {
      require(std::abs(bake.cubeRgba[i] - 4) < .0004f &&
                  std::abs(bake.cubeRgba[i + 1] - 1) < .0001f &&
                  std::abs(bake.cubeRgba[i + 2] - 12) < .0012f &&
                  bake.cubeRgba[i + 3] == 1,
              "Constant HDR cube lost energy/range in a face/mip");
    }
  for (auto n : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, -1),
                 glm::normalize(glm::vec3(1, 2, 3))}) {
    auto irradiance = evaluateIrradianceSh(bake.sh, n);
    require(glm::all(glm::lessThan(
                glm::abs(irradiance - glm::vec3(4, 1, 12) * float(pi)),
                glm::vec3(.0001f))),
            "Tiny constant HDR SH contains spurious bands/incorrect pi");
  }
  auto gradientSettings = settings;
  gradientSettings.faceSize = 32;
  auto gradient = bakeEnvironment(source(128, 64, true), gradientSettings);
  auto base = gradient.levels.front();
  for (unsigned f = 0; f < 6; ++f)
    for (unsigned y = 0; y < base.size; ++y)
      for (unsigned x = 0; x < base.size; ++x) {
        auto direction = environmentCubeDirection(
            f, 2 * (x + .5f) / base.size - 1, 2 * (y + .5f) / base.size - 1);
        auto p = ((std::size_t(f) * base.size + y) * base.size + x) * 4;
        glm::vec3 actual{gradient.cubeRgba[p], gradient.cubeRgba[p + 1],
                         gradient.cubeRgba[p + 2]};
        require(
            glm::all(
                glm::lessThan(glm::abs(actual - (glm::vec3(1) + direction)),
                              glm::vec3(.003f))),
            "Cube face orientation/edge/equirectangular direction mismatch");
      }
  // Explicit Vulkan face-axis contract, independent of the source generator.
  std::array axes{glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
                  glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
                  glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)};
  for (unsigned f = 0; f < 6; ++f)
    require(environmentCubeDirection(f, 0, 0) == axes[f],
            "Cube axis order inverted");
  require(glm::length(environmentCubeDirection(0, -1, 0) -
                      environmentCubeDirection(4, 1, 0)) < 1e-6f,
          "Adjacent cube faces disagree at their common edge");
  auto ab = integrateEnvironmentBrdf(1, 1, 16384);
  require(std::abs(ab.x + ab.y - (1 - std::log(2.0))) < .0001,
          "BRDF roughness=1 white furnace differs from 1-ln(2)");
  for (float nv : {.05f, .2f, .5f, 1.f})
    for (float r : {0.f, .1f, .3f, .7f, 1.f}) {
      auto value = integrateEnvironmentBrdf(nv, r, 8192);
      require(std::isfinite(value.x) && std::isfinite(value.y) &&
                  value.x >= 0 && value.y >= 0 && value.x + value.y < 1.006f,
              "BRDF white furnace creates energy or NaN");
      if (r == 0)
        require(std::abs(value.y - std::pow(1 - nv, 5)) < 1e-6 &&
                    std::abs(value.x + value.y - 1) < 1e-6,
                "BRDF zero-roughness Fresnel limit differs");
    }
  for (float nv : {.1f, .5f, 1.f})
    for (float r : {.3f, .6f, 1.f}) {
      auto actual = glm::dvec2(integrateEnvironmentBrdf(nv, r, 16384));
      auto expected = reference(nv, r);
      require(glm::all(
                  glm::lessThan(glm::abs(actual - expected), glm::dvec2(.008))),
              "BRDF split-sum integration differs from independent hemisphere "
              "quadrature");
    }
  auto bright = source(128, 64);
  for (unsigned y = 0; y < bright.height; ++y)
    for (unsigned x = 0; x < bright.width; ++x) {
      double lat = ((y + .5) / bright.height - .5) * pi,
             lon = ((x + .5) / bright.width - .5) * 2 * pi;
      float value = std::cos(lat) * std::cos(lon) > .992 ? 100.f : .02f;
      auto p = (std::size_t(y) * bright.width + x) * 4;
      for (unsigned c = 0; c < 3; ++c)
        bright.rgba[p + c] = value;
    }
  settings.faceSize = 32;
  auto filtered = bakeEnvironment(bright, settings);
  auto peak = [&](unsigned mip) {
    auto level = filtered.levels[mip];
    float value = 0;
    for (std::size_t p = level.offset;
         p < level.offset + std::size_t(level.size) * level.size * 24; p += 4)
      value = std::max(value, filtered.cubeRgba[p]);
    return value;
  };
  require(peak(0) > 50 &&
              peak(unsigned(filtered.levels.size() - 1)) < peak(0) * .2f,
          "GGX high roughness does not spread/dampen bright source");
  for (float value : filtered.cubeRgba)
    require(std::isfinite(value) && value >= 0, "Bright HDR bake contains NaN");
  std::cout << "PASS: HDR energy, exact SH areas, cube axes/edges, BRDF "
               "limits/white furnace/quadrature, bright emitter\n";
}
void cache() {
  auto directory =
      std::filesystem::temp_directory_path() /
      ("vulkan-ibl-cache-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    std::filesystem::path p;
    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove_all(p, e);
    }
  } cleanup{directory};
  auto image = source(8, 4);
  EnvironmentBakeSettings s{
      .faceSize = 4, .prefilterSamples = 16, .lutSize = 4, .lutSamples = 32};
  auto first = loadOrBakeEnvironment(image, s, directory),
       second = loadOrBakeEnvironment(image, s, directory);
  require(!first.cacheHit && first.cacheStored && second.cacheHit &&
              first.data.cubeRgba == second.data.cubeRgba &&
              first.data.brdfLut.rgba == second.data.brdfLut.rgba &&
              first.data.sh == second.data.sh,
          "IBL cache did not preserve exact bake");
  auto originalFile = std::filesystem::directory_iterator(directory)->path();
  for (unsigned parameter = 0; parameter < 4; ++parameter) {
    auto changed = s;
    switch (parameter) {
    case 0:
      changed.faceSize *= 2;
      break;
    case 1:
      ++changed.prefilterSamples;
      break;
    case 2:
      ++changed.lutSize;
      break;
    case 3:
      ++changed.lutSamples;
      break;
    }
    require(!loadOrBakeEnvironment(image, changed, directory).cacheHit,
            "Changed IBL setting reused stale cache");
  }
  auto reshaped = image;
  reshaped.width = 4;
  reshaped.height = 8;
  require(!loadOrBakeEnvironment(reshaped, s, directory).cacheHit,
          "Changed source dimensions reused stale cache");
  image.rgba[0] += 1;
  require(!loadOrBakeEnvironment(image, s, directory).cacheHit,
          "Edited source reused stale cache");
  image.rgba[0] -= 1;
  // A valid file placed at another hash's path must still fail exact identity.
  // Force identity mismatch without changing total size/header or payload hash.
  {
    std::fstream file(originalFile,
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(32);
    file.put(char(99));
  }
  require(!loadOrBakeEnvironment(image, s, directory).cacheHit,
          "Hash alias bypassed exact cache key comparison");
  {
    std::fstream file(originalFile,
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(-1, std::ios::end);
    file.put(char(99));
  }
  require(!loadOrBakeEnvironment(image, s, directory).cacheHit,
          "Corrupt cache bypassed payload checksum");
  std::filesystem::resize_file(originalFile, 12);
  require(!loadOrBakeEnvironment(image, s, directory).cacheHit,
          "Truncated cache was accepted");
  auto blocked = directory / "not-a-directory";
  {
    std::ofstream file(blocked);
    file << "blocked";
  }
  auto fallback = loadOrBakeEnvironment(image, s, blocked);
  require(!fallback.cacheHit && !fallback.cacheStored &&
              fallback.data.cubeRgba == first.data.cubeRgba,
          "Unwritable cache changed/blocked rendering");
  auto parallel = directory / "parallel";
  auto a = std::async(std::launch::async, [&] {
    return loadOrBakeEnvironment(image, s, parallel);
  });
  auto b = std::async(std::launch::async, [&] {
    return loadOrBakeEnvironment(image, s, parallel);
  });
  require(a.get().data.cubeRgba == first.data.cubeRgba &&
              b.get().data.cubeRgba == first.data.cubeRgba &&
              loadOrBakeEnvironment(image, s, parallel).cacheHit,
          "Concurrent cache publication left an incomplete file");
  for (auto const &file : std::filesystem::directory_iterator(parallel))
    require(file.path().extension() == ".ibl",
            "Temporary IBL cache directory leaked");
  image.rgba[0] = std::numeric_limits<float>::infinity();
  bool rejected = false;
  try {
    loadOrBakeEnvironment(image, s, directory);
  } catch (std::runtime_error const &) {
    rejected = true;
  }
  require(rejected, "IBL cache accepted invalid source radiance");
  std::cout << "PASS: cache bit identity, all parameters, source edit, hash "
               "alias, checksum/truncation, fallback, concurrent publication\n";
}
} // namespace
int main() {
  try {
    numerics();
    cache();
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
