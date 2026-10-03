#include "gltf_loader.hpp"
#include <cmath>
#include <iostream>

bool close(glm::mat4 const &a, glm::mat4 const &b) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      if (std::abs(a[c][r] - b[c][r]) > 0.00001f)
        return false;
  return true;
}
int main() {
  auto scene = loadStaticGltfScene(TEST_FIXTURE, {});
  bool failed = false;
  auto const &material = scene.materials.at(0);
  TextureSamplerDescription expectedSampler{.mag = TextureFilter::Nearest,
                                            .min = TextureFilter::Linear,
                                            .mip = TextureMipFilter::Linear,
                                            .u = TextureWrap::ClampToEdge,
                                            .v = TextureWrap::MirroredRepeat};
  TextureSamplerDescription expectedPacked{.mag = TextureFilter::Linear,
                                           .min = TextureFilter::Nearest,
                                           .mip = TextureMipFilter::None};
  if (material.albedoSampler != expectedSampler ||
      material.normalSampler != expectedSampler ||
      material.metallicRoughnessSampler != expectedPacked) {
    std::cerr << "glTF per-texture sampler/filter/wrap semantics were lost\n";
    failed = true;
  }
  auto const &v = scene.meshes.at(0).vertices.at(0);
  if (glm::length(v.uv - glm::vec2{0.1f, 0.2f}) > 0.00001f) {
    std::cerr << "Base color selected TEXCOORD_0 instead of TEXCOORD_5\n";
    failed = true;
  }
  glm::mat4 parent = glm::translate(glm::mat4{1}, glm::vec3{2, 3, 4}) *
                     glm::rotate(glm::mat4{1}, 0.6f, glm::vec3{0, 0, 1}) *
                     glm::scale(glm::mat4{1}, glm::vec3{2, 1, 0.5f});
  glm::mat4 expected =
      parent * glm::rotate(glm::mat4{1}, 0.4f, glm::vec3{1, 0, 0});
  if (!close(scene.objects.at(0).transform.matrix(), expected)) {
    std::cerr << "Nested rotation/nonuniform scale changed the imported world "
                 "matrix\n";
    failed = true;
  }
  if (!close(scene.objects.at(1).transform.matrix(),
             glm::translate(glm::mat4{1}, glm::vec3{5, 6, 7}))) {
    std::cerr << "glTF column-major matrix translation was transposed\n";
    failed = true;
  }
  auto const fixtureDir = std::filesystem::path{TEST_FIXTURE}.parent_path();
  auto const mixed = loadStaticGltfScene(fixtureDir / "uv_channels.gltf", {});
  auto const &mixedVertex = mixed.meshes.at(0).vertices.at(0);
  if (glm::length(mixedVertex.uv - glm::vec2{0.1f, 0.2f}) > 0.00001f ||
      glm::length(mixedVertex.normalUv) > 0.00001f ||
      glm::length(mixedVertex.metallicRoughnessUv) > 0.00001f) {
    std::cerr << "Independent texture coordinate sets were merged\n";
    failed = true;
  }
  try {
    loadStaticGltfScene(fixtureDir / "uv_missing.gltf", {});
    std::cerr << "Missing material UV set was silently accepted\n";
    failed = true;
  } catch (std::runtime_error const &error) {
    if (std::string{error.what()}.find("TEXCOORD_5") == std::string::npos)
      throw;
  }
  for (auto const *name : {"sampler_missing.gltf", "sampler_bad_filter.gltf",
                           "sampler_bad_wrap.gltf"}) {
    try {
      loadStaticGltfScene(fixtureDir / name, {});
      std::cerr << "Invalid sampler was silently accepted: " << name << '\n';
      failed = true;
    } catch (std::runtime_error const &error) {
      if (std::string{error.what()}.find("sampler") == std::string::npos)
        throw;
    }
  }
  return failed ? 1 : 0;
}
