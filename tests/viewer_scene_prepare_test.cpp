#include "viewer_scene_prepare.hpp"
#include <iostream>
#include <stdexcept>
void check(bool b, char const *s) {
  if (!b)
    throw std::runtime_error(s);
}
int main() {
  try {
    ImportedScene s;
    Material m;
    m.doubleSided = true;
    s.materials.push_back(m);
    Mesh mesh;
    for (auto p : {glm::vec3{0, 0, 0}, glm::vec3{1, 0, 0}, glm::vec3{0, 1, 0}})
      mesh.vertices.push_back(
          Vertex{.position = p, .color = {1, 1, 1}, .normal = {0, 0, 1}});
    for (auto p : {glm::vec3{0, 0, 0}, glm::vec3{0, 1, 0}, glm::vec3{1, 0, 0}})
      mesh.vertices.push_back(
          Vertex{.position = p, .color = {1, 1, 1}, .normal = {0, 0, -1}});
    mesh.indices = {0, 1, 2, 3, 4, 5, 0, 0, 1};
    s.meshes.push_back(mesh);
    s.objects.push_back(SceneObject{});
    auto result = prepareImportedSceneForViewer(s);
    check(result.zeroArea == 1 && result.duplicates == 1 &&
              s.meshes[0].indices.size() == 3,
          "Viewer retains degenerate/redundant opposite faces");
    check(s.meshes[0].vertices.size() == 6 && s.objects.size() == 1,
          "Viewer changed identity/vertex data");
    auto again = prepareImportedSceneForViewer(s);
    check(again.zeroArea == 0 && again.duplicates == 0,
          "Viewer cleanup is not idempotent");
    auto pair = [&](Material material) {
      ImportedScene x;
      x.materials.push_back(material);
      x.meshes.push_back(mesh);
      x.meshes[0].indices = {0, 1, 2, 3, 4, 5};
      x.objects.push_back(SceneObject{});
      return x;
    };
    Material single;
    single.doubleSided = false;
    auto a = pair(single);
    check(prepareImportedSceneForViewer(a).duplicates == 0,
          "Single-sided opposite surface incorrectly removed");
    Material transparent = m;
    transparent.alphaMode = AlphaMode::Blend;
    auto b = pair(transparent);
    check(prepareImportedSceneForViewer(b).duplicates == 0,
          "Transparent layers incorrectly removed");
    Material textured = m;
    textured.albedoPath = "intentional-decal.png";
    auto c = pair(textured);
    check(prepareImportedSceneForViewer(c).duplicates == 0,
          "Textured overlap incorrectly removed");
    auto wrong = pair(m);
    for (unsigned i = 3; i < 6; ++i)
      wrong.meshes[0].vertices[i].normal = {0, 0, 1};
    check(prepareImportedSceneForViewer(wrong).duplicates == 0,
          "Non-equivalent normal/winding surface removed");
    auto lights = pair(m);
    lights.materials[0].sourceAreaLight = true;
    auto repair = prepareImportedSceneForViewer(lights);
    check(repair.lightCards == 1 && !lights.objects[0].primaryVisible &&
              !lights.objects[0].shadowCaster,
          "Light-card pass visibility not isolated");
    check(prepareImportedSceneForViewer(lights).lightCards == 0,
          "Repeated preparation changed light-card state");
    auto ordinary = pair(m);
    ordinary.materials[0].emissiveFactor = {1, 1, 1};
    prepareImportedSceneForViewer(ordinary);
    check(ordinary.objects[0].primaryVisible &&
              ordinary.objects[0].shadowCaster,
          "Ordinary emissive geometry hidden");
    std::cout << "Viewer scene preparation semantic checks passed\n";
    return 0;
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
