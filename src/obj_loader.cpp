#include "pch.hpp"

#include "obj_loader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

#include <stb_image.h>
#include <tiny_obj_loader.h>

namespace {
constexpr int kAlphaMaskThreshold = 250;

struct MeshBuilder {
  Mesh mesh;
  MaterialId materialId = 0;
};

Aabb emptyBounds() {
  return {
      .min = glm::vec3{std::numeric_limits<float>::max()},
      .max = glm::vec3{std::numeric_limits<float>::lowest()},
      .valid = false,
  };
}

void includePoint(Aabb &bounds, glm::vec3 point) {
  bounds.min = glm::min(bounds.min, point);
  bounds.max = glm::max(bounds.max, point);
  bounds.valid = true;
}

std::string lowercase(std::string value) {
  for (char &c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

std::filesystem::path normalizeAssetPath(std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  return std::filesystem::path{value};
}

std::filesystem::path resolveCaseInsensitivePath(std::filesystem::path path) {
  if (std::filesystem::exists(path)) {
    return path;
  }

  std::filesystem::path const parent = path.parent_path();
  if (parent.empty() || !std::filesystem::exists(parent)) {
    return path;
  }

  std::string const wanted = lowercase(path.filename().string());
  for (std::filesystem::directory_entry const &entry :
       std::filesystem::directory_iterator(parent)) {
    if (lowercase(entry.path().filename().string()) == wanted) {
      return entry.path();
    }
  }
  return path;
}

std::string optionalMaterialTexturePath(std::filesystem::path const &objPath,
                                        tinyobj::material_t const &material,
                                        std::string const &textureName,
                                        char const *textureKind) {
  if (textureName.empty()) {
    return {};
  }

  std::filesystem::path texturePath =
      resolveCaseInsensitivePath(objPath.parent_path() /
                                 normalizeAssetPath(textureName));
  if (!std::filesystem::exists(texturePath)) {
    if (!material.name.empty()) {
      std::cerr << "OBJ material '" << material.name
                << "' missing " << textureKind << " texture '" << textureName
                << "'\n";
    } else {
      std::cerr << "OBJ material missing " << textureKind << " texture '"
                << textureName << "'\n";
    }
    return {};
  }
  return texturePath.string();
}

std::string materialAlbedoPath(std::filesystem::path const &objPath,
                               tinyobj::material_t const &material,
                               std::string const &fallbackAlbedoPath) {
  std::string path = optionalMaterialTexturePath(
      objPath, material, material.diffuse_texname, "diffuse");
  if (!path.empty()) {
    return path;
  }
  if (!material.diffuse_texname.empty()) {
    std::cerr << "Falling back to " << fallbackAlbedoPath << '\n';
  }
  return fallbackAlbedoPath;
}

bool hasTransparentPixels(std::filesystem::path const &path,
                          int alphaThreshold) {
  int width = 0;
  int height = 0;
  int channels = 0;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
      stbi_load(path.string().c_str(), &width, &height, &channels,
                STBI_rgb_alpha),
      stbi_image_free);
  if (!pixels || width <= 0 || height <= 0) {
    return false;
  }

  std::size_t const pixelCount =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  for (std::size_t index = 0; index < pixelCount; ++index) {
    if (pixels.get()[index * 4 + 3] < alphaThreshold) {
      return true;
    }
  }
  return false;
}

std::vector<Material>
loadMaterials(std::filesystem::path const &objPath,
              std::vector<tinyobj::material_t> const &objMaterials,
              std::string const &fallbackAlbedoPath) {
  std::vector<Material> materials;
  materials.reserve(std::max<std::size_t>(objMaterials.size(), 1));

  if (objMaterials.empty()) {
    materials.push_back(Material{
        .albedoPath = fallbackAlbedoPath,
        .tint = {1.0f, 1.0f, 1.0f, 1.0f},
    });
    return materials;
  }

  for (tinyobj::material_t const &objMaterial : objMaterials) {
    Material material{
        .albedoPath = materialAlbedoPath(objPath, objMaterial,
                                         fallbackAlbedoPath),
        .normalPath = optionalMaterialTexturePath(
            objPath, objMaterial, objMaterial.normal_texname, "normal"),
        .heightPath = optionalMaterialTexturePath(
            objPath, objMaterial, objMaterial.bump_texname, "height"),
        .alphaPath = optionalMaterialTexturePath(
            objPath, objMaterial, objMaterial.alpha_texname, "alpha mask"),
        .tint =
            {
                objMaterial.diffuse[0],
                objMaterial.diffuse[1],
                objMaterial.diffuse[2],
                objMaterial.dissolve,
            },
    };

    bool const hasMaskTexture = !material.alphaPath.empty();
    bool const diffuseHasAlpha =
        hasTransparentPixels(material.albedoPath, kAlphaMaskThreshold);
    if (hasMaskTexture || diffuseHasAlpha) {
      material.alphaMode = AlphaMode::Mask;
      material.alphaCutoff = 0.5f;
    } else if (objMaterial.dissolve < 0.999f) {
      material.alphaMode = AlphaMode::Blend;
    }

    materials.push_back(std::move(material));
  }
  return materials;
}

glm::vec3 readPosition(tinyobj::attrib_t const &attrib, int index) {
  if (index < 0) {
    throw std::runtime_error("OBJ face is missing a position index.");
  }
  std::size_t const base = static_cast<std::size_t>(index) * 3;
  return {
      attrib.vertices.at(base + 0),
      attrib.vertices.at(base + 1),
      attrib.vertices.at(base + 2),
  };
}

glm::vec3 readNormal(tinyobj::attrib_t const &attrib, int index,
                     glm::vec3 fallback) {
  if (index < 0 || attrib.normals.empty()) {
    return fallback;
  }
  std::size_t const base = static_cast<std::size_t>(index) * 3;
  return {
      attrib.normals.at(base + 0),
      attrib.normals.at(base + 1),
      attrib.normals.at(base + 2),
  };
}

glm::vec2 readUv(tinyobj::attrib_t const &attrib, int index) {
  if (index < 0 || attrib.texcoords.empty()) {
    return {0.0f, 0.0f};
  }
  std::size_t const base = static_cast<std::size_t>(index) * 2;
  return {
      attrib.texcoords.at(base + 0),
      1.0f - attrib.texcoords.at(base + 1),
  };
}

glm::vec3 fallbackNormal(glm::vec3 p0, glm::vec3 p1, glm::vec3 p2) {
  glm::vec3 normal = glm::cross(p1 - p0, p2 - p0);
  if (glm::length(normal) <= 0.0001f) {
    return {0.0f, 1.0f, 0.0f};
  }
  return glm::normalize(normal);
}

MaterialId resolveMaterialId(int materialIndex, std::size_t materialCount) {
  if (materialIndex < 0 ||
      static_cast<std::size_t>(materialIndex) >= materialCount) {
    return 0;
  }
  return static_cast<MaterialId>(materialIndex);
}

std::uint32_t appendVertex(MeshBuilder &builder,
                           tinyobj::attrib_t const &attrib,
                           tinyobj::index_t const &index,
                           glm::vec3 fallbackNormalValue) {
  glm::vec3 const position = readPosition(attrib, index.vertex_index);
  Vertex vertex{
      .position = position,
      .color = {1.0f, 1.0f, 1.0f},
      .normal = readNormal(attrib, index.normal_index, fallbackNormalValue),
      .uv = readUv(attrib, index.texcoord_index),
  };

  std::uint32_t const vertexIndex =
      static_cast<std::uint32_t>(builder.mesh.vertices.size());
  builder.mesh.vertices.push_back(vertex);
  builder.mesh.indices.push_back(vertexIndex);
  includePoint(builder.mesh.localBounds, position);
  return vertexIndex;
}

Aabb transformBounds(Aabb const &localBounds, glm::mat4 const &matrix) {
  if (!localBounds.valid) {
    return {};
  }

  Aabb world = emptyBounds();
  for (int x = 0; x < 2; ++x) {
    for (int y = 0; y < 2; ++y) {
      for (int z = 0; z < 2; ++z) {
        glm::vec3 corner{
            x == 0 ? localBounds.min.x : localBounds.max.x,
            y == 0 ? localBounds.min.y : localBounds.max.y,
            z == 0 ? localBounds.min.z : localBounds.max.z,
        };
        includePoint(world, glm::vec3(matrix * glm::vec4(corner, 1.0f)));
      }
    }
  }
  return world;
}

Aabb sceneBounds(std::vector<Mesh> const &meshes) {
  Aabb bounds = emptyBounds();
  for (Mesh const &mesh : meshes) {
    if (!mesh.localBounds.valid) {
      continue;
    }
    includePoint(bounds, mesh.localBounds.min);
    includePoint(bounds, mesh.localBounds.max);
  }
  return bounds;
}

Transform normalizedSceneTransform(Aabb const &bounds) {
  if (!bounds.valid) {
    return {};
  }

  glm::vec3 const center = (bounds.min + bounds.max) * 0.5f;
  glm::vec3 const extent = bounds.max - bounds.min;
  float const maxExtent = std::max({extent.x, extent.y, extent.z, 0.001f});
  float const scale = 5.0f / maxExtent;

  Transform transform{};
  transform.scale = {scale, scale, scale};
  transform.translation = -center * scale;
  return transform;
}
} // namespace

LoadedScene loadStaticObjScene(std::filesystem::path const &path,
                               std::string fallbackAlbedoPath) {
  tinyobj::attrib_t attrib;
  std::vector<tinyobj::shape_t> shapes;
  std::vector<tinyobj::material_t> objMaterials;
  std::string warn;
  std::string err;

  std::filesystem::path const objDir = path.parent_path();
  bool const loaded = tinyobj::LoadObj(&attrib, &shapes, &objMaterials, &warn,
                                       &err, path.string().c_str(),
                                       objDir.string().c_str(), true, true);
  if (!loaded) {
    throw std::runtime_error("Failed to load OBJ: " + path.string() + " " +
                             err);
  }
  if (!warn.empty()) {
    std::cerr << "OBJ loader warning for " << path.string() << ": " << warn;
    if (warn.back() != '\n') {
      std::cerr << '\n';
    }
  }
  if (attrib.vertices.empty() || shapes.empty()) {
    throw std::runtime_error("OBJ contains no drawable geometry: " +
                             path.string());
  }

  LoadedScene result{};
  result.materials = loadMaterials(path, objMaterials, fallbackAlbedoPath);

  for (tinyobj::shape_t const &shape : shapes) {
    std::map<MaterialId, MeshBuilder> buildersByMaterial;

    std::size_t indexOffset = 0;
    for (std::size_t faceIndex = 0;
         faceIndex < shape.mesh.num_face_vertices.size(); ++faceIndex) {
      int const vertexCount = shape.mesh.num_face_vertices[faceIndex];
      if (vertexCount < 3) {
        indexOffset += static_cast<std::size_t>(std::max(vertexCount, 0));
        continue;
      }

      int materialIndex = -1;
      if (faceIndex < shape.mesh.material_ids.size()) {
        materialIndex = shape.mesh.material_ids[faceIndex];
      }
      MaterialId const materialId =
          resolveMaterialId(materialIndex, result.materials.size());

      MeshBuilder &builder = buildersByMaterial[materialId];
      builder.materialId = materialId;

      for (int i = 1; i + 1 < vertexCount; ++i) {
        std::array<tinyobj::index_t, 3> triangleIndices = {
            shape.mesh.indices[indexOffset + 0],
            shape.mesh.indices[indexOffset + static_cast<std::size_t>(i)],
            shape.mesh.indices[indexOffset + static_cast<std::size_t>(i + 1)],
        };
        glm::vec3 const p0 =
            readPosition(attrib, triangleIndices[0].vertex_index);
        glm::vec3 const p1 =
            readPosition(attrib, triangleIndices[1].vertex_index);
        glm::vec3 const p2 =
            readPosition(attrib, triangleIndices[2].vertex_index);
        glm::vec3 const normal = fallbackNormal(p0, p1, p2);

        appendVertex(builder, attrib, triangleIndices[0], normal);
        appendVertex(builder, attrib, triangleIndices[1], normal);
        appendVertex(builder, attrib, triangleIndices[2], normal);
      }

      indexOffset += static_cast<std::size_t>(vertexCount);
    }

    for (auto &[_, builder] : buildersByMaterial) {
      if (builder.mesh.vertices.empty() || builder.mesh.indices.empty()) {
        continue;
      }
      MeshId const meshId = static_cast<MeshId>(result.meshes.size());
      generateMeshTangents(builder.mesh);
      result.meshes.push_back(std::move(builder.mesh));
      SceneObject object{};
      object.meshId = meshId;
      object.materialId = builder.materialId;
      result.objects.push_back(object);
    }
  }

  Transform const sceneTransform =
      normalizedSceneTransform(sceneBounds(result.meshes));
  for (SceneObject &object : result.objects) {
    object.transform = sceneTransform;
    object.worldBounds = transformBounds(
        result.meshes.at(object.meshId).localBounds, object.transform.matrix());
  }

  if (result.meshes.empty() || result.objects.empty()) {
    throw std::runtime_error("OBJ import produced no drawable objects: " +
                             path.string());
  }
  return result;
}
