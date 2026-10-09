#include "gltf_loader.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, char const *message) {
  if (!value)
    throw std::runtime_error(message);
}
bool near(float a, float b) { return std::abs(a - b) < 1e-5f; }
ImportedScene fixture(char const *name) {
  return loadStaticGltfScene(
      std::filesystem::path{TEST_ADAPTER_FIXTURES} / name, {});
}
} // namespace
int main() {
  try {
    auto scene = fixture("sparse_normalized_tangent.gltf");
    auto const &vertices = scene.meshes.at(0).vertices;
    require(vertices.size() == 3 && vertices[1].position.x == 1 &&
                vertices[2].position.y == 1,
            "Sparse POSITION without a base bufferView was lost");
    auto const &v = vertices.at(0);
    require(v.alpha == 1, "VEC3 color must default alpha to one");
    auto alphaScene = fixture("vertex_alpha.gltf");
    require(near(alphaScene.meshes[0].vertices[0].alpha, .2f) &&
                near(alphaScene.meshes[0].vertices[2].alpha, .8f),
            "VEC4 vertex alpha lost during import/tangent generation");
    require(near(v.normal.z, 1) && near(v.uv.x, 32768.0f / 65535.0f) &&
                near(v.uv.y, 1),
            "Normalized signed normal/unsigned UV conversion failed");
    require(near(v.color.r, 64.0f / 255) && near(v.color.g, 128.0f / 255),
            "Normalized vertex color conversion failed");
    require(v.tangent == glm::vec4(0, 1, 0, -1),
            "Authored tangent/handedness was overwritten by generated tangent");
    auto interleaved = fixture("interleaved_sparse.gltf");
    require(
        interleaved.meshes[0].vertices[1].position == glm::vec3(8, 9, 0) &&
            interleaved.meshes[0].vertices[2].position == glm::vec3(0, 1, 0),
        "Sparse replacement incorrectly inherited the interleaved base stride");
    auto sparseIndices = fixture("sparse_indices.gltf");
    require(sparseIndices.meshes[0].indices ==
                std::vector<std::uint32_t>{0, 1, 2},
            "Sparse 32-bit primitive indices failed");
    auto implicit = fixture("implicit_default.gltf");
    auto const &material =
        implicit.materials.at(implicit.objects[0].materialId);
    require(implicit.materials.size() == 2 && material.tint == glm::vec4(1) &&
                material.metallicFactor == 1,
            "Material-less primitive incorrectly used authored material zero");
    auto transformed = fixture("texture_transform.gltf");
    auto const &tv = transformed.meshes[0].vertices[0];
    require(
        transformed.materials[0].albedoTexCoord == 5 && near(tv.uv.x, -0.35f) &&
            near(tv.uv.y, -0.05f),
        "KHR_texture_transform override/scale/rotation/offset order failed");
    require(near(tv.normalUv.x, 0.1f) && near(tv.normalUv.y, 0.2f),
            "Base-color transform contaminated independent normal UV");
    auto independent = fixture("independent_uv_without_normal.gltf");
    auto const &iv = independent.meshes[0].vertices[0];
    require(near(iv.uv.x, 0.1f) && near(iv.uv.y, 0.2f) &&
                near(iv.metallicRoughnessUv.x, 0) &&
                near(iv.metallicRoughnessUv.y, 0),
            "Generated tangent without a normal map overwrote packed-map UV");
    auto optional = fixture("optional_extension.gltf");
    require(optional.warnings.size() == 1 &&
                optional.warnings[0].find("TEST_optional") != std::string::npos,
            "Optional unsupported extension fallback was silent");
    auto supported = fixture("required_specular.gltf");
    require(supported.warnings.empty(),
            "Supported required specular extension rejected/warned");
    auto spec = fixture("specular_semantics.gltf");
    auto const &sm = spec.materials[0];
    auto const &sv = spec.meshes[0].vertices[0];
    require(spec.warnings.empty() && near(sm.specularFactor, .35f) &&
                sm.specularColorFactor == glm::vec3(2, .5f, 30) &&
                !sm.specularPath.empty() && !sm.specularColorPath.empty(),
            "Specular factor/color greater than 1/texture references lost");
    require(sm.specularTexCoord == 5 && sm.specularColorTexCoord == 5 &&
                near(sv.specularUv.x, .1f) && near(sv.specularUv.y, .2f) &&
                near(sv.specularColorUv.x, -.35f) &&
                near(sv.specularColorUv.y, -.05f),
            "Independent specular UV or transform lost during Mikk generation");
    require(sm.specularSampler.mag == TextureFilter::Nearest &&
                sm.specularSampler.mip == TextureMipFilter::Linear &&
                sm.specularSampler.u == TextureWrap::ClampToEdge &&
                sm.specularSampler.v == TextureWrap::MirroredRepeat &&
                sm.specularColorSampler.min == TextureFilter::Nearest &&
                sm.specularColorSampler.mip == TextureMipFilter::None,
            "Independent specular samplers lost");
    auto defaults = fixture("specular_defaults.gltf");
    require(defaults.materials[0].specularFactor == 1 &&
                defaults.materials[0].specularColorFactor == glm::vec3(1) &&
                material.specularFactor == 1 &&
                material.specularColorFactor == glm::vec3(1),
            "Absent extension/extension field defaults changed core material");
    for (auto extension :
         {"punctual_semantics.gltf", "punctual_semantics.glb"}) {
      auto path = std::filesystem::path{TEST_ADAPTER_FIXTURES} / extension;
      auto lightsScene = path.extension() == ".glb"
                             ? loadStaticGlbScene(path, {})
                             : loadStaticGltfScene(path, {});
      auto const &ls = lightsScene.lights;
      require(lightsScene.warnings.empty() && ls.size() == 4 &&
                  ls[0].type == PunctualLightType::Directional &&
                  ls[1].type == PunctualLightType::Spot &&
                  ls[2].type == PunctualLightType::Point,
              "Selected-scene light instances/type/required extension failed");
      require(glm::length(ls[0].direction - glm::vec3(-1, 0, 0)) < 1e-5f &&
                  ls[1].position == glm::vec3(1, 2, 7) &&
                  ls[1].direction == glm::vec3(0, 0, -1) && ls[1].range == 12 &&
                  ls[1].intensity == 32 && near(ls[1].innerCone, .2f) &&
                  near(ls[1].outerCone, .6f),
              "World light transform scaled photometric units/range/cones or "
              "lost local -Z");
      require(ls[2].position == glm::vec3(4, 5, 6) &&
                  ls[3].position == glm::vec3(-4, -5, -6) &&
                  ls[2].color == glm::vec3(1, .25f, .5f) && ls[2].range == 8 &&
                  ls[2].intensity == 16,
              "Shared light definition was not instantiated per selected node");
    }
    auto ld = fixture("punctual_defaults.gltf").lights;
    require(ld.size() == 3 && ld[0].range == 0 && ld[0].intensity == 1 &&
                ld[0].color == glm::vec3(1) && ld[1].innerCone == 0 &&
                near(ld[1].outerCone, .78539816f) && ld[2].range == 0,
            "Absent punctual fields failed Khronos defaults");
    for (auto const *name : {"punctual_zero_range.gltf",
                             "punctual_negative_range.gltf",
                             "punctual_negative_intensity.gltf",
                             "punctual_bad_color.gltf",
                             "punctual_missing_spot.gltf",
                             "punctual_equal_cones.gltf",
                             "punctual_oversized_cone.gltf",
                             "punctual_degenerate_direction.gltf",
                             "required_extension.gltf",
                             "specular_bad_strength.gltf",
                             "specular_negative_color.gltf",
                             "specular_nonfinite_color.gltf",
                             "specular_missing_uv.gltf",
                             "specular_missing_image.gltf",
                             "specular_unlit.gltf",
                             "specular_specgloss.gltf",
                             "short_view.gltf",
                             "overflow_view.gltf",
                             "cycle.gltf",
                             "bad_sparse_index.gltf",
                             "duplicate_sparse_index.gltf",
                             "bad_triangle_index.gltf",
                             "bad_color_alpha.gltf",
                             "bad_color_rgb.gltf"}) {
      bool rejected = false;
      try {
        (void)fixture(name);
      } catch (std::runtime_error const &e) {
        require(std::string{e.what()}.starts_with("glTF:"),
                "Import error escaped adapter diagnostics");
        rejected = true;
      }
      require(rejected, name);
    }
    auto roles = fixture("area_light_metadata.gltf");
    require(roles.materials.size() == 6,
            "Area-light metadata material count changed");
    for (unsigned i = 0; i < 6; ++i)
      require(roles.materials[i].sourceAreaLight == (i == 0 || i == 5),
              "Light-card role confused metadata with emissive/string/invalid "
              "array");
    auto glass=fixture("dielectric_uniform.gltf");
    require(glass.materials[0].optical.enabled && glass.materials[0].optical.solid && near(glass.materials[0].optical.ior,1.33f) && near(glass.materials[0].optical.transmission,.8f),"Uniform dielectric factors lost");
    require(near(glass.materials[0].optical.absorption.x,float(-std::log(.5)/.5)) && near(glass.materials[0].optical.absorption.y,float(-std::log(.25)/.5)),"Attenuation semantics lost");
    require(glass.materials[1].optical.enabled && glass.materials[1].optical.coverage==0 && near(glass.materials[1].optical.ior,1.5f),"PBRT conversion alpha mistaken for optical coverage");
    require(!glass.materials[2].optical.enabled,"Plain alpha BLEND guessed as glass");
    bool invalidDielectric=false;try{fixture("dielectric_invalid.gltf");}catch(std::runtime_error const &){invalidDielectric=true;}
    require(invalidDielectric,"Invalid PBRT eta accepted");
    std::cout << "Adapter semantics: "
                 "sparse/interleaved/normalized/tangent/default/UV "
                 "transform/extension/error cases passed\n";
    return 0;
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
