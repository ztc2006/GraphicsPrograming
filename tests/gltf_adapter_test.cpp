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
    for (auto const *name :
         {"required_extension.gltf", "specular_bad_strength.gltf",
          "specular_negative_color.gltf", "specular_nonfinite_color.gltf",
          "specular_missing_uv.gltf", "specular_missing_image.gltf",
          "specular_unlit.gltf", "specular_specgloss.gltf", "short_view.gltf",
          "overflow_view.gltf", "cycle.gltf", "bad_sparse_index.gltf",
          "duplicate_sparse_index.gltf", "bad_triangle_index.gltf",
          "bad_color_alpha.gltf", "bad_color_rgb.gltf"}) {
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
    std::cout << "Adapter semantics: "
                 "sparse/interleaved/normalized/tangent/default/UV "
                 "transform/extension/error cases passed\n";
    return 0;
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
