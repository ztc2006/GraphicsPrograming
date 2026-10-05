#include "mesh.hpp"
#include "pch.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <mikktspace.h>
#include <stdexcept>
#include <unordered_map>

namespace {
glm::vec3 unitNormal(glm::vec3 n) {
  return glm::dot(n, n) > 1e-12f ? glm::normalize(n) : glm::vec3(0, 0, 1);
}
glm::vec3 fallbackTangent(glm::vec3 normal) {
  auto axis =
      std::abs(normal.y) < .999f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  return glm::normalize(glm::cross(axis, normal));
}
struct TangentInput {
  Mesh const &mesh;
  std::vector<glm::vec4> corners;
  Vertex const &vertex(int face, int corner) const {
    return mesh.vertices[mesh.indices[std::size_t(face) * 3 + corner]];
  }
};
TangentInput &input(SMikkTSpaceContext const *ctx) {
  return *static_cast<TangentInput *>(ctx->m_pUserData);
}
struct CornerKey {
  std::array<std::uint32_t, 5> bits;
  bool operator==(CornerKey const &) const = default;
};
struct CornerHash {
  std::size_t operator()(CornerKey const &key) const {
    std::size_t hash = 0;
    for (auto value : key.bits)
      hash ^= std::size_t(value) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};
} // namespace

void generateMeshTangents(Mesh &mesh, bool useNormalUv) {
  if (mesh.indices.size() % 3 ||
      mesh.indices.size() / 3 > std::numeric_limits<int>::max())
    throw std::runtime_error("Tangent generation requires indexed triangles.");
  for (auto index : mesh.indices)
    if (index >= mesh.vertices.size())
      throw std::runtime_error("Tangent triangle index is out of range.");
  for (auto &v : mesh.vertices) {
    if (!useNormalUv)
      v.normalUv = v.metallicRoughnessUv = v.occlusionUv = v.emissiveUv =
          v.specularUv = v.specularColorUv = v.uv;
    for (auto value : {v.position.x, v.position.y, v.position.z, v.normal.x,
                       v.normal.y, v.normal.z, v.normalUv.x, v.normalUv.y})
      if (!std::isfinite(value))
        throw std::runtime_error("Tangent source must be finite.");
    v.tangent = glm::vec4(fallbackTangent(unitNormal(v.normal)), 1);
  }
  if (mesh.indices.empty())
    return;
  TangentInput data{mesh, std::vector<glm::vec4>(mesh.indices.size())};
  SMikkTSpaceInterface interface{};
  interface.m_getNumFaces = [](SMikkTSpaceContext const *ctx) {
    return int(input(ctx).mesh.indices.size() / 3);
  };
  interface.m_getNumVerticesOfFace = [](SMikkTSpaceContext const *, int) {
    return 3;
  };
  interface.m_getPosition = [](SMikkTSpaceContext const *ctx, float out[],
                               int face, int corner) {
    auto v = input(ctx).vertex(face, corner).position;
    for (int c = 0; c < 3; ++c)
      out[c] = v[c];
  };
  interface.m_getNormal = [](SMikkTSpaceContext const *ctx, float out[],
                             int face, int corner) {
    auto v = unitNormal(input(ctx).vertex(face, corner).normal);
    for (int c = 0; c < 3; ++c)
      out[c] = v[c];
  };
  interface.m_getTexCoord = [](SMikkTSpaceContext const *ctx, float out[],
                               int face, int corner) {
    auto uv = input(ctx).vertex(face, corner).normalUv;
    out[0] = uv.x;
    out[1] = uv.y;
  };
  interface.m_setTSpaceBasic = [](SMikkTSpaceContext const *ctx,
                                  float const tangent[], float sign, int face,
                                  int corner) {
    auto &data = input(ctx);
    glm::vec3 t(tangent[0], tangent[1], tangent[2]);
    auto n = unitNormal(data.vertex(face, corner).normal);
    t -= n * glm::dot(n, t);
    if (!std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(t.z) ||
        glm::dot(t, t) < 1e-12f)
      t = fallbackTangent(n);
    else
      t = glm::normalize(t);
    data.corners[std::size_t(face) * 3 + corner] =
        glm::vec4(t, sign < 0 ? -1 : 1);
  };
  SMikkTSpaceContext context{&interface, &data};
  if (!genTangSpaceDefault(&context))
    throw std::runtime_error("MikkTSpace tangent generation failed.");
  std::vector<bool> assigned(mesh.vertices.size());
  std::unordered_map<CornerKey, std::uint32_t, CornerHash> remap;
  remap.reserve(mesh.indices.size());
  // Preserve original vertex order. Split only incompatible per-corner frames;
  // averaging mirrored signs would corrupt both triangles' normal maps.
  for (std::size_t corner = 0; corner < mesh.indices.size(); ++corner) {
    auto original = mesh.indices[corner];
    auto t = data.corners[corner];
    CornerKey key{{original}};
    for (int c = 0; c < 4; ++c)
      key.bits[c + 1] = std::bit_cast<std::uint32_t>(t[c] == 0 ? 0.0f : t[c]);
    auto found = remap.find(key);
    if (found != remap.end()) {
      mesh.indices[corner] = found->second;
      continue;
    }
    std::uint32_t index = original;
    if (assigned[original]) {
      if (mesh.vertices.size() >= std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Tangent seam vertex count overflows.");
      index = static_cast<std::uint32_t>(mesh.vertices.size());
      Vertex copy = mesh.vertices[original];
      mesh.vertices.push_back(copy);
    }
    assigned[original] = true;
    mesh.vertices[index].tangent = t;
    remap.emplace(key, index);
    mesh.indices[corner] = index;
  }
}
