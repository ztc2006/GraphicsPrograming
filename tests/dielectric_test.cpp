#include "dielectric.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value, char const *why) {
  if (!value)
    throw std::runtime_error(why);
}
Mesh box(float thickness = 1) {
  Mesh m;
  for (auto p :
       {glm::vec3{-1, -1, -thickness / 2}, glm::vec3{1, -1, -thickness / 2},
        glm::vec3{1, 1, -thickness / 2}, glm::vec3{-1, 1, -thickness / 2},
        glm::vec3{-1, -1, thickness / 2}, glm::vec3{1, -1, thickness / 2},
        glm::vec3{1, 1, thickness / 2}, glm::vec3{-1, 1, thickness / 2}})
    m.vertices.push_back(Vertex{.position = p});
  m.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
               1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
  return m;
}
} // namespace
int main() {
  try {
    require(std::abs(dielectricFresnel(1, 1, 1.5) - .04) < 1e-12,
            "Normal incidence Fresnel is not .04");
    require(dielectricFresnel(1, 1, 1) == 0, "Matched IOR reflected light");
    require(dielectricFresnel(std::cos(.8), 1.5, 1) == 1,
            "TIR failed past critical angle");
    require(dielectricFresnel(std::cos(.5), 1.5, 1) < 1,
            "Subcritical exit incorrectly TIR");
    auto beer = dielectricBeer({1, 2, 3}, .5);
    require(glm::length(beer - glm::dvec3(std::exp(-.5), std::exp(-1),
                                          std::exp(-1.5))) < 1e-12,
            "Beer distance law incorrect");
    auto m = box(.001f);
    auto closed = classifyDielectricMesh(m);
    require(closed.solid && closed.components == 1 &&
                std::abs(closed.boxThickness - .001f) < 1e-7,
            "Millimeter closed box failed validation");
    auto open = m;
    open.indices.resize(open.indices.size() - 3);
    require(!classifyDielectricMesh(open).solid,
            "Open boundary accepted as volume");
    auto reversed = m;
    for (unsigned i = 0; i < reversed.indices.size(); i += 3)
      std::swap(reversed.indices[i], reversed.indices[i + 1]);
    require(!classifyDielectricMesh(reversed).solid,
            "Reversed boundary accepted as volume");
    auto seam = m;
    for (auto id : m.indices)
      seam.vertices.push_back(m.vertices[id]);
    seam.indices.clear();
    for (unsigned i = 8; i < seam.vertices.size(); ++i)
      seam.indices.push_back(i);
    require(classifyDielectricMesh(seam).solid,
            "Exact geometric seam welding failed");
    auto two = m;
    auto other = box();
    for (auto v : other.vertices) {
      v.position.x += 5;
      two.vertices.push_back(v);
    }
    for (auto id : other.indices)
      two.indices.push_back(id + 8);
    require(classifyDielectricMesh(two).solid &&
                classifyDielectricMesh(two).components == 2,
            "Separated components failed validation");
    OpticalMaterial valid;
    valid.enabled = true;
    validateOpticalMaterial(valid);
    valid.ior = .5;
    bool rejected = false;
    try {
      validateOpticalMaterial(valid);
    } catch (...) {
      rejected = true;
    }
    require(rejected, "Invalid IOR accepted");
    std::cout << "PASS dielectric: Fresnel/TIR/Beer, millimeter "
                 "box/open/reversed/seams/components/invalid\n";
    return 0;
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
