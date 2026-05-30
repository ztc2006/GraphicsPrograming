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

  static vk::VertexInputBindingDescription bindingDescription() {
    return vk::VertexInputBindingDescription{
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };
  }

  static std::array<vk::VertexInputAttributeDescription, 4>
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
    };
  }
};
struct Mesh {
  std::vector<Vertex> vertices;
  std::vector<std::uint32_t> indices;
  Aabb localBounds{};
};
