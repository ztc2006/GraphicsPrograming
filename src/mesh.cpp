#include "pch.hpp"

#include "mesh.hpp"

#include <cmath>

namespace {
glm::vec3 fallbackTangent(glm::vec3 normal) {
  glm::vec3 const axis =
      std::abs(normal.y) < 0.999f ? glm::vec3{0.0f, 1.0f, 0.0f}
                                  : glm::vec3{1.0f, 0.0f, 0.0f};
  return glm::normalize(glm::cross(axis, normal));
}
} // namespace

void generateMeshTangents(Mesh &mesh) {
  std::vector<glm::vec3> tangents(mesh.vertices.size(), glm::vec3{0.0f});
  std::vector<glm::vec3> bitangents(mesh.vertices.size(), glm::vec3{0.0f});

  for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
    std::uint32_t const i0 = mesh.indices[index + 0];
    std::uint32_t const i1 = mesh.indices[index + 1];
    std::uint32_t const i2 = mesh.indices[index + 2];
    if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() ||
        i2 >= mesh.vertices.size()) {
      continue;
    }

    Vertex const &v0 = mesh.vertices[i0];
    Vertex const &v1 = mesh.vertices[i1];
    Vertex const &v2 = mesh.vertices[i2];
    glm::vec3 const edge1 = v1.position - v0.position;
    glm::vec3 const edge2 = v2.position - v0.position;
    glm::vec2 const deltaUv1 = v1.uv - v0.uv;
    glm::vec2 const deltaUv2 = v2.uv - v0.uv;
    float const determinant =
        deltaUv1.x * deltaUv2.y - deltaUv1.y * deltaUv2.x;
    if (std::abs(determinant) <= 0.000001f) {
      continue;
    }

    float const inverseDeterminant = 1.0f / determinant;
    glm::vec3 const tangent =
        (edge1 * deltaUv2.y - edge2 * deltaUv1.y) * inverseDeterminant;
    glm::vec3 const bitangent =
        (edge2 * deltaUv1.x - edge1 * deltaUv2.x) * inverseDeterminant;
    tangents[i0] += tangent;
    tangents[i1] += tangent;
    tangents[i2] += tangent;
    bitangents[i0] += bitangent;
    bitangents[i1] += bitangent;
    bitangents[i2] += bitangent;
  }

  for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
    Vertex &vertex = mesh.vertices[index];
    glm::vec3 normal = vertex.normal;
    if (glm::length(normal) <= 0.0001f) {
      normal = {0.0f, 0.0f, 1.0f};
    } else {
      normal = glm::normalize(normal);
    }

    glm::vec3 tangent =
        tangents[index] - normal * glm::dot(normal, tangents[index]);
    if (glm::length(tangent) <= 0.0001f) {
      tangent = fallbackTangent(normal);
    } else {
      tangent = glm::normalize(tangent);
    }

    float const handedness =
        glm::dot(glm::cross(normal, tangent), bitangents[index]) < 0.0f ? -1.0f
                                                                       : 1.0f;
    vertex.tangent = glm::vec4{tangent, handedness};
  }
}
