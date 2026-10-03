#include <algorithm>
#include <filesystem>
#include <iostream>

#include <glm/common.hpp>

#include "gltf_loader.hpp"
#include "material_pipeline.hpp"
#include "texture.hpp"

int main() {
  std::filesystem::path const scenePath =
      std::filesystem::path{TEST_ASSET_DIR} / "scene.gltf";
  ImportedScene const scene = loadStaticGltfScene(scenePath, {});

  if (scene.materials.size() != 2) {
    std::cerr << "Expected two fixture materials, got "
              << scene.materials.size() << '\n';
    return 1;
  }
  if (!scene.materials[0].doubleSided) {
    std::cerr << "glTF doubleSided=true was not preserved\n";
    return 1;
  }
  if (scene.materials[1].doubleSided) {
    std::cerr << "glTF doubleSided=false was not preserved\n";
    return 1;
  }
  Material const &pbr = scene.materials[0];
  if (pbr.metallicFactor != 0.25f || pbr.roughnessFactor != 0.75f) {
    std::cerr << "glTF metallic/roughness factors were not preserved\n";
    return 1;
  }
  if (pbr.metallicRoughnessPath.empty() || pbr.occlusionPath.empty() ||
      pbr.emissivePath.empty()) {
    std::cerr << "glTF PBR texture references were not preserved\n";
    return 1;
  }
  if (pbr.occlusionStrength != 0.6f ||
      glm::any(glm::notEqual(pbr.emissiveFactor,
                             glm::vec3{0.1f, 0.2f, 0.3f}))) {
    std::cerr << "glTF occlusion/emissive factors were not preserved\n";
    return 1;
  }
  if (scene.materials[1].metallicFactor != 1.0f ||
      scene.materials[1].roughnessFactor != 1.0f) {
    std::cerr << "glTF metallic/roughness defaults are incorrect\n";
    return 1;
  }
  if (textureFormat(TextureColorSpace::Srgb) != vk::Format::eR8G8B8A8Srgb ||
      textureFormat(TextureColorSpace::Linear) !=
          vk::Format::eR8G8B8A8Unorm) {
    std::cerr << "Texture color-space formats are incorrect\n";
    return 1;
  }
  if (selectMaterialPipeline(scene.materials[0].alphaMode,
                             scene.materials[0].doubleSided,
                             RasterPass::Main) !=
      MaterialPipelineVariant::OpaqueDoubleSided) {
    std::cerr << "Double-sided material selected the wrong main pipeline\n";
    return 1;
  }
  if (selectMaterialPipeline(scene.materials[0].alphaMode,
                             scene.materials[0].doubleSided,
                             RasterPass::Shadow) !=
      MaterialPipelineVariant::ShadowDoubleSided) {
    std::cerr << "Double-sided material selected the wrong shadow pipeline\n";
    return 1;
  }
  if (selectMaterialPipeline(scene.materials[1].alphaMode,
                             scene.materials[1].doubleSided,
                             RasterPass::Main) !=
      MaterialPipelineVariant::OpaqueSingleSided) {
    std::cerr << "Single-sided material selected the wrong main pipeline\n";
    return 1;
  }

  ImportedScene const siheyuan =
      loadStaticGlbScene(TEST_SIHEYUAN_PATH, {});
  if (siheyuan.materials.at(0).albedoBytes.empty() ||
      siheyuan.materials.at(0).normalBytes.empty() ||
      siheyuan.materials.at(0).metallicRoughnessBytes.empty()) {
    std::cerr << "Embedded GLB images were replaced by guessed external files\n";
    return 1;
  }
  if (siheyuan.materials.at(9).albedoTexCoord != 5 ||
      siheyuan.materials.at(9).normalTexCoord != 5 ||
      siheyuan.materials.at(9).metallicRoughnessTexCoord != 5) {
    std::cerr << "siheyuan material TEXCOORD_5 selection was lost\n";
    return 1;
  }
  std::size_t const packedMaterialCount = std::count_if(
      siheyuan.materials.begin(), siheyuan.materials.end(),
      [](Material const &material) {
        return !material.metallicRoughnessPath.empty() ||
               !material.metallicRoughnessBytes.empty();
      });
  if (siheyuan.materials.size() != 27 || packedMaterialCount < 1) {
    std::cerr << "siheyuan GLB PBR material import is incomplete\n";
    return 1;
  }

  return 0;
}
