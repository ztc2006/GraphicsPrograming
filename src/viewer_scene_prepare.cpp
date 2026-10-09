#include "viewer_scene_prepare.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <unordered_map>
namespace {
using Key = std::array<std::uint32_t, 9>;
struct Hash {
  std::size_t operator()(Key const &k) const {
    std::size_t h = 0;
    for (auto v : k)
      h ^= std::size_t(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};
using Corners = std::array<std::uint32_t, 3>;
struct Face {
  Corners corners;
  glm::dvec3 geometry;
};
bool plain(Material const &m) {
  return m.doubleSided && m.alphaMode == AlphaMode::Opaque &&
         m.albedoPath.empty() && m.albedoBytes.empty() &&
         m.normalPath.empty() && m.normalBytes.empty() &&
         m.metallicRoughnessPath.empty() && m.metallicRoughnessBytes.empty() &&
         m.occlusionPath.empty() && m.occlusionBytes.empty() &&
         m.emissivePath.empty() && m.emissiveBytes.empty() &&
         m.specularPath.empty() && m.specularBytes.empty() &&
         m.specularColorPath.empty() && m.specularColorBytes.empty() &&
         m.alphaPath.empty() && m.heightPath.empty();
}
bool positionLess(glm::vec3 a, glm::vec3 b) {
  for (unsigned i = 0; i < 3; ++i) {
    if (a[i] < b[i])
      return true;
    if (a[i] > b[i])
      return false;
  }
  return false;
}
Key key(Mesh const &m, Corners &c) {
  std::sort(c.begin(), c.end(), [&](auto a, auto b) {
    return positionLess(m.vertices[a].position, m.vertices[b].position);
  });
  Key k;
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned j = 0; j < 3; ++j) {
      float v = m.vertices[c[i]].position[j];
      k[3 * i + j] = std::bit_cast<std::uint32_t>(v == 0 ? 0.f : v);
    }
  return k;
}
} // namespace
ViewerSceneRepair prepareImportedSceneForViewer(ImportedScene &scene) {
  ViewerSceneRepair result;
  std::vector<std::optional<MaterialId>> material(scene.meshes.size());
  std::vector<bool> conflicting(scene.meshes.size());
  for (auto &o : scene.objects) {
    if (o.meshId >= scene.meshes.size() ||
        o.materialId >= scene.materials.size())
      throw std::runtime_error(
          "Viewer object references invalid geometry/material");
    auto &m = material[o.meshId];
    if (m && *m != o.materialId)
      conflicting[o.meshId] = true;
    else
      m = o.materialId;
    if (scene.materials[o.materialId].sourceAreaLight) {
      if (o.primaryVisible || o.shadowCaster)
        ++result.lightCards;
      o.primaryVisible = false;
      o.shadowCaster = false;
    }
  }
  for (unsigned mi = 0; mi < scene.meshes.size(); ++mi) {
    auto &mesh = scene.meshes[mi];
    if (mesh.indices.size() % 3)
      throw std::runtime_error("Viewer mesh has incomplete triangle");
    bool dedup = material[mi] && !conflicting[mi] &&
                 plain(scene.materials[*material[mi]]);
    std::unordered_map<Key, std::vector<Face>, Hash> seen;
    std::vector<std::uint32_t> kept;
    kept.reserve(mesh.indices.size());
    std::size_t zero = 0, duplicates = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
      Corners c{mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
      for (auto id : c)
        if (id >= mesh.vertices.size())
          throw std::runtime_error("Viewer triangle index out of bounds");
      auto cross = glm::cross(glm::dvec3(mesh.vertices[c[1]].position) -
                                  glm::dvec3(mesh.vertices[c[0]].position),
                              glm::dvec3(mesh.vertices[c[2]].position) -
                                  glm::dvec3(mesh.vertices[c[0]].position));
      if (glm::dot(cross, cross) == 0) {
        ++zero;
        continue;
      }
      bool redundant = false;
      if (dedup) {
        auto sorted = c;
        auto k = key(mesh, sorted);
        auto &matches = seen[k];
        for (auto const &old : matches) {
          bool same = true, opposite = true;
          for (unsigned corner = 0; corner < 3; ++corner) {
            auto const &a = mesh.vertices[old.corners[corner]],
                       &b = mesh.vertices[sorted[corner]];
            if (a.color != b.color || a.alpha != b.alpha) {
              same = opposite = false;
              break;
            }
            auto an = glm::normalize(a.normal), bn = glm::normalize(b.normal);
            same &= glm::length(an - bn) < 1e-6f;
            opposite &= glm::length(an + bn) < 1e-6f;
          }
          if ((glm::dot(old.geometry, cross) > 0 && same) ||
              (glm::dot(old.geometry, cross) < 0 && opposite)) {
            redundant = true;
            break;
          }
        }
        if (!redundant)
          matches.push_back({sorted, cross});
      }
      if (redundant) {
        ++duplicates;
        continue;
      }
      kept.insert(kept.end(), c.begin(), c.end());
    }
    // Keep an all-degenerate primitive intact; the importer/GPU preparation
    // already owns its existing empty-geometry contract.
    if (!kept.empty()) {
      mesh.indices = std::move(kept);
      result.zeroArea += zero;
      result.duplicates += duplicates;
    }
  }
  if (result.zeroArea || result.duplicates || result.lightCards)
    scene.warnings.push_back(
        "Viewer preparation: removed " + std::to_string(result.zeroArea) +
        " zero-area and " + std::to_string(result.duplicates) +
        " equivalent triangles; " + std::to_string(result.lightCards) +
        " imported area-light cards retained for probe capture, hidden from "
        "primary camera/shadows.");
  std::vector<std::optional<DielectricGeometry>> opticalGeometry(scene.meshes.size());
  for(auto const &object:scene.objects){auto const &material=scene.materials[object.materialId];
    if(!material.optical.enabled || !material.optical.solid)continue;
    auto &geometry=opticalGeometry[object.meshId];
    if(!geometry){geometry=classifyDielectricMesh(scene.meshes[object.meshId]);
      if(!geometry->solid)scene.warnings.push_back("Dielectric mesh "+std::to_string(object.meshId)+": thin-sheet fallback; "+geometry->reason+". No holes filled or thickness invented.");}
  }
  return result;
}
