#include "dielectric.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
void validateOpticalMaterial(OpticalMaterial const &m) {
  if (!std::isfinite(m.ior) || m.ior < 1 || !std::isfinite(m.transmission) ||
      m.transmission < 0 || m.transmission > 1 || !std::isfinite(m.thickness) ||
      m.thickness < 0 || m.coverage > 2)
    throw std::runtime_error(
        "Invalid dielectric IOR/transmission/thickness/coverage");
  for (unsigned i = 0; i < 3; ++i)
    if (!std::isfinite(m.absorption[i]) || m.absorption[i] < 0)
      throw std::runtime_error("Invalid dielectric absorption coefficient");
}
double dielectricFresnel(double c, double a, double b) {
  if (a == b)
    return 0;
  c = std::clamp(std::abs(c), 0., 1.);
  double st2 = (a * a / (b * b)) * (1 - c * c);
  if (st2 >= 1)
    return 1;
  double t = std::sqrt(1 - st2);
  double rs = (a * c - b * t) / (a * c + b * t),
         rp = (b * c - a * t) / (b * c + a * t);
  return .5 * (rs * rs + rp * rp);
}
glm::dvec3 dielectricBeer(glm::dvec3 s, double d) { return glm::exp(-s * d); }
DielectricGeometry classifyDielectricMesh(Mesh const &m) {
  DielectricGeometry result;
  if (m.indices.empty() || m.indices.size() % 3) {
    result.reason = "non-triangle/empty boundary";
    return result;
  }
  using Position = std::array<std::uint32_t, 3>;
  std::map<Position, unsigned> weld;
  std::vector<glm::dvec3> positions;
  std::vector<unsigned> vertexIds;
  glm::vec3 lo(INFINITY), hi(-INFINITY);
  for (auto const &v : m.vertices) {
    Position key;
    for (unsigned a = 0; a < 3; ++a) {
      if (!std::isfinite(v.position[a])) {
        result.reason = "nonfinite boundary";
        return result;
      }
      key[a] = std::bit_cast<std::uint32_t>(v.position[a] == 0 ? 0.f
                                                               : v.position[a]);
    }
    auto [it, inserted] = weld.emplace(key, unsigned(positions.size()));
    if (inserted)
      positions.push_back(glm::dvec3(v.position));
    vertexIds.push_back(it->second);
    lo = glm::min(lo, v.position);
    hi = glm::max(hi, v.position);
  }
  result.boxThickness =
      glm::min(glm::min(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
  struct Face {
    std::array<unsigned, 3> ids;
  };
  std::vector<Face> faces;
  for (std::size_t i = 0; i < m.indices.size(); i += 3) {
    std::array<unsigned, 3> ids;
    for (unsigned j = 0; j < 3; ++j) {
      if (m.indices[i + j] >= vertexIds.size()) {
        result.reason = "out-of-range boundary";
        return result;
      }
      ids[j] = vertexIds[m.indices[i + j]];
    }
    if (ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[0])
      continue;
    auto n = glm::cross(positions[ids[1]] - positions[ids[0]],
                        positions[ids[2]] - positions[ids[0]]);
    if (glm::dot(n, n) == 0)
      continue;
    faces.push_back({ids});
  }
  if (faces.empty()) {
    result.reason = "zero-area boundary";
    return result;
  }
  std::vector<unsigned> parent(faces.size());
  std::iota(parent.begin(), parent.end(), 0);
  auto find = [&](unsigned id) {
    while (parent[id] != id) {
      parent[id] = parent[parent[id]];
      id = parent[id];
    }
    return id;
  };
  struct Edge {
    unsigned count = 0, first = 0;
    int direction = 0;
  };
  std::map<std::pair<unsigned, unsigned>, Edge> edges;
  for (unsigned i = 0; i < faces.size(); ++i)
    for (unsigned j = 0; j < 3; ++j) {
      auto a = faces[i].ids[j], b = faces[i].ids[(j + 1) % 3];
      auto &edge = edges[{std::min(a, b), std::max(a, b)}];
      if (edge.count)
        parent[find(i)] = find(edge.first);
      else
        edge.first = i;
      ++edge.count;
      edge.direction += a < b ? 1 : -1;
    }
  bool inconsistent = false;
  for (auto const &[key, e] : edges) {
    if (e.count != 2)
      ++result.boundaryEdges;
    if (e.count == 2 && e.direction != 0)
      inconsistent = true;
  }
  if (result.boundaryEdges) {
    result.reason = "open or non-manifold boundary";
    return result;
  }
  if (inconsistent) {
    result.reason = "inconsistent boundary orientation";
    return result;
  }
  auto center = glm::dvec3((lo + hi) * .5f);
  std::map<unsigned, double> volumes;
  for (unsigned i = 0; i < faces.size(); ++i) {
    auto const &f = faces[i];
    volumes[find(i)] += glm::dot(positions[f.ids[0]] - center,
                                 glm::cross(positions[f.ids[1]] - center,
                                            positions[f.ids[2]] - center)) /
                        6.;
  }
  result.components = volumes.size();
  double scale = std::max(1e-30, double(hi.x - lo.x) * double(hi.y - lo.y) *
                                     double(hi.z - lo.z));
  for (auto const &[id, v] : volumes)
    if (v <= scale * 1e-12) {
      result.reason = "reversed or zero-volume component";
      return result;
    }
  result.solid = true;
  result.reason = "closed oriented boundary (self-intersections not proven)";
  return result;
}
