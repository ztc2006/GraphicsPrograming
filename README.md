# Vulkan scene viewer

The confirmed development route targets Linux, native 1080p / 60 FPS and
high-quality static glTF/GLB scenes. See the [roadmap](Engine_Roadmap.md),
[design review](Renderer_Design_Review.md) and [execution checklist](Renderer_Refactor_Checklist.md).
M0 measurement tools and material fixtures are implemented. Software-device
smoke tests and RenderDoc capture pass. Hardware acceptance is deferred until the
user reconnects the RTX 4060 Ti host and requests testing. Development continues:
M1 keeps the Renderer persistent, prepares loads in the background, retires old
scene/preview resources after frame completion, shares images/samplers and batches each
scene upload into one submission. Hardware
performance/VRAM and final motion-quality acceptance remain open. M1-C now uses
a pinned cgltf adapter for sparse/normalized data, authored tangents and per-texture
UV transforms; unsupported required extensions fail, and optional fallback
warnings appear with the committed scene. See the [library decision](docs/M1_C_Library_Decision.md).
Buffers and Texture/HDR/depth/shadow images now use the shared project VMA
adapter. Buffer read/write remains bounded and allocation-relative; upload
memory has a separate lifetime. The resource ledger separates unique backing blocks
from buffer/image suballocations. Resize, cancellation/retirement and final zero
are tested; see the [image allocator contract](docs/M1_C_VMA_Image_Adapter.md).
M2-A now renders scene, sky and transparent surfaces into linear RGBA16F, then
applies one exposure/tone-map/sRGB display output before UI. See the
[HDR decision](docs/M2_A_HDR_Decision.md) and
[implementation contract](docs/M2_A_HDR_Implementation.md). M2-B next migrates
passes into the minimal single-queue Render Graph.

Build and open the viewer:

```bash
./run.sh
```

The default launch incrementally builds the executable and shaders. It opens an
empty scene viewer; use the **Scene** panel to browse glTF, GLB or OBJ models,
enter a path, or drop a model file onto the window. Load and Reload replace the
current scene. A failed import or texture upload keeps the previous scene and
shows the error in the panel. Imported bounds determine the initial camera and
shadow coverage.

```bash
./run.sh assets/models/siheyuan/source/siheyuan.glb
./run.sh --no-build                         # reuse the existing executable
./run.sh --build-only -j 4                  # build without launching
./run.sh --debug assets/models/siheyuan/source/siheyuan.glb
ctest --test-dir build-linux --output-on-failure
```

Model paths passed to the launcher are relative to the caller's working
directory. The executable also resolves model arguments before selecting its
build-time resource directory, so direct executable launches have the same
shader and environment paths. Startup
and UI model loads share the same importer and GPU upload path. There is no
implicit model selection or environment-variable model override.

The glTF loader follows each texture's texCoord attribute, including
TEXCOORD_5 used by the siheyuan model. Base color, normal, metallic-roughness,
occlusion and emissive maps may each select a different coordinate set.
Embedded GLB images and image data URIs are used as authored; exported images
are not substituted by filename guesses. Node hierarchy matrices are retained
without decomposition, preserving rotation, nonuniform scale and shear.

Rendering includes direct Cook-Torrance PBR and SH diffuse environment lighting.
The **Lighting** panel separates **Environment Intensity** (sky and IBL radiance)
from **Exposure EV** (whole-scene display exposure; +1 EV doubles input radiance)
and **Tone Mapping**. Data debug views bypass exposure and tone mapping.
Specular IBL currently uses an approximation; GGX environment prefiltering and a
split-sum BRDF LUT are still pending. Lighting's PBR Debug selector exposes base
color, metallic, roughness, normals, AO and environment-light components.


## Repeatable M0 measurements

Build an optimized executable and record a fixed scene/camera configuration:

```bash
./run.sh --release --benchmark benchmarks/siheyuan-orbit-01 \
  --gpu "RTX 4060 Ti" --size 1920x1080 --warmup 30 --duration 120 \
  --camera-path orbit --present immediate \
  assets/models/siheyuan/source/siheyuan.glb
```

Repeat three times using different output directories. Run a separate `--present
fifo` series for normal VSync behavior. A requested present mode must be supported;
`--gpu` requires a matching suitable device and fails if it cannot find one.
`--no-build` reuses the existing binary and its build configuration; use
`--release` with a build before collecting optimized results.

Each run writes `frames.csv` and `summary.json`, refuses to overwrite a previous
report and exits after the requested duration. Reports include the actual device,
driver/API, build type, a SHA-256 fingerprint of project C++/shader sources,
framebuffer size, initial camera, environment intensity, exposure EV, tone mapping,
scene color format, load time, draw counts and p50/p95/p99. Report schema 3 adds
`gpu_output_ms`, `exposure_ev`, `tone_mapping_enabled` and `scene_color_format`;
`environment_intensity` replaces the misleading `environment_exposure` key.
CPU frame/preparation, fence/acquire/submit/present call time and GPU total/shadow/
main/output/UI intervals are recorded separately. GPU query results are matched to their
submitted frame after the existing fence completes. Unsupported timing is marked
missing; disabled UI has zero GPU UI time.

Device-local heap capacity and optional driver-reported memory-budget samples
are included. The sampled peak includes the old/candidate resource coexistence
point, but is not exact application VRAM accounting and can include other
processes. On a software Vulkan device these heaps may represent system memory.
The report never automatically accepts the hardware target: review image quality,
completed runs, timing coverage and the repeated hardware measurements first.

Benchmark mode locks scene/camera controls and rejects framebuffer-size or
swapchain changes. If Wayland decorations change the requested framebuffer size,
use an X11/Xwayland session or adjust the session's window configuration; the
size check prevents accidentally reporting another resolution as native 1080p.
Use `--validation` for a separate correctness run, including in Release. Check
`validation_enabled` in the report: a missing layer is reported and does not
establish a zero-error validation result.

```bash
./run.sh --release assets/render_tests/material_baseline.gltf
python3 tools/generate_render_baseline.py
python3 tests/viewer_benchmark_smoke.py build-linux/vulkan tests/fixtures/uv_transform.gltf
```

The material fixture has dielectric/metal spheres with roughness 0.05–1.0,
repeated high-frequency UVs, alpha mask/blend and a mirrored single-sided plane.
Existing glTF and HDR tests cover additional import semantics. These are diagnostic
inputs; the current provisional specular IBL, filtering and mirrored geometry
still need reference-image acceptance. The graphical smoke test requires a display
and timestamp-capable Vulkan device and is intentionally separate from CTest.
RenderDoc receives Frame/Shadow/Main/Display output/UI labels and named resources. Capture is
used for correctness inspection; measure performance in ordinary runs.


## Scene replacement and uploads (M1)

Startup, Scene Load/Reload and file drop call the same prepare/commit path.
Preparation builds a candidate mesh/material/descriptor set without modifying
the active scene. Runtime preparation runs in one background task, recording
independent uploads without submitting to the queue. The main thread submits
once, polls upload readiness and commits outside frame recording. Commit retains
old assets and preview descriptors until their last submitted frame completes.
A preparation failure leaves the previous scene usable. Environment, pipelines,
frame resources, shadow target, UI draw callback and frame numbering persist.
Render Debug reports scene commits, environment uploads, pipeline builds and the
last scene upload's submissions, image/buffer copies and staging bytes.

M1-B shares image/view storage by exact encoded content and color space. File
and embedded sources with identical bytes share storage; edits at the same path
produce new storage. Samplers have independent filter/wrap identities and can
share across images. Bindings hold strong references; renderer-local caches use
weak references and prune expired entries, so a cache does not pin old GPU assets.
Four shared fallback images cover normal, height, linear white and sRGB white.
The exact-content cache currently retains encoded keys in CPU memory while entries
exist; a future asset adapter can replace that duplication with interned sources.

Mesh and missing-image copies record into one UploadBatch. Staging stays alive
until one submission fence completes; decode/descriptor failure discards the
unsubmitted candidate. Repeated scene preparation reuploads geometry but reuses
unchanged images. The old per-resource queue.waitIdle upload path is removed.
Runtime loading, upload polling and scene retirement avoid explicit fence waits.
Cold startup and shutdown may wait. Latest requests supersede earlier jobs;
Cancel discards a result without destroying an active future. Already submitted
uploads remain owned by Renderer until completion, even after cancellation. A
cache publication ticket prevents reuse of another candidate's unsubmitted image.

The resource ledger records successful engine-owned allocations at their actual
lifetime boundaries, including background preparation. Render Debug and benchmark
`summary.json` expose current and high-water counts/bytes for persistent resources,
prepared/live/retired scene-private resources, unique shared textures/samplers and
staging. Shared bindings do not multiply physical storage. Payload bytes describe
buffer contents or uncompressed texels; allocated bytes count unique VMA
backing blocks. Resource domains own buffer/image suballocation ranges,
including alignment; those ranges are not added to backing bytes. Memory type property totals
may overlap on UMA; do not add device-local and host-visible totals together.
Peaks are independent high-water marks, and domain peaks need not occur together.

Scope includes engine buffers, images/views, samplers and frame/material descriptor
pools/sets. Swapchain storage, ImGui backend allocations, driver object overhead and
CPU assets/cache keys are excluded. This is an ownership/allocation ledger, not
GPU residency or total VRAM. Driver heap usage/budget remains a separate sampled
metric. VMA backing allocations belong to `allocator_blocks`; resource domains record
payloads and suballocations. Final resource release also releases allocator blocks.

glTF mag/min/mip filter choices and wrapS/wrapT now remain separate per texture;
invalid sampler indices/filter/wrap values produce errors. Images still have one
mip level with maxLod=0. Typed mip generation, anisotropy and alpha coverage are
M3 work, so this change does not resolve the diagnostic ground's minification
aliasing by itself.

An optional graphical regression exercises the real Renderer, including texture
decode failure after geometry recording, invalid geometry, commit rejection while
recording, cancellation with a deliberately pending upload, deferred preview
release, repeated replacement and UI callback preservation. A second regression
drives the actual Application load/cancel queue with the real ImGui backend:

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DVULKAN_GPU_TESTS=ON
cmake --build build-linux --parallel 4
ctest --test-dir build-linux -L gpu --output-on-failure
```

It runs on a suitable Vulkan 1.3 device, including llvmpipe, and skips explicitly
if no graphical session is available. The asynchronous M1-B run passed on
llvmpipe: four commits and six frames, five scene upload submissions, six scene
image copies, zero runtime upload fence waits, one environment upload and nine
pipeline builds (including the M2-A display output). Cache tests check file/embedded reuse, distinct samplers,
color-space separation, in-place edits, GPU pixel readback, unpublished-image
isolation and unused-resource reclamation. Application tests cover queued reload,
latest-request failure, cancellation before/after submission, preview retirement
and shutdown during preparation. Resource ledger regressions additionally verify exact mesh/UBO payloads, Vulkan
allocation alignment, shared deduplication, rollback, pending cancellation, retired
resource reclamation and zero counters after Renderer destruction. This verifies
functionality; RTX 4060 Ti performance testing waits for the user's device-switch notification. Use `ctest --test-dir build-linux
-LE gpu --output-on-failure` to run only CPU regressions.


## Linear HDR and common display output (M2-A)

Sky, opaque, alpha mask/blend and scene debug render into one native-resolution
`R16G16B16A16_SFLOAT` attachment. The scene shaders emit linear radiance, including
values above 1. Transparent blending completes there before a fullscreen pass
reads it and applies exposure, the retained filmic fit and the SDR sRGB transfer.
The sRGB attachment uses hardware encoding; supported UNORM output uses the
shader transfer instead. ImGui follows the output pass. This filmic fit is an
approximation, not a complete ACES color-management pipeline.

RGBA16F adds 15.82 MiB of payload at 1920×1080. Format attachment/blend/sample/
readback capabilities are checked. HDR resources and output pipeline are replaced
as one fully constructed candidate after old frame users finish.

The GPU regression reads actual PBR emissive/blend and sky output, compares four
sRGB/UNORM RGBA/BGRA display formats across exposure/tone settings, and verifies
UI ordering, invalid-setting rejection, failed replacement cleanup and resize.
It retains the asynchronous scene/ImGui tests and final-zero resource checks.
Current llvmpipe results establish numeric correctness, not RTX frame-budget or
motion-quality acceptance. The direct Vulkan 1.3 RenderDoc attempt on this software
configuration failed device selection; a new hardware capture remains pending.
