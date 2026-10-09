#pragma once
#include "mesh.hpp"
#include <string>
struct OpticalMaterial {
  bool enabled = false, solid = false;
  float ior = 1.5f, transmission = 1.f, thickness = 0;
  glm::vec3 absorption{0};
  unsigned coverage = 0; // Opaque=0, MASK=1, BLEND=2; independent of optics.
};
struct DielectricGeometry {
  bool solid = false;
  float boxThickness = 0;
  std::size_t components = 0, boundaryEdges = 0;
  std::string reason;
};
void validateOpticalMaterial(OpticalMaterial const &);
DielectricGeometry classifyDielectricMesh(Mesh const &);
// Analytic reference functions also used by authoring/content validation.
double dielectricFresnel(double cosine, double etaIncident,
                         double etaTransmitted);
glm::dvec3 dielectricBeer(glm::dvec3 coefficient, double distance);
