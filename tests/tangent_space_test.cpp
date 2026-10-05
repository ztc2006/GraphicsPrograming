#include "mesh.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool ok, char const *message) {
  if (!ok)
    throw std::runtime_error(message);
}
Mesh quad() {
  Mesh mesh;
  for (auto position : {glm::vec3(0, 0, 0), glm::vec3(1, 0, 0),
                        glm::vec3(0, 1, 0), glm::vec3(1, 1, 0)}) {
    Vertex v{};
    v.position = position;
    v.normal = {0, 0, 1};
    v.color = {1, 1, 1};
    v.uv = {position.x, position.y};
    v.normalUv = v.uv;
    v.metallicRoughnessUv = {.2f, .7f};
    v.alpha = .3f;
    mesh.vertices.push_back(v);
  }
  mesh.indices = {0, 1, 2, 1, 3, 2};
  return mesh;
}
void validFrames(Mesh const &mesh) {
  for (auto const &v : mesh.vertices) {
    glm::vec3 t(v.tangent);
    require(std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z) &&
                std::abs(glm::length(t) - 1) < 1e-5f &&
                std::abs(glm::dot(glm::normalize(v.normal), t)) < 1e-5f &&
                std::abs(v.tangent.w) == 1,
            "Generated tangent frame is not finite/orthonormal");
  }
}
int main() {
  try {
    auto ordinary = quad();
    generateMeshTangents(ordinary, true);
    require(ordinary.vertices.size() == 4 &&
                ordinary.indices ==
                    std::vector<std::uint32_t>({0, 1, 2, 1, 3, 2}),
            "Compatible corners unnecessarily split/reordered");
    for (auto const &v : ordinary.vertices)
      require(glm::length(v.tangent - glm::vec4(1, 0, 0, 1)) < 1e-5f &&
                  v.metallicRoughnessUv == glm::vec2(.2f, .7f) &&
                  v.alpha == .3f,
              "Mikk frame or independent attributes lost");
    auto mirrored = quad();
    mirrored.vertices[3].normalUv = {0, 0};
    generateMeshTangents(mirrored, true);
    require(mirrored.vertices.size() == 6 &&
                mirrored.indices[1] != mirrored.indices[3] &&
                mirrored.indices[2] != mirrored.indices[5],
            "Mirrored UV corners were averaged instead of split");
    require(mirrored.vertices[mirrored.indices[0]].tangent.w == 1 &&
                mirrored.vertices[mirrored.indices[3]].tangent.w == -1,
            "Mirrored handedness lost");
    validFrames(mirrored);
    auto rotated = quad();
    for (auto &v : rotated.vertices)
      v.normalUv = {-v.uv.y, v.uv.x};
    generateMeshTangents(rotated, true);
    require(glm::length(rotated.vertices[0].tangent - glm::vec4(0, -1, 0, 1)) <
                1e-5f,
            "Generated frame used base UV instead of normal-map UV");
    auto degenerate = quad();
    for (auto &v : degenerate.vertices) {
      v.normal = {1, 0, 0};
      v.normalUv = {0, 0};
    }
    generateMeshTangents(degenerate, true);
    validFrames(degenerate);
    auto collapsed = quad();
    for (auto &v : collapsed.vertices)
      v.position = {0, 0, 0};
    generateMeshTangents(collapsed, true);
    validFrames(collapsed);
    auto obj = quad();
    generateMeshTangents(obj);
    require(obj.vertices[3].normalUv == obj.vertices[3].uv &&
                obj.vertices[3].emissiveUv == obj.vertices[3].uv,
            "OBJ UV defaults lost");
    obj.indices[0] = 99;
    bool rejected = false;
    try {
      generateMeshTangents(obj);
    } catch (std::runtime_error const &) {
      rejected = true;
    }
    require(rejected, "Invalid tangent triangle accepted");
    std::cout << "PASS MikkTSpace: "
                 "compatible/mirrored/normal-UV/degenerate/OBJ frames\n";
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
