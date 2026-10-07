#include "frame_context_regression.hpp"
#include "gltf_loader.hpp"
#include "hdr_output_regression.hpp"
#include "presentation_regression.hpp"
#include "renderer.hpp"
#include "texture_mip_regression.hpp"

#include <GLFW/glfw3.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

namespace {
void require(bool condition, char const *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class F> void expectFailure(F &&operation, char const *message) {
  bool failed = false;
  try {
    operation();
  } catch (std::runtime_error const &) {
    failed = true;
  }
  require(failed, message);
}
void checkAllocationStatistics(Device const &device) {
  auto snapshot = device.resourceLedger().snapshot();
  auto stats = device.gpuAllocationStatistics();
  auto const &backing = snapshot.at(ResourceLedger::Domain::AllocatorBlocks);
  require(backing.allocations == stats.blocks &&
              backing.allocatedBytes == stats.blockBytes &&
              snapshot.current.suballocations == stats.suballocations &&
              snapshot.current.suballocatedBytes == stats.suballocatedBytes,
          "Ledger differs from authoritative VMA block/allocation statistics");
}

void exerciseGpuAllocator(Device const &device) {
  auto ledger = device.resourceLedger();
  require(ledger.snapshot().current == ResourceLedger::Footprint{},
          "Allocator test baseline is not empty");
  {
    auto scene = ledger.scope(ResourceLedger::Domain::PreparedScene);
    auto staging = ledger.scope(ResourceLedger::Domain::Staging);
    auto first =
        device.createBuffer(4096, vk::BufferUsageFlagBits::eTransferSrc,
                            vk::MemoryPropertyFlagBits::eHostVisible, scene);
    auto second =
        device.createBuffer(4096, vk::BufferUsageFlagBits::eTransferDst,
                            vk::MemoryPropertyFlagBits::eHostVisible, staging);
    auto a = first.memoryInfo(), b = second.memoryInfo();
    require(a.block == b.block && a.offset + a.suballocationBytes <= b.offset,
            "Small compatible buffers did not share a block with independent "
            "ranges");
    auto initial = ledger.snapshot();
    require(
        initial.current.buffers == 2 && initial.current.payloadBytes == 8192 &&
            initial.current.allocations == 1 &&
            initial.current.suballocations == 2 &&
            initial.current.allocatedBytes > initial.current.suballocatedBytes,
        "Shared block was counted per resource or confused with suballocation "
        "bytes");
    checkAllocationStatistics(device);
    scene.setDomain(ResourceLedger::Domain::LiveScene);
    require(ledger.snapshot().current == initial.current &&
                ledger.snapshot()
                        .at(ResourceLedger::Domain::LiveScene)
                        .suballocatedBytes == a.suballocationBytes,
            "Resource transfer reclassified or duplicated its shared backing "
            "block");
    std::array<std::byte, 4096> bytesA, bytesB, downloaded;
    bytesA.fill(std::byte{17});
    bytesB.fill(std::byte{51});
    first.write(bytesA);
    second.write(bytesB);
    std::array<std::byte, 7> partial;
    partial.fill(std::byte{93});
    first.write(partial, 3);
    std::copy(partial.begin(), partial.end(), bytesA.begin() + 3);
    first.read(downloaded);
    require(downloaded == bytesA,
            "Allocation-relative unaligned write changed neighboring bytes");
    second.read(downloaded);
    require(downloaded == bytesB,
            "Mapping an adjacent suballocation corrupted shared memory");
    std::array<std::byte, 7> slice;
    first.read(slice, 3);
    require(slice == partial,
            "Allocation-relative invalidate/read range failed");
    expectFailure([&] { first.write(partial, 4095); },
                  "Out-of-bounds write accepted");
    expectFailure([&] { first.read(slice, UINT64_MAX); },
                  "Overflow read range accepted");
    expectFailure(
        [&] {
          (void)device.createBuffer(0, vk::BufferUsageFlagBits::eTransferSrc,
                                    {});
        },
        "Zero-size buffer accepted");
    auto replacement =
        device.createBuffer(512, vk::BufferUsageFlagBits::eTransferSrc,
                            vk::MemoryPropertyFlagBits::eHostVisible);
    replacement = std::move(first);
    require(!first.valid() && replacement.valid(),
            "Buffer move assignment left duplicate ownership");
    expectFailure([&] { first.read(slice); },
                  "Moved buffer still exposes an allocation");
    replacement.read(downloaded);
    require(downloaded == bytesA && ledger.snapshot().current.buffers == 2,
            "Move assignment freed the source or leaked the destination");
    checkAllocationStatistics(device);
    auto residentBacking =
        ledger.snapshot().at(ResourceLedger::Domain::AllocatorBlocks);
    {
      auto upload = device.createUploadBuffer(1024);
      require(upload.memoryInfo().block != replacement.memoryInfo().block,
              "Upload memory shared a block pinned by resident buffers");
      upload.write(partial, 3);
      upload.read(slice, 3);
      require(slice == partial, "Upload allocator mapping failed");
      checkAllocationStatistics(device);
    }
    require(
        ledger.snapshot().at(ResourceLedger::Domain::AllocatorBlocks) ==
            residentBacking,
        "Completed upload retained memory blocks behind live resident buffers");
    checkAllocationStatistics(device);
    std::cout << "PASS upload lifetime isolation: resident buffers remain, "
                 "upload backing fully reclaimed\n";
    auto concurrent = ledger.scope(ResourceLedger::Domain::PreparedScene);
    auto worker = [&] {
      std::vector<Device::BufferResources> held;
      for (int i = 0; i < 16; ++i) {
        auto resource = device.createBuffer(
            1024, vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible, concurrent);
        resource.write(partial, 5);
        held.push_back(std::move(resource));
      }
      return held;
    };
    auto x = std::async(std::launch::async, worker),
         y = std::async(std::launch::async, worker);
    auto heldX = x.get(), heldY = y.get();
    require(
        ledger.snapshot().at(ResourceLedger::Domain::PreparedScene).buffers ==
            32,
        "Concurrent allocator registration lost resources");
    checkAllocationStatistics(device);
    for (auto const &resource : heldY) {
      resource.read(slice, 5);
      require(slice == partial, "Concurrent mapping mixed allocation offsets");
    }
    std::cout << "PASS buffer allocator: shared block="
              << initial.current.allocatedBytes << " bytes, 2 suballocations="
              << initial.current.suballocatedBytes
              << " bytes; offset mapping, bounds, moves, 32 concurrent "
                 "buffers, VMA statistics\n";
    if (a.properties & vk::MemoryPropertyFlagBits::eHostCoherent)
      std::cout << "NOTE buffer mapping used coherent memory; noncoherent "
                   "hardware coverage remains open\n";
  }
  require(
      device.gpuAllocationStatistics().blocks == 0 &&
          ledger.snapshot().current == ResourceLedger::Footprint{},
      "Last buffer did not release allocator backing blocks and accounting");
  std::cout << "PASS allocator lifetime: last buffer destroys allocator; "
               "backing and suballocation counters return to zero\n";
}

void exerciseImageAllocator(Device const &device) {
  auto ledger = device.resourceLedger();
  require(ledger.snapshot().current == ResourceLedger::Footprint{},
          "Image allocator baseline is not empty");
  vk::ImageCreateInfo description{.imageType = vk::ImageType::e2D,
                                  .format = vk::Format::eR8G8B8A8Unorm,
                                  .extent = {17, 13, 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = vk::SampleCountFlagBits::e1,
                                  .tiling = vk::ImageTiling::eOptimal,
                                  .usage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eTransferDst,
                                  .sharingMode = vk::SharingMode::eExclusive};
  auto local = vk::MemoryPropertyFlagBits::eDeviceLocal;
  auto payload = vk::DeviceSize{17 * 13 * 4};
  GpuImage survivor;
  {
    auto scope = ledger.scope(ResourceLedger::Domain::PreparedScene);
    auto firstBuffer = device.createBuffer(
        128, vk::BufferUsageFlagBits::eTransferSrc, local, scope);
    auto color = device.createImage(description, payload, local, scope);
    auto secondBuffer = device.createBuffer(
        128, vk::BufferUsageFlagBits::eTransferDst, local, scope);
    auto depthDescription = description;
    depthDescription.format = vk::Format::eD32Sfloat;
    depthDescription.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                             vk::ImageUsageFlagBits::eSampled;
    auto depth = device.createImage(depthDescription, payload, local, scope);
    auto colorInfo = color.memoryInfo();
    auto requirement = color.image.getMemoryRequirements();
    require(colorInfo.offset % requirement.alignment == 0 &&
                colorInfo.suballocationBytes >= requirement.size &&
                colorInfo.payloadBytes == payload &&
                (colorInfo.properties & local) == local,
            "Image allocation ignored requirements or confused texel payload "
            "with its range");
    auto snapshot = ledger.snapshot();
    require(
        snapshot.at(ResourceLedger::Domain::PreparedScene).images == 2 &&
            snapshot.at(ResourceLedger::Domain::PreparedScene).allocations ==
                0 &&
            snapshot.current.suballocations == 4 &&
            snapshot.current.payloadBytes == 256 + 2 * payload,
        "Mixed images/buffers duplicated backing or lost resource accounting");
    checkAllocationStatistics(device);
    auto granularity =
        device.physicalDevice().getProperties().limits.bufferImageGranularity;
    for (auto const &buffer :
         {firstBuffer.memoryInfo(), secondBuffer.memoryInfo()}) {
      for (auto const &image : {colorInfo, depth.memoryInfo()}) {
        if (image.block != buffer.block)
          continue;
        auto lower = image.offset < buffer.offset ? image : buffer;
        auto upper = image.offset < buffer.offset ? buffer : image;
        require(lower.offset + lower.suballocationBytes <= upper.offset &&
                    (lower.offset + lower.suballocationBytes - 1) /
                            granularity <
                        upper.offset / granularity,
                "Mixed optimal image/buffer ranges overlap or share a "
                "granularity page");
      }
    }
    auto replacement = device.createImage(description, payload, local, scope);
    replacement = std::move(color);
    require(!color.valid() &&
                replacement.memoryInfo().block == colorInfo.block &&
                replacement.memoryInfo().offset == colorInfo.offset &&
                ledger.snapshot().current.images == 2,
            "Image move assignment leaked destination or freed source");
    expectFailure([&] { (void)color.memoryInfo(); },
                  "Moved image retains allocation access");
    auto baseline = ledger.snapshot();
    auto invalid = description;
    invalid.extent.width = 0;
    expectFailure(
        [&] { (void)device.createImage(invalid, payload, local, scope); },
        "Zero-extent image accepted");
    invalid = description;
    invalid.flags = vk::ImageCreateFlagBits::eSparseBinding;
    expectFailure(
        [&] { (void)device.createImage(invalid, payload, local, scope); },
        "Sparse ownership accepted");
    expectFailure(
        [&] {
          auto allocation =
              device.createImage(description, payload, local, scope);
          auto viewLease = scope.track({.imageViews = 1});
          vk::raii::ImageView view(
              device.logicalDevice(),
              vk::ImageViewCreateInfo{
                  .image = *allocation.image,
                  .viewType = vk::ImageViewType::e2D,
                  .format = description.format,
                  .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                       1}});
          throw std::runtime_error(
              "Injected candidate failure after view creation");
        },
        "Image candidate did not throw");
    auto afterFailure = ledger.snapshot();
    for (std::size_t i = 0; i < ResourceLedger::domainCount; ++i)
      if (i != std::size_t(ResourceLedger::Domain::AllocatorBlocks))
        require(afterFailure.domains[i] == baseline.domains[i],
                "Image/view failure leaked or changed live resources");
    checkAllocationStatistics(device);
    survivor = std::move(replacement);
  }
  require(ledger.snapshot().current.images == 1 &&
              ledger.snapshot().current.buffers == 0 &&
              ledger.snapshot().current.allocations > 0,
          "Last image did not retain backing after buffers died");
  checkAllocationStatistics(device);
  survivor = GpuImage{};
  require(ledger.snapshot().current == ResourceLedger::Footprint{} &&
              device.gpuAllocationStatistics().blocks == 0,
          "Last image did not release all ranges, blocks and accounting");
  std::cout
      << "PASS image allocator: mixed granularity/alignment, color/depth, "
         "moves, image/view rollback, last-image backing release\n";
}

void verifyUploadedTexel(Device const &device, TextureResources const &texture,
                         std::span<std::byte const> expected) {
  auto before = device.resourceLedger().snapshot().current;
  auto readback = device.createBuffer(
      expected.size(), vk::BufferUsageFlagBits::eTransferDst,
      vk::MemoryPropertyFlagBits::eHostVisible |
          vk::MemoryPropertyFlagBits::eHostCoherent);
  auto now = device.resourceLedger().snapshot().current;
  auto requirement = readback.buffer.getMemoryRequirements();
  auto info = readback.memoryInfo();
  require(now.suballocatedBytes ==
                  before.suballocatedBytes + info.suballocationBytes &&
              now.payloadBytes == before.payloadBytes + expected.size() &&
              now.buffers == before.buffers + 1 &&
              now.suballocations == before.suballocations + 1 &&
              info.suballocationBytes >= requirement.size,
          "Ledger confused buffer payload, suballocation and backing size");
  vk::raii::CommandPool pool(
      device.logicalDevice(),
      vk::CommandPoolCreateInfo{
          .flags = vk::CommandPoolCreateFlagBits::eTransient,
          .queueFamilyIndex = device.graphicsQueueFamilyIndex()});
  vk::raii::CommandBuffers commands(
      device.logicalDevice(),
      vk::CommandBufferAllocateInfo{.commandPool = *pool,
                                    .level = vk::CommandBufferLevel::ePrimary,
                                    .commandBufferCount = 1});
  auto &command = commands.front();
  command.begin(vk::CommandBufferBeginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
  vk::ImageMemoryBarrier2 barrier{
      .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
      .srcAccessMask =
          vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
      .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
      .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
      .newLayout = vk::ImageLayout::eTransferSrcOptimal,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = texture.image(),
      .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
  command.pipelineBarrier2(vk::DependencyInfo{
      .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
  command.copyImageToBuffer(
      texture.image(), vk::ImageLayout::eTransferSrcOptimal, *readback.buffer,
      {vk::BufferImageCopy{
          .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
          .imageExtent = {1, 1, 1}}});
  barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
  barrier.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
  barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
  barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
  barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
  barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  vk::BufferMemoryBarrier2 host{
      .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
      .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
      .dstStageMask = vk::PipelineStageFlagBits2::eHost,
      .dstAccessMask = vk::AccessFlagBits2::eHostRead,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = *readback.buffer,
      .size = expected.size()};
  command.pipelineBarrier2(
      vk::DependencyInfo{.bufferMemoryBarrierCount = 1,
                         .pBufferMemoryBarriers = &host,
                         .imageMemoryBarrierCount = 1,
                         .pImageMemoryBarriers = &barrier});
  command.end();
  vk::raii::Fence fence(device.logicalDevice(), vk::FenceCreateInfo{});
  vk::CommandBuffer raw = *command;
  device.graphicsQueue().submit(
      {vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &raw}},
      *fence);
  require(device.logicalDevice().waitForFences({*fence}, true, UINT64_MAX) ==
              vk::Result::eSuccess,
          "Image readback did not complete");
  std::vector<std::byte> downloaded(expected.size());
  readback.read(downloaded);
  bool equal = std::ranges::equal(expected, downloaded);
  require(equal, "Uploaded GPU pixel differs from source content");
}

void verifyUploadedPixel(Device const &device, TextureResources const &texture,
                         std::array<std::byte, 4> expected) {
  verifyUploadedTexel(device, texture, expected);
}

void exerciseHdrAllocation(Device const &device) {
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "HDR baseline is not empty");
  {
    TextureLoader loader(device);
    std::array<float, 4> pixels{2.0f, 4.0f, 0.25f, 1.0f};
    auto texture = loader.createFromHdrPixels(pixels, 1, 1);
    require(texture.format() == vk::Format::eR32G32B32A32Sfloat,
            "HDR format changed");
    auto current = device.resourceLedger().snapshot().current;
    require(current.images == 1 && current.imageViews == 1 &&
                current.samplers == 1 && current.buffers == 0 &&
                current.payloadBytes == 16 && current.suballocations == 1,
            "HDR or finished upload accounting is incorrect");
    verifyUploadedTexel(device, texture, std::as_bytes(std::span(pixels)));
    checkAllocationStatistics(device);
  }
  require(device.resourceLedger().snapshot().current ==
              ResourceLedger::Footprint{},
          "HDR release did not return to zero");
  std::cout << "PASS HDR image: float texel readback preserves values >1, "
               "independent view/sampler and zero after release\n";
}

void exerciseTextureCache(Device const &device) {
  TextureCache cache(device);
  // Tiny PPM: stb_image decodes this without a test-only encoder dependency.
  std::string ppm = "P6\n1 1\n255\n";
  ppm += std::string{char(20), char(80), char(160)};
  auto bytes = std::as_bytes(std::span(ppm));
  auto path =
      std::filesystem::temp_directory_path() /
      ("vulkan-cache-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".ppm");
  struct RemoveFile {
    std::filesystem::path path;
    ~RemoveFile() {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  } cleanup{path};
  auto write = [&] {
    std::ofstream output(path, std::ios::binary);
    output.write(ppm.data(), ppm.size());
    require(bool(output), "Could not write cache fixture");
  };
  write();
  {
    UploadBatch uploads(device);
    auto beforeShared = device.resourceLedger().snapshot().at(
        ResourceLedger::Domain::SharedTextures);
    auto srgb =
        cache.encoded(bytes, "embedded", TextureColorSpace::Srgb, {}, uploads);
    VkMemoryRequirements imageRequirements{};
    vkGetImageMemoryRequirements(static_cast<VkDevice>(device.deviceHandle()),
                                 static_cast<VkImage>(srgb.image()),
                                 &imageRequirements);
    auto imageLedger = device.resourceLedger().snapshot().at(
        ResourceLedger::Domain::SharedTextures);
    require(imageLedger.allocatedBytes == 0 &&
                imageLedger.suballocatedBytes >=
                    beforeShared.suballocatedBytes + imageRequirements.size &&
                imageLedger.suballocations == beforeShared.suballocations + 1 &&
                imageLedger.payloadBytes == beforeShared.payloadBytes + 4 &&
                imageLedger.images == beforeShared.images + 1 &&
                imageLedger.imageViews == beforeShared.imageViews + 1,
            "Ledger confused decoded texel bytes with actual image allocation "
            "size");
    std::cout << "PASS: 1x1 image payload=4 bytes, Vulkan required range="
              << imageRequirements.size << " bytes\n";

    auto duplicate = cache.encoded(bytes, "another material",
                                   TextureColorSpace::Srgb, {}, uploads);
    auto external =
        cache.file(path.string(), TextureColorSpace::Srgb, {}, uploads);
    TextureSamplerDescription clamp{.mag = TextureFilter::Nearest,
                                    .u = TextureWrap::ClampToEdge};
    auto clamped = cache.encoded(bytes, "clamped", TextureColorSpace::Srgb,
                                 clamp, uploads);
    auto linear = cache.encoded(bytes, "packed data", TextureColorSpace::Linear,
                                {}, uploads);
    require(srgb.image() == duplicate.image() &&
                srgb.imageView() == external.imageView() &&
                srgb.sampler() == duplicate.sampler(),
            "Same-content materials did not share image/view/sampler");
    require(srgb.imageView() == clamped.imageView() &&
                srgb.sampler() != clamped.sampler(),
            "Sampler selection duplicated the image or ignored filtering/wrap");
    require(linear.image() != srgb.image() &&
                linear.format() == vk::Format::eR8G8B8A8Unorm &&
                srgb.format() == vk::Format::eR8G8B8A8Srgb &&
                linear.sampler() == srgb.sampler(),
            "Color and data texture identities were mixed");
    ppm.back() = char(60);
    write();
    auto changed =
        cache.file(path.string(), TextureColorSpace::Srgb, {}, uploads);
    require(changed.image() != srgb.image(),
            "File changed in place reused stale texture content");
    auto white =
        cache.solid({255, 255, 255, 255}, TextureColorSpace::Linear, uploads);
    auto whiteAgain =
        cache.solid({255, 255, 255, 255}, TextureColorSpace::Linear, uploads);
    auto whiteColor =
        cache.solid({255, 255, 255, 255}, TextureColorSpace::Srgb, uploads);
    require(white.image() == whiteAgain.image() &&
                white.image() != whiteColor.image(),
            "Fallback texture sharing/color identity is incorrect");
    auto stats = uploads.finish();
    require(stats.imageCopies == 5 && stats.submissions == 1 &&
                stats.fenceWaits == 1,
            "Texture cache did not upload five distinct images in one batch");
    verifyUploadedPixel(
        device, srgb,
        {std::byte{20}, std::byte{80}, std::byte{160}, std::byte{255}});
    verifyUploadedPixel(
        device, changed,
        {std::byte{20}, std::byte{80}, std::byte{60}, std::byte{255}});
    verifyUploadedPixel(
        device, white,
        {std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}});
    UploadBatch cachedOnly(device);
    auto reused =
        cache.file(path.string(), TextureColorSpace::Srgb, {}, cachedOnly);
    auto emptyStats = cachedOnly.finish();
    require(reused.image() == changed.image() && emptyStats.submissions == 0 &&
                emptyStats.bytes == 0,
            "Cache-only request submitted an unnecessary upload");
  }
  {
    TextureCache isolated(device);
    UploadBatch abandoned(device), independent(device);
    auto first = isolated.encoded(bytes, "unsubmitted candidate",
                                  TextureColorSpace::Srgb, {}, abandoned);
    auto second = isolated.encoded(bytes, "independent candidate",
                                   TextureColorSpace::Srgb, {}, independent);
    require(first.image() != second.image(),
            "Independent candidate reused an unsubmitted image");
    independent.finish();
    verifyUploadedPixel(
        device, second,
        {std::byte{20}, std::byte{80}, std::byte{60}, std::byte{255}});
    // abandoned is intentionally never submitted; second remains correct.
  }
  cache.pruneExpired();
  require(cache.imageEntries() == 0 && cache.samplerEntries() == 0,
          "Weak cache retained unused scene resources");
  std::cout
      << "PASS: shared images and independent samplers; 5 images in 1 batch, "
         "GPU pixel readback, file edits and unused-resource reclamation\n";
}

// Timeline waits may be submitted before a future host signal. A host Event
// cannot legally gate pending commands. See Vulkan
// vkSignalSemaphore/vkSetEvent.
void exerciseCanceledUpload(Renderer &renderer, Device const &device,
                            AssetLibrary const &assets) {
  if (!device.timelineSemaphoreSupported()) {
    std::cout << "SKIP subcase: timeline upload gate unsupported\n";
    return;
  }
  device.logicalDevice().waitIdle(); // Isolate this deterministic test gate.
  renderer
      .collectCompletedWork(); // Drain old timestamp readback before gating.
  auto ledgerBaseline = renderer.resourceSnapshot();
  auto candidate = renderer.prepareScene(assets);
  std::weak_ptr<Renderer::SceneAssets> canceled = candidate;
  vk::SemaphoreTypeCreateInfo type{.semaphoreType =
                                       vk::SemaphoreType::eTimeline};
  vk::raii::Semaphore gate(device.logicalDevice(),
                           vk::SemaphoreCreateInfo{.pNext = &type});
  std::uint64_t value = 1;
  vk::TimelineSemaphoreSubmitInfo timeline{.waitSemaphoreValueCount = 1,
                                           .pWaitSemaphoreValues = &value};
  vk::Semaphore raw = *gate;
  vk::PipelineStageFlags stages = vk::PipelineStageFlagBits::eAllCommands;
  device.graphicsQueue().submit({vk::SubmitInfo{.pNext = &timeline,
                                                .waitSemaphoreCount = 1,
                                                .pWaitSemaphores = &raw,
                                                .pWaitDstStageMask = &stages}},
                                nullptr);
  auto release = [&] {
    device.logicalDevice().signalSemaphore(
        vk::SemaphoreSignalInfo{.semaphore = raw, .value = value});
    device.logicalDevice().waitIdle();
  };
  try {
    auto commits = renderer.resourceStatistics().sceneCommits;
    require(!renderer.commitScene(candidate),
            "Submission committed a blocked candidate");
    require(!renderer.commitScene(candidate),
            "Polling waited through the unsignaled gate");
    candidate
        .reset(); // No wait in cancellation; renderer retains destinations.
    renderer.collectCompletedWork();
    require(!canceled.expired() &&
                renderer.resourceStatistics().pendingSceneUploads == 1,
            "Canceled pending upload destroyed destinations before its fence");
    require(renderer.resourceStatistics().sceneCommits == commits &&
                renderer.resourceStatistics().sceneUploadFenceWaits == 0,
            "Cancellation changed live assets or waited for upload completion");
    auto held = renderer.resourceSnapshot();
    require(held.at(ResourceLedger::Domain::PreparedScene).buffers ==
                    assets.meshes.size() * 2 + assets.materials.size() &&
                held.at(ResourceLedger::Domain::Staging).suballocatedBytes > 0,
            "Canceled pending upload vanished from the resource ledger");
    release();
  } catch (...) {
    release();
    throw;
  }
  renderer.collectCompletedWork();
  require(canceled.expired() &&
              renderer.resourceStatistics().pendingSceneUploads == 0,
          "Completed canceled upload was not reclaimed");
  auto reclaimed = renderer.resourceSnapshot();
  for (std::size_t i = 0; i < ResourceLedger::domainCount; ++i)
    if (i != static_cast<std::size_t>(ResourceLedger::Domain::AllocatorBlocks))
      require(
          reclaimed.domains[i] == ledgerBaseline.domains[i],
          "Canceled upload did not restore resource/suballocation baseline");
  std::cout << "PASS: unsignaled timeline gate, nonblocking "
               "polling/cancellation, safe upload reclamation\n";
}

void exercise(Renderer &renderer, Device const &device,
              AssetLibrary const &assets) {
  auto initial = renderer.resourceStatistics();
  require(initial.environmentUploads == 1 && initial.pipelineBuilds == 13 + (renderer.clusterSupported() ? 1 : 0),
          "Initial environment and material pipelines are missing");
  unsigned uiCalls = 0, releases = 0;
  renderer.setUiDrawCallback([&](vk::CommandBuffer) { ++uiCalls; });
  Camera camera;
  camera.position = {0, 0, 4};
  camera.target = {0, 0, 0};
  LightingSettings lighting;
  lighting.shadowDebugMode = 0;
  std::uint64_t frameId = 0;
  auto begin = [&] {
    glfwPollEvents();
    require(renderer.beginFrame(camera.viewProj(4.f / 3.f), camera.position,
                                lighting,
                                false) == Renderer::FrameResult::eSuccess,
            "Begin frame failed");
    renderer.drawEnvironment();
  };
  auto finish = [&](MeshId mesh = 0) {
    renderer.drawObject(mesh, 0, glm::mat4(1));
    require(renderer.endFrame() != Renderer::FrameResult::eSwapChainOutOfDate,
            "Unexpected swapchain change");
    require(renderer.submittedFrameId() == ++frameId,
            "Scene replacement reset the submitted frame sequence");
  };
  auto draw = [&](MeshId mesh = 0) {
    begin();
    finish(mesh);
  };
  auto commitWhenReady = [&](Renderer::PreparedScene &candidate,
                             std::function<void()> releaseUi = {}) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!renderer.commitScene(candidate, releaseUi)) {
      require(std::chrono::steady_clock::now() < deadline,
              "Upload did not become ready");
      std::this_thread::sleep_for(
          std::chrono::milliseconds(1)); // Test-only poll loop.
    }
  };
  std::uint64_t geometryBytes = 0;
  for (auto const &mesh : assets.meshes)
    geometryBytes += mesh.vertices.size() * sizeof(Vertex) +
                     mesh.indices.size() * sizeof(std::uint32_t);
  auto privateBytes =
      geometryBytes + assets.materials.size() *
                          sizeof(MaterialGpuStore::MaterialUniformBufferObject);
  auto candidate = renderer.prepareScene(assets);
  auto preparedLedger = renderer.resourceSnapshot();
  auto const &prepared =
      preparedLedger.at(ResourceLedger::Domain::PreparedScene);
  require(prepared.payloadBytes == privateBytes &&
              prepared.suballocatedBytes >= privateBytes &&
              prepared.buffers ==
                  assets.meshes.size() * 2 + assets.materials.size() &&
              prepared.descriptorPools == 1 &&
              prepared.descriptorSets == assets.materials.size(),
          "Prepared geometry/UBO/descriptors do not match physical resources");
  require(preparedLedger.at(ResourceLedger::Domain::Staging).payloadBytes >=
              geometryBytes,
          "Recorded staging bytes disappeared before submission");
  std::weak_ptr<Renderer::SceneAssets> live = candidate;
  require(renderer.resourceStatistics().sceneUploadSubmissions == 0,
          "Resource preparation submitted from a worker-capable method");
  require(!renderer.commitScene(candidate),
          "Initial upload bypassed pending state");
  commitWhenReady(candidate);
  auto firstUpload = renderer.resourceStatistics().lastSceneUpload;
  require(firstUpload.submissions == 1 && firstUpload.fenceWaits == 0,
          "Initial scene waited instead of polling its upload fence");
  // The fixture reuses one encoded image for normal and packed data: their mip
  // semantics now require independent storage, in addition to sRGB storage.
  require(firstUpload.imageCopies == 7,
          "Initial scene lost typed texture identity or uploaded duplicates");
  draw();
  auto settledLedger = renderer.resourceSnapshot();
  require(
      settledLedger.at(ResourceLedger::Domain::LiveScene).payloadBytes ==
              privateBytes &&
          settledLedger.at(ResourceLedger::Domain::PreparedScene)
                  .suballocatedBytes == 0 &&
          settledLedger.at(ResourceLedger::Domain::Staging).suballocatedBytes ==
              0,
      "Initial commit left staging or private candidate allocations behind");
  auto oldImage = renderer.materialAlbedoTexture(0).image();
  auto beforeFailure = renderer.resourceStatistics().sceneUploadSubmissions;
  auto broken = assets;
  broken.materials[0].albedoBytes = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto failedPreparation = std::async(
      std::launch::async, [&] { return renderer.prepareScene(broken); });
  draw(); // Old scene remains drawable while an independent task prepares
          // resources.
  expectFailure([&] { (void)failedPreparation.get(); },
                "Corrupt texture was accepted");
  require(renderer.materialAlbedoTexture(0).image() == oldImage &&
              renderer.resourceStatistics().sceneUploadSubmissions ==
                  beforeFailure,
          "Background decode failure changed or uploaded the live scene");
  auto rolledBack = renderer.resourceSnapshot();
  for (std::size_t i = 0; i < ResourceLedger::domainCount; ++i)
    if (i != static_cast<std::size_t>(ResourceLedger::Domain::AllocatorBlocks))
      require(rolledBack.domains[i] == settledLedger.domains[i],
              "Decode rollback leaked resources/suballocations");
  auto malformed = assets;
  malformed.meshes[0].indices[0] =
      static_cast<std::uint32_t>(malformed.meshes[0].vertices.size());
  expectFailure([&] { (void)renderer.prepareScene(malformed); },
                "Invalid geometry was accepted");
  Renderer::PreparedScene empty;
  expectFailure([&] { (void)renderer.commitScene(empty); },
                "Null candidate was accepted");
  exerciseCanceledUpload(renderer, device, assets);

  auto replacement = assets;
  replacement.meshes.push_back(assets.meshes.front());
  replacement.materials[0].tint = {.2f, .8f, .4f, 1};
  begin();
  auto preparation = std::async(
      std::launch::async, [&] { return renderer.prepareScene(replacement); });
  candidate =
      preparation
          .get(); // Independent pool/cache work is valid while recording.
  expectFailure([&] { (void)renderer.commitScene(candidate); },
                "Commit during recording was accepted");
  require(candidate != nullptr,
          "Rejected commit consumed the recorded candidate");
  finish();
  for (unsigned i = 0; i < 3; ++i) {
    if (i)
      candidate = renderer.prepareScene(replacement);
    auto beforeCommitLedger = renderer.resourceSnapshot();
    require(beforeCommitLedger.at(ResourceLedger::Domain::SharedTextures) ==
                settledLedger.at(ResourceLedger::Domain::SharedTextures),
            "Shared images/samplers were double-counted across candidates");
    auto oldPrivate = beforeCommitLedger.at(ResourceLedger::Domain::LiveScene);
    auto newPrivate =
        beforeCommitLedger.at(ResourceLedger::Domain::PreparedScene);
    auto nextLive = std::weak_ptr<Renderer::SceneAssets>(candidate);
    auto oldLive = live;
    unsigned const priorReleases = releases;
    require(!renderer.commitScene(candidate),
            "New upload skipped pending submission state");
    require(renderer.materialAlbedoTexture(0).image() == oldImage,
            "Pending upload changed active materials");
    commitWhenReady(candidate, [&, oldLive, lastFrame = frameId] {
      require(!oldLive.expired(),
              "Old assets died before preview descriptor removal");
      require(renderer.resourceStatistics().completedFrameId >= lastFrame,
              "Preview removal preceded completion of the last old frame");
      ++releases;
    });
    require(releases == priorReleases && !oldLive.expired() &&
                renderer.resourceStatistics().retiredScenes > 0,
            "Commit destroyed old resources or ran retirement inline");
    require(renderer.materialAlbedoTexture(0).image() == oldImage,
            "Same-content reload uploaded a duplicate image");
    auto committedLedger = renderer.resourceSnapshot();
    require(committedLedger.at(ResourceLedger::Domain::RetiredScene) ==
                    oldPrivate &&
                committedLedger.at(ResourceLedger::Domain::LiveScene) ==
                    newPrivate &&
                committedLedger.at(ResourceLedger::Domain::Staging)
                        .suballocatedBytes == 0 &&
                committedLedger.peak.allocatedBytes >=
                    beforeCommitLedger.current.allocatedBytes &&
                committedLedger.peak.suballocatedBytes >=
                    beforeCommitLedger.current.suballocatedBytes,
            "Commit lost old/new coexistence or staging peak accounting");
    auto upload = renderer.resourceStatistics().lastSceneUpload;
    require(upload.imageCopies == 0 &&
                upload.bufferCopies == replacement.meshes.size() * 2 &&
                upload.submissions == 1 && upload.fenceWaits == 0,
            "Reload did not use a nonblocking shared-image batch");
    device.logicalDevice()
        .waitIdle(); // Test-only: now the collector must retire.
    renderer.collectCompletedWork();
    require(oldLive.expired() && releases == priorReleases + 1,
            "Completed old assets/previews were not reclaimed");
    require(renderer.resourceSnapshot()
                    .at(ResourceLedger::Domain::RetiredScene)
                    .suballocatedBytes == 0,
            "Retired resources remained in the ledger after reclamation");
    live = nextLive;
    draw(1);
  }
  device.logicalDevice().waitIdle();
  renderer.collectCompletedWork();
  auto finalLedger = renderer.resourceSnapshot();
  require(finalLedger.at(ResourceLedger::Domain::SharedTextures) ==
                  settledLedger.at(ResourceLedger::Domain::SharedTextures) &&
              finalLedger.at(ResourceLedger::Domain::Staging).buffers == 0,
          "Repeated scene replacement grew shared storage or leaked staging");
  std::cout << "PASS: exact payload/allocation ledger; shared dedup, rollback, "
               "cancellation, retirement and coexistence peak\n";
  auto final = renderer.resourceStatistics();
  require(final.sceneCommits == 4 && releases == 3 && uiCalls == frameId,
          "Commit/release/UI frame counts changed unexpectedly");
  require(final.sceneImageCopies == firstUpload.imageCopies &&
              final.sceneUploadFenceWaits == 0 &&
              final.pendingSceneUploads == 0 && final.retiredScenes == 0,
          "Reloads duplicated images, blocked or leaked retired resources");
  require(final.environmentUploads == initial.environmentUploads &&
              final.pipelineBuilds == initial.pipelineBuilds,
          "Scene switching recreated environment or pipelines");
  std::cout << "PASS: " << final.sceneCommits << " commits, " << frameId
            << " frames; background rollback, deferred preview retirement; "
               "upload batches="
            << final.sceneUploadSubmissions
            << ", image copies=" << final.sceneImageCopies
            << ", upload fence waits=" << final.sceneUploadFenceWaits << '\n';
}
} // namespace

int main(int argc, char **argv) {
  if (glfwInit() != GLFW_TRUE) {
    char const *message = nullptr;
    glfwGetError(&message);
    std::cerr << "SKIP: no graphical session: "
              << (message ? message : "unknown") << '\n';
    return 77;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
  GLFWwindow *window = glfwCreateWindow(
      320, 240, "GPU scene transaction regression", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 77;
  }
  // Complete initial native window events before borrowing it for a surface.
  glfwPollEvents();
  int status = 1;
  try {
    vk::raii::Context context;
    unsigned extensionCount = 0;
    auto glfwExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
    require(glfwExtensions && extensionCount,
            "No Vulkan window-system extensions");
    std::vector<char const *> extensions(glfwExtensions,
                                         glfwExtensions + extensionCount);
    auto policy = parsePresentationPolicy(argc > 2 ? argv[2] : "auto");
    auto presentationInstance = enablePresentationInstanceExtensions(
        context.enumerateInstanceExtensionProperties(), extensions, policy);
    bool extOnly = argc > 3 && std::string_view(argv[3]) == "ext";
    if (extOnly) {
      presentationInstance.khr = false;
      std::erase_if(extensions, [](auto name) {
        return std::strcmp(name, vk::KHRSurfaceMaintenance1ExtensionName) == 0;
      });
    }
    vk::ApplicationInfo info{.pApplicationName = "Scene transaction regression",
                             .apiVersion = vk::ApiVersion13};
    vk::raii::Instance instance(
        context,
        vk::InstanceCreateInfo{.pApplicationInfo = &info,
                               .enabledExtensionCount =
                                   static_cast<unsigned>(extensions.size()),
                               .ppEnabledExtensionNames = extensions.data()});
    VkSurfaceKHR rawSurface{};
    require(glfwCreateWindowSurface(static_cast<VkInstance>(*instance), window,
                                    nullptr, &rawSurface) == VK_SUCCESS,
            "Surface creation failed");
    vk::raii::SurfaceKHR surface(instance, rawSurface);
    Device device(instance, surface, {vk::KHRSwapchainExtensionName}, {}, false,
                  presentationInstance, policy);
    if (extOnly &&
        device.presentationSupport().backend != PresentationBackend::ExtFence) {
      status = 77;
      throw std::runtime_error("SKIP EXT backend: matching instance/device "
                               "extension or feature unavailable");
    }
    std::cout << "Presentation backend: "
              << presentationBackendName(device.presentationSupport().backend)
              << " (" << device.presentationSupport().reason << ")\n";
    SwapChain swapchain(device, surface, window);
    unsigned framesInFlight = argc > 1 ? std::stoul(argv[1]) : 1;
    if (argc > 3 && std::string_view(argv[3]) == "taa") {
      exerciseTaaResolve(device,swapchain,framesInFlight);
    } else if (argc > 3 && std::string_view(argv[3]) == "motion") {
      exerciseTemporalMotion(device, swapchain, framesInFlight);
      exerciseFrameContexts(device, swapchain);
    } else if (argc > 3 && std::string_view(argv[3]) == "csm") {
      exerciseSunCascades(device, swapchain, framesInFlight);
      exerciseFrameContexts(device, swapchain);
    } else if (argc > 3 && std::string_view(argv[3]) == "indoor") {
      exerciseIndoorLighting(device, swapchain, framesInFlight);
    } else if (argc>5 && (std::string_view(argv[3])=="kitchen" || std::string_view(argv[3])=="kitchen-colour")) {
      exerciseKitchenScene(device,swapchain,framesInFlight,argv[4],argv[5],std::string_view(argv[3])=="kitchen-colour");
    } else {
    exerciseGpuAllocator(device);
    exerciseImageAllocator(device);
    exerciseHdrAllocation(device);
    exerciseTextureMips(device);
    exerciseHdrOutput(device);
    exercisePresentation(device, swapchain, framesInFlight);
    exerciseHdrScene(device, swapchain, framesInFlight);
    exerciseMaterialContract(device, swapchain, framesInFlight);
    exerciseEnvironmentIbl(device, swapchain, framesInFlight);
    exerciseSpecularExtension(device, swapchain, framesInFlight);
    exerciseSpecularAa(device, swapchain, framesInFlight);
    exercisePunctualLights(device, swapchain, framesInFlight);
    exerciseFrameContexts(device, swapchain);
    {
      std::unique_ptr<SwapChain> resized;
      Renderer renderer(device, framesInFlight);
      renderer.recreateForSwapChain(swapchain);
      auto imported = loadStaticGltfScene(TEST_FIXTURE, {});
      AssetLibrary assets{.meshes = std::move(imported.meshes),
                          .materials = std::move(imported.materials)};
      try {
        exerciseTextureCache(device);
        exercise(renderer, device, assets);
        verifyDrainedFrameContexts(renderer);
        renderer.setUiDrawCallback({});
        auto baseline = renderer.resourceSnapshot();
        auto sceneStats = renderer.resourceStatistics();
        for (auto size : {vk::Extent2D{400, 300}, vk::Extent2D{640, 360},
                          swapchain.extent()}) {
          device.logicalDevice()
              .waitIdle(); // Test-only, matching resize's drain.
          glfwSetWindowSize(window, size.width, size.height);
          auto deadline =
              std::chrono::steady_clock::now() + std::chrono::seconds(5);
          int width = 0, height = 0;
          do {
            glfwPollEvents();
            glfwGetFramebufferSize(window, &width, &height);
            require(std::chrono::steady_clock::now() < deadline,
                    "Resize event did not arrive");
            if (width != int(size.width) || height != int(size.height))
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
          } while (width != int(size.width) || height != int(size.height));
          auto const beforeResizeFrame = renderer.submittedFrameId();
          auto beforeResizeResult = renderer.renderFrame(
              {.sky = true}, glm::mat4(1), {0, 0, 2}, {}, false);
          std::cout << "Resize old-generation acquire/present result: "
                    << int(beforeResizeResult) << ", submitted delta="
                    << (renderer.submittedFrameId() - beforeResizeFrame)
                    << '\n';
          auto &old = resized ? *resized : swapchain;
          old.drainPresentations();
          require(old.presentationStatistics().pendingFences == 0 &&
                      old.presentationReleaseProven() ==
                          device.presentationSupport().fencesEnabled(),
                  "Old generation was not drained before replacement");
          auto candidate = std::make_unique<SwapChain>(
              device, surface, window,
              resized ? *resized->handle() : *swapchain.handle());
          require(candidate->extent() == size,
                  "Swapchain extent differs from resized framebuffer");
          renderer.recreateForSwapChain(*candidate);
          resized = std::move(candidate);
          auto snapshot = renderer.resourceSnapshot();
          auto before = baseline.at(ResourceLedger::Domain::Persistent);
          auto after = snapshot.at(ResourceLedger::Domain::Persistent);
          auto expected = before.payloadBytes -
                          std::uint64_t(swapchain.extent().width) *
                              swapchain.extent().height * 44 +
                          std::uint64_t(size.width) * size.height * 44;
          if (after.payloadBytes != expected || after.images != before.images ||
              after.imageViews != before.imageViews)
            std::cerr << "Resize " << size.width << 'x' << size.height
                      << " initial=" << swapchain.extent().width << 'x'
                      << swapchain.extent().height
                      << " persistent payload before=" << before.payloadBytes
                      << " after=" << after.payloadBytes
                      << " expected=" << expected << " images=" << before.images
                      << "->" << after.images << " views=" << before.imageViews
                      << "->" << after.imageViews << '\n';
          require(after.payloadBytes == expected &&
                      after.images == before.images &&
                      after.imageViews == before.imageViews,
                  "Resize retained old depth payload/image/view");
          require(snapshot.at(ResourceLedger::Domain::SharedTextures) ==
                      baseline.at(ResourceLedger::Domain::SharedTextures),
                  "Resize changed shared textures");
          require(snapshot.at(ResourceLedger::Domain::LiveScene) ==
                      baseline.at(ResourceLedger::Domain::LiveScene),
                  "Resize changed live scene");
          Camera camera;
          camera.position = {0, 0, 4};
          camera.target = {0, 0, 0};
          require(renderer.beginFrame(
                      camera.viewProj(float(size.width) / size.height),
                      camera.position, {},
                      false) == Renderer::FrameResult::eSuccess,
                  "Frame failed after resize");
          renderer.drawEnvironment();
          renderer.drawObject(0, 0, glm::mat4(1));
          require(renderer.endFrame() !=
                      Renderer::FrameResult::eSwapChainOutOfDate,
                  "Resized rendering unexpectedly out of date");
          device.logicalDevice().waitIdle();
          checkAllocationStatistics(device);
        }
        require(renderer.resourceStatistics().environmentUploads ==
                        sceneStats.environmentUploads &&
                    renderer.resourceStatistics().sceneCommits ==
                        sceneStats.sceneCommits &&
                    renderer.resourceSnapshot().current.suballocatedBytes ==
                        baseline.current.suballocatedBytes,
                "Resize reuploaded assets or retained old depth ranges");
        std::cout << "PASS resize: 400x300, 640x360, original "
                  << swapchain.extent().width << 'x'
                  << swapchain.extent().height
                  << "; renders after each, depth ranges restored, "
                     "scene/shared storage stable\n";
      } catch (...) {
        device.logicalDevice().waitIdle();
        throw;
      }
    }
    }
    require(device.resourceLedger().snapshot().current ==
                ResourceLedger::Footprint{},
            "Renderer destruction did not release every tracked resource");
    status = 0;
  } catch (std::exception const &error) {
    std::cerr << error.what() << '\n';
  }
  // Surface/swapchain/renderer have died before their GLFW window.
  glfwDestroyWindow(window);
  glfwTerminate();
  return status;
}
