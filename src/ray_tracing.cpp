#include "ray_tracing.hpp"
#include "upload_batch.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace {
template <class T> T proc(Device const &device, char const *name) {
  auto p = reinterpret_cast<T>(
      vkGetDeviceProcAddr(VkDevice(device.deviceHandle()), name));
  if (!p)
    throw std::runtime_error(std::string("Missing RT function: ") + name);
  return p;
}
void check(VkResult status, char const *operation) {
  if (status != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + ": " +
                             std::to_string(status));
}
vk::DeviceAddress bufferAddress(Device const &device, vk::Buffer buffer) {
  VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = VkBuffer(buffer);
  auto result =
      vkGetBufferDeviceAddress(VkDevice(device.deviceHandle()), &info);
  if (!result)
    throw std::runtime_error("RT buffer has no device address");
  return result;
}
std::uint64_t align(std::uint64_t value, std::uint64_t alignment) {
  if (!alignment || value > UINT64_MAX - alignment + 1)
    throw std::runtime_error("Invalid RT address alignment");
  return (value + alignment - 1) / alignment * alignment;
}
VkAccelerationStructureBuildSizesInfoKHR
sizes(Device const &device,
      VkAccelerationStructureBuildGeometryInfoKHR const &info,
      std::uint32_t count) {
  VkAccelerationStructureBuildSizesInfoKHR result{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  proc<PFN_vkGetAccelerationStructureBuildSizesKHR>(
      device, "vkGetAccelerationStructureBuildSizesKHR")(
      VkDevice(device.deviceHandle()),
      VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &result);
  return result;
}
void barrier(vk::CommandBuffer command, vk::PipelineStageFlags2 src,
             vk::AccessFlags2 access, vk::PipelineStageFlags2 dst,
             vk::AccessFlags2 next) {
  vk::MemoryBarrier2 memory{.srcStageMask = src,
                            .srcAccessMask = access,
                            .dstStageMask = dst,
                            .dstAccessMask = next};
  command.pipelineBarrier2(
      vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &memory});
}
struct Acceleration {
  Acceleration() = default;
  Acceleration(Acceleration &&) = default;
  Acceleration &operator=(Acceleration &&other) noexcept {
    if (this != &other) {
      handle.clear();
      storage = std::move(other.storage);
      scratch = std::move(other.scratch);
      handle = std::move(other.handle);
      scratchAddress = other.scratchAddress;
      address = other.address;
    }
    return *this;
  }
  GpuBuffer storage, scratch;
  vk::raii::AccelerationStructureKHR handle = nullptr;
  vk::DeviceAddress scratchAddress = 0, address = 0;
};
Acceleration
createAcceleration(Device const &device, VkAccelerationStructureTypeKHR type,
                   VkAccelerationStructureBuildSizesInfoKHR const &size,
                   ResourceLedger::Scope scope,
                   ResourceLedger::Scope scratchScope = {}) {
  Acceleration out;
  out.storage = device.createBuffer(
      size.accelerationStructureSize,
      vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
          vk::BufferUsageFlagBits::eShaderDeviceAddress,
      vk::MemoryPropertyFlagBits::eDeviceLocal, scope);
  auto properties =
      device.physicalDevice()
          .getProperties2<
              vk::PhysicalDeviceProperties2,
              vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
  auto a =
      properties.get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
          .minAccelerationStructureScratchOffsetAlignment;
  out.scratch = device.createBuffer(
      std::max<std::uint64_t>(size.buildScratchSize, 1) + a - 1,
      vk::BufferUsageFlagBits::eStorageBuffer |
          vk::BufferUsageFlagBits::eShaderDeviceAddress,
      vk::MemoryPropertyFlagBits::eDeviceLocal,
      scratchScope ? scratchScope : scope);
  out.scratchAddress = align(bufferAddress(device, *out.scratch.buffer), a);
  out.handle = vk::raii::AccelerationStructureKHR(
      device.logicalDevice(),
      vk::AccelerationStructureCreateInfoKHR{
          .buffer = *out.storage.buffer,
          .size = size.accelerationStructureSize,
          .type = vk::AccelerationStructureTypeKHR(type)});
  VkAccelerationStructureDeviceAddressInfoKHR address{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  address.accelerationStructure = VkAccelerationStructureKHR(*out.handle);
  out.address = proc<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
      device, "vkGetAccelerationStructureDeviceAddressKHR")(
      VkDevice(device.deviceHandle()), &address);
  return out;
}
void buildAcceleration(Device const &device, vk::CommandBuffer command,
                       VkAccelerationStructureBuildGeometryInfoKHR &info,
                       Acceleration const &as, unsigned count) {
  info.dstAccelerationStructure = VkAccelerationStructureKHR(*as.handle);
  info.scratchData.deviceAddress = as.scratchAddress;
  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = count;
  auto p = &range;
  proc<PFN_vkCmdBuildAccelerationStructuresKHR>(
      device, "vkCmdBuildAccelerationStructuresKHR")(VkCommandBuffer(command),
                                                     1, &info, &p);
}
std::vector<std::uint32_t> shader(char const *name) {
  std::ifstream file(std::string("shaders/") + name + ".spv",
                     std::ios::binary | std::ios::ate);
  auto n = file ? file.tellg() : std::streampos(-1);
  if (n <= 0 || std::size_t(n) % 4)
    throw std::runtime_error(std::string("Invalid RT shader: ") + name);
  std::vector<std::uint32_t> result(std::size_t(n) / 4);
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(result.data()), n))
    throw std::runtime_error("RT shader read failed");
  return result;
}
struct InstanceGpu {
  std::uint64_t vertices = 0, indices = 0;
  glm::vec4 tint{1};
  // cutoff, alpha mode, double sided, reserved
  glm::vec4 surface{};
  glm::uvec4 textures{};
  glm::vec4 pbr{}, emission{}, specular{1};
  glm::uvec4 moreTextures{0xffffffffu}, maps{0xffffffffu};
  glm::vec4 optical{1.5f,1,0,0}, absorptionThickness{0};
};
static_assert(sizeof(InstanceGpu) == 176 && offsetof(InstanceGpu, tint) == 16);
} // namespace
struct RayTracingGeometry::Impl {
  std::vector<RtMeshInput> meshes;
  std::vector<Acceleration> blas;
  struct CpuMesh {
    std::vector<glm::vec3> positions;
    std::vector<std::uint32_t> indices;
  };
  std::vector<CpuMesh> cpu;
};
RayTracingGeometry::RayTracingGeometry(Device const &device,
                                       std::span<RtMeshInput const> meshes,
                                       UploadBatch &uploads,
                                       ResourceLedger::Scope scope)
    : impl_(std::make_unique<Impl>()) {
  impl_->meshes.assign(meshes.begin(), meshes.end());
  for (auto const &mesh : meshes) {
    Impl::CpuMesh cpu;
    cpu.positions.reserve(mesh.hostVertices.size());
    for (auto const &v : mesh.hostVertices)
      cpu.positions.push_back(v.position);
    cpu.indices.assign(mesh.hostIndices.begin(), mesh.hostIndices.end());
    impl_->cpu.push_back(std::move(cpu));
  }
  for (auto &mesh : impl_->meshes) {
    mesh.hostVertices = {};
    mesh.hostIndices = {};
  }
  auto properties =
      device.physicalDevice()
          .getProperties2<
              vk::PhysicalDeviceProperties2,
              vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
  auto maximum =
      properties.get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
          .maxPrimitiveCount;
  for (auto const &mesh : meshes) {
    if (!mesh.vertexCount || !mesh.indexCount || mesh.indexCount % 3 ||
        mesh.indexCount / 3 > maximum)
      throw std::runtime_error("RT mesh requires bounded indexed triangles");
    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    auto &triangles = geometry.geometry.triangles;
    triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = bufferAddress(device, mesh.vertices);
    triangles.vertexStride = sizeof(Vertex);
    triangles.maxVertex = mesh.vertexCount - 1;
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = bufferAddress(device, mesh.indices);
    VkAccelerationStructureBuildGeometryInfoKHR info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    auto as = createAcceleration(
        device, info.type, sizes(device, info, mesh.indexCount / 3), scope,
        device.resourceLedger().scope(ResourceLedger::Domain::Staging));
    buildAcceleration(device, uploads.recordingCommand(), info, as,
                      mesh.indexCount / 3);
    impl_->blas.push_back(std::move(as));
  }
  barrier(uploads.recordingCommand(),
          vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
          vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
          vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR |
              vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
          vk::AccessFlagBits2::eAccelerationStructureReadKHR);
}
RayTracingGeometry::~RayTracingGeometry() = default;
void RayTracingGeometry::uploadsCompleted() {
  for (auto &as : impl_->blas)
    as.scratch = GpuBuffer{};
}
vk::DeviceAddress RayTracingGeometry::address(std::size_t mesh) const {
  return impl_->blas.at(mesh).address;
}
RtMeshInput const &RayTracingGeometry::mesh(std::size_t mesh) const {
  return impl_->meshes.at(mesh);
}
std::array<glm::vec3, 3>
RayTracingGeometry::triangle(std::size_t mesh, std::size_t primitive) const {
  auto const &cpu = impl_->cpu.at(mesh);
  return {cpu.positions.at(cpu.indices.at(primitive * 3)),
          cpu.positions.at(cpu.indices.at(primitive * 3 + 1)),
          cpu.positions.at(cpu.indices.at(primitive * 3 + 2))};
}
struct RayTracingRenderer::Impl {
  Device const &device;
  vk::Extent2D extent;
  ResourceLedger::Scope scope;
  GpuImage target;
  TextureResources fallbackEnvironment;
  vk::raii::ImageView view = nullptr;
  vk::raii::DescriptorSetLayout descriptors = nullptr;
  vk::raii::DescriptorPool pool = nullptr;
  ResourceLedger::Lease descriptorAccounting;
  vk::raii::PipelineLayout layout = nullptr;
  vk::raii::Pipeline pipeline = nullptr;
  GpuBuffer sbt;
  VkStridedDeviceAddressRegionKHR raygen{}, miss{}, hit{}, callable{};
  struct Slot {
    GpuBuffer instances, table, lighting, punctual, areas;
    std::vector<std::byte> pendingKey;
    std::uint32_t nextSamples = 0;
    bool transport = false;
    Acceleration tlas;
    vk::DescriptorSet set{};
    std::uint32_t count = 0, capacity = 0;
  };
  std::vector<Slot> slots;
  RenderGraph::State state{};
  std::vector<std::byte> historyKey;
  std::uint32_t samples = 0, sequence = 0, lastSlot = 0;
  bool reset = true;
  Impl(Device const &d, vk::Extent2D e)
      : device(d), extent(e),
        scope(d.resourceLedger().scope(ResourceLedger::Domain::Persistent)) {}
};
RayTracingRenderer::RayTracingRenderer(Device const &device,
                                       vk::Extent2D extent, unsigned frameSlots)
    : impl_(std::make_unique<Impl>(device, extent)) {
  auto &p = *impl_;
  if (!device.rayTracingSupported())
    throw std::runtime_error("Vulkan RT Pipeline features unavailable");
  auto const limits = device.physicalDevice().getProperties().limits;
  if (textureCapacity > limits.maxPerStageDescriptorSamplers ||
      textureCapacity + 1 > limits.maxDescriptorSetSamplers ||
      textureCapacity > limits.maxPerStageDescriptorSampledImages ||
      textureCapacity + 1 > limits.maxDescriptorSetSampledImages ||
      textureCapacity + 8 > limits.maxPerStageResources)
    throw std::runtime_error(
        "RT needs 512 sampled-image descriptors per stage");
  auto required = vk::FormatFeatureFlagBits::eStorageImage |
                  vk::FormatFeatureFlagBits::eSampledImage |
                  vk::FormatFeatureFlagBits::eTransferSrc;
  if ((device.physicalDevice()
           .getFormatProperties(format)
           .optimalTilingFeatures &
       required) != required)
    throw std::runtime_error(
        "RT RGBA32F storage/sampling/readback unsupported");
  p.target = device.createImage(
      vk::ImageCreateInfo{.imageType = vk::ImageType::e2D,
                          .format = format,
                          .extent = {extent.width, extent.height, 1},
                          .mipLevels = 1,
                          .arrayLayers = 1,
                          .samples = vk::SampleCountFlagBits::e1,
                          .tiling = vk::ImageTiling::eOptimal,
                          .usage = vk::ImageUsageFlagBits::eStorage |
                                   vk::ImageUsageFlagBits::eSampled |
                                   vk::ImageUsageFlagBits::eTransferSrc,
                          .sharingMode = vk::SharingMode::eExclusive},
      std::uint64_t(extent.width) * extent.height * 16,
      vk::MemoryPropertyFlagBits::eDeviceLocal, p.scope);
  p.view = vk::raii::ImageView(
      device.logicalDevice(),
      vk::ImageViewCreateInfo{
          .image = *p.target.image,
          .viewType = vk::ImageViewType::e2D,
          .format = format,
          .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
  UploadBatch fallbackUpload(device);
  std::array<float, 24> fallbackPixels{};
  p.fallbackEnvironment =
      TextureLoader(device, p.scope)
          .createFromHdrCube(fallbackPixels, 1, fallbackUpload);
  fallbackUpload.finish();
  auto hitStages = vk::ShaderStageFlagBits::eAnyHitKHR |
                   vk::ShaderStageFlagBits::eClosestHitKHR;
  auto materialStages = hitStages | vk::ShaderStageFlagBits::eRaygenKHR;
  std::array bindings{
      vk::DescriptorSetLayoutBinding{
          0, vk::DescriptorType::eAccelerationStructureKHR, 1,
          vk::ShaderStageFlagBits::eRaygenKHR},
      vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eStorageImage, 1,
                                     vk::ShaderStageFlagBits::eRaygenKHR},
      vk::DescriptorSetLayoutBinding{2, vk::DescriptorType::eStorageBuffer, 1,
                                     materialStages},
      vk::DescriptorSetLayoutBinding{3,
                                     vk::DescriptorType::eCombinedImageSampler,
                                     textureCapacity, materialStages},
      vk::DescriptorSetLayoutBinding{4,
                                     vk::DescriptorType::eCombinedImageSampler,
                                     1, vk::ShaderStageFlagBits::eRaygenKHR},
      vk::DescriptorSetLayoutBinding{5, vk::DescriptorType::eStorageBuffer, 1,
                                     vk::ShaderStageFlagBits::eRaygenKHR |
                                         hitStages},
      vk::DescriptorSetLayoutBinding{6, vk::DescriptorType::eStorageBuffer, 1,
                                     vk::ShaderStageFlagBits::eRaygenKHR},
      vk::DescriptorSetLayoutBinding{7, vk::DescriptorType::eStorageBuffer, 1,
                                     vk::ShaderStageFlagBits::eRaygenKHR}};
  p.descriptors = vk::raii::DescriptorSetLayout(
      device.logicalDevice(),
      vk::DescriptorSetLayoutCreateInfo{.bindingCount = bindings.size(),
                                        .pBindings = bindings.data()});
  std::array poolSizes{
      vk::DescriptorPoolSize{vk::DescriptorType::eAccelerationStructureKHR,
                             frameSlots},
      vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, frameSlots},
      vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer,
                             frameSlots * 4},
      vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                             frameSlots * (textureCapacity + 1)}};
  p.pool = vk::raii::DescriptorPool(
      device.logicalDevice(),
      vk::DescriptorPoolCreateInfo{.maxSets = frameSlots,
                                   .poolSizeCount = poolSizes.size(),
                                   .pPoolSizes = poolSizes.data()});
  std::vector<vk::DescriptorSetLayout> layouts(frameSlots, *p.descriptors);
  auto sets = (*device.logicalDevice())
                  .allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
                      .descriptorPool = *p.pool,
                      .descriptorSetCount = frameSlots,
                      .pSetLayouts = layouts.data()});
  p.slots.resize(frameSlots);
  for (unsigned i = 0; i < frameSlots; ++i)
    p.slots[i].set = sets[i];
  p.descriptorAccounting = p.scope.track(
      {.imageViews = 1, .descriptorPools = 1, .descriptorSets = frameSlots});
  vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eRaygenKHR |
                                           hitStages,
                             .size = sizeof(RtPush)};
  auto layout = *p.descriptors;
  p.layout = vk::raii::PipelineLayout(
      device.logicalDevice(),
      vk::PipelineLayoutCreateInfo{.setLayoutCount = 1,
                                   .pSetLayouts = &layout,
                                   .pushConstantRangeCount = 1,
                                   .pPushConstantRanges = &push});
  std::array<char const *, 5> names{"primary.rgen", "primary.rmiss",
                                    "primary.rchit", "primary.rahit",
                                    "shadow.rmiss"};
  std::array stagesFlags{
      vk::ShaderStageFlagBits::eRaygenKHR, vk::ShaderStageFlagBits::eMissKHR,
      vk::ShaderStageFlagBits::eClosestHitKHR,
      vk::ShaderStageFlagBits::eAnyHitKHR, vk::ShaderStageFlagBits::eMissKHR};
  std::vector<vk::raii::ShaderModule> modules;
  std::array<vk::PipelineShaderStageCreateInfo, 5> stages{};
  for (unsigned i = 0; i < 5; ++i) {
    auto code = shader(names[i]);
    modules.emplace_back(device.logicalDevice(),
                         vk::ShaderModuleCreateInfo{.codeSize = code.size() * 4,
                                                    .pCode = code.data()});
    stages[i] = {
        .stage = stagesFlags[i], .module = *modules.back(), .pName = "main"};
  }
  std::array<vk::RayTracingShaderGroupCreateInfoKHR, 5> groups{};
  for (auto &g : groups) {
    g.generalShader = g.closestHitShader = g.anyHitShader =
        g.intersectionShader = VK_SHADER_UNUSED_KHR;
  }
  groups[0].type = groups[1].type = groups[2].type =
      vk::RayTracingShaderGroupTypeKHR::eGeneral;
  groups[0].generalShader = 0;
  groups[1].generalShader = 1;
  groups[2].generalShader = 4;
  groups[3].type = groups[4].type =
      vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup;
  groups[3].closestHitShader = 2;
  groups[3].anyHitShader = 3;
  groups[4].anyHitShader = 3;
  p.pipeline = vk::raii::Pipeline(
      device.logicalDevice(), nullptr, nullptr,
      vk::RayTracingPipelineCreateInfoKHR{.stageCount = stages.size(),
                                          .pStages = stages.data(),
                                          .groupCount = groups.size(),
                                          .pGroups = groups.data(),
                                          .maxPipelineRayRecursionDepth = 1,
                                          .layout = *p.layout});
  auto props =
      device.physicalDevice()
          .getProperties2<vk::PhysicalDeviceProperties2,
                          vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
  auto const &rt =
      props.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
  auto stride = align(rt.shaderGroupHandleSize, rt.shaderGroupHandleAlignment);
  if (stride > rt.maxShaderGroupStride)
    throw std::runtime_error("RT SBT stride exceeds device limit");
  auto record = align(stride, rt.shaderGroupBaseAlignment);
  p.sbt =
      device.createBuffer(5 * record + rt.shaderGroupBaseAlignment - 1,
                          vk::BufferUsageFlagBits::eShaderBindingTableKHR |
                              vk::BufferUsageFlagBits::eShaderDeviceAddress,
                          vk::MemoryPropertyFlagBits::eHostVisible, p.scope);
  auto base = bufferAddress(device, *p.sbt.buffer),
       aligned = align(base, rt.shaderGroupBaseAlignment);
  std::vector<std::byte> handles(5 * rt.shaderGroupHandleSize);
  check(proc<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
            device, "vkGetRayTracingShaderGroupHandlesKHR")(
            VkDevice(device.deviceHandle()), VkPipeline(*p.pipeline), 0, 5,
            handles.size(), handles.data()),
        "RT SBT handles");
  for (unsigned i = 0; i < 5; ++i)
    p.sbt.write(std::span(handles).subspan(i * rt.shaderGroupHandleSize,
                                           rt.shaderGroupHandleSize),
                aligned - base + i * record);
  p.raygen = {aligned, record, record};
  p.miss = {aligned + record, record, record * 2};
  p.hit = {aligned + 3 * record, record, record * 2};
  device.nameObject(*p.pipeline, "RT-A independent camera primary rays");
  device.nameObject(*p.target.image, "RT-A HDR RGBA32F");
}
RayTracingRenderer::~RayTracingRenderer() = default;
void RayTracingRenderer::prepare(
    unsigned index, RayTracingGeometry const &geometry,
    std::span<RtInstanceInput const> input,
    std::span<vk::DescriptorImageInfo const> textures,
    RtLightingFrame const &frame) {
  auto &p = *impl_;
  auto &slot = p.slots.at(index);
  if (frame.settings.counts.w < 1 || frame.settings.counts.w > 32 ||
      frame.settings.sampling.x < 1 || frame.settings.sampling.x > 64)
    throw std::runtime_error(
        "RT sample/bounce settings outside supported range");
  auto limit =
      p.device.physicalDevice().getProperties().limits.maxStorageBufferRange;
  if (frame.punctual.size_bytes() > limit || frame.areas.size_bytes() > limit)
    throw std::runtime_error("RT light snapshot exceeds storage buffer range");
  auto limits = p.device.physicalDevice().getProperties().limits;
  auto properties =
      p.device.physicalDevice()
          .getProperties2<
              vk::PhysicalDeviceProperties2,
              vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
  if (input.size() > 0xffffff ||
      input.size() >
          properties.get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
              .maxInstanceCount ||
      std::max<std::size_t>(input.size(), 1) * sizeof(InstanceGpu) >
          limits.maxStorageBufferRange ||
      textures.empty() || textures.size() > textureCapacity)
    throw std::runtime_error("RT scene exceeds instance/table/texture limits");
  std::vector<VkAccelerationStructureInstanceKHR> instances(
      std::max<std::size_t>(input.size(), 1));
  std::vector<InstanceGpu> table(instances.size());
  for (unsigned i = 0; i < input.size(); ++i) {
    auto const &source = input[i];
    if (source.albedoTexture == 0xffffffffu &&
        source.material >= textures.size() / 2)
      throw std::runtime_error("RT material texture index out of range");
    auto const &mesh = geometry.mesh(source.mesh);
    auto &dst = instances[i];
    for (unsigned row = 0; row < 3; ++row)
      for (unsigned col = 0; col < 4; ++col)
        dst.transform.matrix[row][col] = source.transform[col][row];
    dst.instanceCustomIndex = i;
    dst.mask =
        (source.primaryVisible ? 1 : 0) | 2 | (source.shadowCaster ? 4 : 0);
    dst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    // Ray triangle facing is determined in object space; mirroring an instance
    // does not require an additional facing flip. Raster compensates
    // separately.
    dst.accelerationStructureReference = geometry.address(source.mesh);
    auto albedo = source.albedoTexture == 0xffffffffu ? source.material * 2
                                                      : source.albedoTexture;
    auto alpha = source.alphaTexture == 0xffffffffu ? source.material * 2 + 1
                                                    : source.alphaTexture;
    table[i] = {
        bufferAddress(p.device, mesh.vertices),
        bufferAddress(p.device, mesh.indices),
        source.tint,
        {source.alpha.y, source.alpha.x, float(source.doubleSided),
         source.normalScale},
        {albedo, alpha, std::uint32_t(source.alpha.z), source.normalTexture},
        source.pbr,
        source.emission,
        source.specular,
        source.moreTextures,
        source.maps,source.optical,source.absorptionThickness};
    table[i].pbr.w = source.sourceAreaLight ? 1.f : 0.f;
  }
  auto wanted = std::uint32_t(instances.size());
  if (wanted > slot.capacity) {
    auto inst = p.device.createBuffer(
        wanted * sizeof(instances[0]),
        vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
            vk::BufferUsageFlagBits::eShaderDeviceAddress,
        vk::MemoryPropertyFlagBits::eHostVisible, p.scope);
    auto data = p.device.createBuffer(
        wanted * sizeof(InstanceGpu), vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible, p.scope);
    VkAccelerationStructureGeometryKHR g{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.data.deviceAddress =
        bufferAddress(p.device, *inst.buffer);
    VkAccelerationStructureBuildGeometryInfoKHR info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &g;
    auto tlas = createAcceleration(p.device, info.type,
                                   sizes(p.device, info, wanted), p.scope);
    slot.tlas = std::move(tlas);
    slot.instances = std::move(inst);
    slot.table = std::move(data);
    slot.capacity = wanted;
  }
  slot.count = input.size();
  slot.instances.write(std::as_bytes(std::span(instances)));
  slot.table.write(std::as_bytes(std::span(table)));
  auto writeStorage = [&](GpuBuffer &buffer, std::span<std::byte const> data) {
    auto wanted = std::max<std::size_t>(data.size_bytes(), 16);
    if (!buffer.valid() || buffer.memoryInfo().payloadBytes < wanted)
      buffer = p.device.createBuffer(
          wanted, vk::BufferUsageFlagBits::eStorageBuffer,
          vk::MemoryPropertyFlagBits::eHostVisible, p.scope);
    if (!data.empty())
      buffer.write(data);
    else {
      std::array<std::byte, 16> zero{};
      buffer.write(zero);
    }
  };
  auto settings = frame.settings;
  if (settings.counts.w < 1 || settings.counts.w > 32 ||
      settings.sampling.x < 1 || settings.sampling.x > 64)
    throw std::runtime_error(
        "RT sample/bounce settings outside supported range");
  settings.counts.x = std::uint32_t(frame.punctual.size());
  settings.counts.y = std::uint32_t(frame.areas.size());
  slot.pendingKey = frame.historyKey;
  auto append = [&](auto const &value) {
    auto bytes = std::as_bytes(std::span{&value, 1});
    slot.pendingKey.insert(slot.pendingKey.end(), bytes.begin(), bytes.end());
  };
  for (auto const &v : table)
    append(v);
  for (auto const &v : instances)
    append(v);
  for (auto const &v : textures) {
    append(v.sampler);
    append(v.imageView);
    append(v.imageLayout);
  }
  for (auto const &v : frame.punctual)
    append(v);
  for (auto const &v : frame.areas)
    append(v);
  auto explicitSeed = settings.sampling.y;
  settings.sampling.y = 0;
  append(settings);
  append(frame.environment.sampler);
  append(frame.environment.imageView);
  append(frame.environment.imageLayout);
  bool same = frame.transport && settings.sampling.z && !p.reset &&
              slot.pendingKey == p.historyKey;
  settings.counts.z = same ? p.samples : 0;
  ++p.sequence;
  settings.sampling.y = explicitSeed ? explicitSeed : p.sequence;
  slot.nextSamples = frame.transport
                         ? std::min<std::uint32_t>(
                               1048576, settings.counts.z + settings.sampling.x)
                         : 0;
  slot.transport = frame.transport;
  p.lastSlot = index;
  writeStorage(slot.lighting, std::as_bytes(std::span{&settings, 1}));
  writeStorage(slot.punctual, std::as_bytes(frame.punctual));
  writeStorage(slot.areas, std::as_bytes(frame.areas));
  std::vector<vk::DescriptorImageInfo> all(textureCapacity, textures.front());
  std::copy(textures.begin(), textures.end(), all.begin());
  auto handle = *slot.tlas.handle;
  vk::WriteDescriptorSetAccelerationStructureKHR as{
      .accelerationStructureCount = 1, .pAccelerationStructures = &handle};
  vk::DescriptorImageInfo target{.imageView = *p.view,
                                 .imageLayout = vk::ImageLayout::eGeneral};
  vk::DescriptorBufferInfo data{
      .buffer = *slot.table.buffer,
      .range = std::max<std::size_t>(input.size(), 1) * sizeof(InstanceGpu)};
  vk::DescriptorImageInfo env = frame.environment;
  if (!env.imageView)
    env = {p.fallbackEnvironment.sampler(), p.fallbackEnvironment.imageView(),
           vk::ImageLayout::eShaderReadOnlyOptimal};
  vk::DescriptorBufferInfo header{*slot.lighting.buffer, 0,
                                  sizeof(RtLightingGpu)};
  vk::DescriptorBufferInfo lights{*slot.punctual.buffer, 0,
                                  slot.punctual.memoryInfo().payloadBytes};
  vk::DescriptorBufferInfo areas{*slot.areas.buffer, 0,
                                 slot.areas.memoryInfo().payloadBytes};
  std::array writes{
      vk::WriteDescriptorSet{.pNext = &as,
                             .dstSet = slot.set,
                             .dstBinding = 0,
                             .descriptorCount = 1,
                             .descriptorType =
                                 vk::DescriptorType::eAccelerationStructureKHR},
      vk::WriteDescriptorSet{.dstSet = slot.set,
                             .dstBinding = 1,
                             .descriptorCount = 1,
                             .descriptorType =
                                 vk::DescriptorType::eStorageImage,
                             .pImageInfo = &target},
      vk::WriteDescriptorSet{.dstSet = slot.set,
                             .dstBinding = 2,
                             .descriptorCount = 1,
                             .descriptorType =
                                 vk::DescriptorType::eStorageBuffer,
                             .pBufferInfo = &data},
      vk::WriteDescriptorSet{.dstSet = slot.set,
                             .dstBinding = 3,
                             .descriptorCount = textureCapacity,
                             .descriptorType =
                                 vk::DescriptorType::eCombinedImageSampler,
                             .pImageInfo = all.data()},
      vk::WriteDescriptorSet{.dstSet = slot.set,
                             .dstBinding = 4,
                             .descriptorCount = 1,
                             .descriptorType =
                                 vk::DescriptorType::eCombinedImageSampler,
                             .pImageInfo = &env},
      vk::WriteDescriptorSet{.dstSet = slot.set,
                             .dstBinding = 5,
                             .descriptorCount = 1,
                             .descriptorType =
                                 vk::DescriptorType::eStorageBuffer,
                             .pBufferInfo = &header},
      vk::WriteDescriptorSet{.dstSet = slot.set,
                             .dstBinding = 6,
                             .descriptorCount = 1,
                             .descriptorType =
                                 vk::DescriptorType::eStorageBuffer,
                             .pBufferInfo = &lights},
      vk::WriteDescriptorSet{.dstSet = slot.set,
                             .dstBinding = 7,
                             .descriptorCount = 1,
                             .descriptorType =
                                 vk::DescriptorType::eStorageBuffer,
                             .pBufferInfo = &areas}};
  p.device.logicalDevice().updateDescriptorSets(writes, {});
}
void RayTracingRenderer::build(vk::CommandBuffer command, unsigned index) {
  auto &p = *impl_;
  auto &slot = p.slots.at(index);
  barrier(command, vk::PipelineStageFlagBits2::eHost,
          vk::AccessFlagBits2::eHostWrite,
          vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR |
              vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
          vk::AccessFlagBits2::eShaderRead |
              vk::AccessFlagBits2::eShaderBindingTableReadKHR);
  VkAccelerationStructureGeometryKHR g{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  g.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  g.geometry.instances.data.deviceAddress =
      bufferAddress(p.device, *slot.instances.buffer);
  VkAccelerationStructureBuildGeometryInfoKHR info{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  info.geometryCount = 1;
  info.pGeometries = &g;
  buildAcceleration(p.device, command, info, slot.tlas, slot.count);
  barrier(command, vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
          vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
          vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
          vk::AccessFlagBits2::eAccelerationStructureReadKHR);
}
void RayTracingRenderer::trace(vk::CommandBuffer command, unsigned slot,
                               RtPush const &push) {
  auto &p = *impl_;
  command.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, *p.pipeline);
  command.bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, *p.layout,
                             0, {p.slots.at(slot).set}, {});
  command.pushConstants<RtPush>(*p.layout,
                                vk::ShaderStageFlagBits::eRaygenKHR |
                                    vk::ShaderStageFlagBits::eClosestHitKHR |
                                    vk::ShaderStageFlagBits::eAnyHitKHR,
                                0, push);
  proc<PFN_vkCmdTraceRaysKHR>(p.device, "vkCmdTraceRaysKHR")(
      VkCommandBuffer(command), &p.raygen, &p.miss, &p.hit, &p.callable,
      p.extent.width, p.extent.height, 1);
}
RenderGraph::ImageId
RayTracingRenderer::importTarget(RenderGraph &graph) const {
  auto const &p = *impl_;
  return graph.importImage({"RT HDR", *p.target.image, *p.view, format,
                            p.extent, vk::ImageAspectFlagBits::eColor,
                            vk::ImageUsageFlagBits::eStorage |
                                vk::ImageUsageFlagBits::eSampled |
                                vk::ImageUsageFlagBits::eTransferSrc,
                            false, p.state});
}
void RayTracingRenderer::submitted(RenderGraph::Plan const &plan,
                                   RenderGraph::ImageId id) {
  auto &p = *impl_;
  p.state = plan.finalState(id);
  p.state.stages |= vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
  p.state.access |= vk::AccessFlagBits2::eShaderStorageWrite;
  auto &slot = p.slots.at(p.lastSlot);
  p.samples = slot.nextSamples;
  p.historyKey = slot.pendingKey;
  p.reset = false;
}
vk::Image RayTracingRenderer::image() const { return *impl_->target.image; }
vk::ImageView RayTracingRenderer::view() const { return *impl_->view; }
std::uint32_t RayTracingRenderer::instanceCount(unsigned slot) const {
  return impl_->slots.at(slot).count;
}

void RayTracingRenderer::resetAccumulation() {
  impl_->reset = true;
  impl_->samples = 0;
}
std::uint32_t RayTracingRenderer::accumulatedSamples() const {
  return impl_->samples;
}
