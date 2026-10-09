#pragma once
#include "device.hpp"
#include "mesh.hpp"
#include "punctual_lights.hpp"
#include "render_graph.hpp"
#include "texture.hpp"
#include <array>
#include <memory>
#include <span>
#include <vector>

class UploadBatch;
struct RtMeshInput {
  vk::Buffer vertices, indices;
  std::uint32_t vertexCount, indexCount;
  std::span<Vertex const> hostVertices{};
  std::span<std::uint32_t const> hostIndices{};
};
// Built with the scene's upload batch; destination buffers outlive this owner.
class RayTracingGeometry {
public:
  RayTracingGeometry(Device const &, std::span<RtMeshInput const>,
                     UploadBatch &, ResourceLedger::Scope);
  ~RayTracingGeometry();
  void uploadsCompleted();
  vk::DeviceAddress address(std::size_t mesh) const;
  RtMeshInput const &mesh(std::size_t index) const;
  std::array<glm::vec3, 3> triangle(std::size_t mesh,
                                    std::size_t primitive) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct RtInstanceInput {
  std::uint32_t mesh = 0, material = 0;
  glm::mat4 transform{1};
  glm::vec4 tint{1};
  glm::vec4 alpha{0, .5f, 0, 0};
  bool doubleSided = false;
  bool primaryVisible = true, shadowCaster = true, sourceAreaLight = false;
  glm::vec4 pbr{0, 1, 1, 0}, emission{0}, specular{1};
  glm::vec4 optical{1.5f,1,0,0}, absorptionThickness{0};
  float normalScale = 1;
  glm::uvec4 moreTextures{0xffffffffu};
  glm::uvec4 maps{0xffffffffu};
  std::uint32_t albedoTexture = 0xffffffffu, alphaTexture = 0xffffffffu,
                normalTexture = 0xffffffffu;
};
struct RtAreaLight {
  glm::vec4 p0Area{}, p1Probability{}, p2Cdf{}, normal{};
  glm::uvec4 ids{}; // instance, primitive, doubleSided, reserved
};
static_assert(sizeof(RtAreaLight) == 80);
struct RtLightingGpu {
  glm::vec4 sunDirection{0, 1, 0, 0}, sunColor{0}, strengths{1}, environment{};
  glm::uvec4 counts{0, 0, 0,
                    8}; // punctual, areas, previous sample count, max bounces
  glm::uvec4 sampling{1, 0, 1,
                      3}; // spp, sequence, accumulate, normal/spec-AA flags
};
static_assert(sizeof(RtLightingGpu) == 96);
struct RtLightingFrame {
  RtLightingGpu settings;
  std::span<GpuPunctualLight const> punctual{};
  std::span<RtAreaLight const> areas{};
  vk::DescriptorImageInfo environment{};
  std::vector<std::byte> historyKey{};
  bool transport = false;
};
struct RtPush {
  glm::mat4 inverseViewProjection{1};
  glm::vec4 camera{0, 0, 0, 1};
  // x: foundation debug (0 color, 1 normal, 2 distance), y: cull, z: front
  // winding.
  glm::uvec4 options{0, 2, 0, 0};
};
static_assert(sizeof(RtPush) == 96);
// Caller waits this frame slot before prepare, and drains before destruction.
class RayTracingRenderer {
public:
  static constexpr vk::Format format = vk::Format::eR32G32B32A32Sfloat;
  static constexpr unsigned textureCapacity = 512;
  RayTracingRenderer(Device const &, vk::Extent2D, unsigned frameSlots);
  ~RayTracingRenderer();
  void prepare(unsigned slot, RayTracingGeometry const &,
               std::span<RtInstanceInput const>,
               std::span<vk::DescriptorImageInfo const>,
               RtLightingFrame const & = {});
  void build(vk::CommandBuffer, unsigned slot);
  void trace(vk::CommandBuffer, unsigned slot, RtPush const &);
  RenderGraph::ImageId importTarget(RenderGraph &) const;
  void submitted(RenderGraph::Plan const &, RenderGraph::ImageId);
  vk::Image image() const;
  vk::ImageView view() const;
  std::uint32_t instanceCount(unsigned slot) const;
  void resetAccumulation();
  std::uint32_t accumulatedSamples() const;
  static constexpr unsigned instanceBytes = 176;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
