#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "glm_include.hpp"
#include "vulkan_include.hpp"

struct Aabb {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  bool valid = false;
};

struct Vertex {
  glm::vec3 position;
  glm::vec3 color;
  glm::vec3 normal;
  glm::vec2 uv;
  glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
  glm::vec2 normalUv{0.0f};
  glm::vec2 metallicRoughnessUv{0.0f};
  glm::vec2 occlusionUv{0.0f};
  glm::vec2 emissiveUv{0.0f};
  float alpha = 1.0f;
  glm::vec2 specularUv{0.0f};
  glm::vec2 specularColorUv{0.0f};

  static vk::VertexInputBindingDescription bindingDescription() {
    return vk::VertexInputBindingDescription{
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };
  }

  static std::array<vk::VertexInputAttributeDescription, 12>
  attributeDescriptions() {
    return {
        vk::VertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32B32Sfloat,
            .offset = offsetof(Vertex, position),
        },
        vk::VertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = vk::Format::eR32G32B32Sfloat,
            .offset = offsetof(Vertex, color),
        },
        vk::VertexInputAttributeDescription{
            .location = 2,
            .binding = 0,
            .format = vk::Format::eR32G32B32Sfloat,
            .offset = offsetof(Vertex, normal),
        },
        vk::VertexInputAttributeDescription{
            .location = 3,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(Vertex, uv),
        },
        vk::VertexInputAttributeDescription{
            .location = 4,
            .binding = 0,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .offset = offsetof(Vertex, tangent),
        },
        vk::VertexInputAttributeDescription{.location = 5,
                                            .binding = 0,
                                            .format = vk::Format::eR32G32Sfloat,
                                            .offset =
                                                offsetof(Vertex, normalUv)},
        vk::VertexInputAttributeDescription{
            .location = 6,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(Vertex, metallicRoughnessUv)},
        vk::VertexInputAttributeDescription{.location = 7,
                                            .binding = 0,
                                            .format = vk::Format::eR32G32Sfloat,
                                            .offset =
                                                offsetof(Vertex, occlusionUv)},
        vk::VertexInputAttributeDescription{.location = 8,
                                            .binding = 0,
                                            .format = vk::Format::eR32G32Sfloat,
                                            .offset =
                                                offsetof(Vertex, emissiveUv)},
        vk::VertexInputAttributeDescription{.location = 9,
                                            .binding = 0,
                                            .format = vk::Format::eR32Sfloat,
                                            .offset = offsetof(Vertex, alpha)},
        vk::VertexInputAttributeDescription{.location = 10,
                                            .binding = 0,
                                            .format = vk::Format::eR32G32Sfloat,
                                            .offset =
                                                offsetof(Vertex, specularUv)},
        vk::VertexInputAttributeDescription{
            .location = 11,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(Vertex, specularColorUv)},
    };
  }
};
static_assert(sizeof(Vertex) == 112 && offsetof(Vertex, alpha) == 92 &&
              offsetof(Vertex, specularUv) == 96 &&
              offsetof(Vertex, specularColorUv) == 104);
struct Mesh {
  std::vector<Vertex> vertices;
  std::vector<std::uint32_t> indices;
  Aabb localBounds{};
};

void generateMeshTangents(Mesh &mesh, bool useNormalUv = false);
