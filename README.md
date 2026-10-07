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
[implementation contract](docs/M2_A_HDR_Implementation.md). M2-B now schedules
shadow/main/display/UI through a minimal single-queue Render Graph, with sampled
scene depth, explicit attachment preservation and an inspectable resource/pass
plan. See the [graph contract](docs/M2_B_Graph_Implementation.md). M2-C1 provides
startup-selectable one/two frame contexts with separate mutable resources and
per-submission completion delivery. See the [frame contract](docs/M2_C_Frames_Implementation.md).
M2-C2 adds capability-selected KHR/EXT present fences and swapchain-owned
presentation resources. See the [presentation contract](docs/M2_C2_Presentation_Implementation.md).
M3-A1 adds typed CPU mip chains, complete image uploads/views and working glTF
minification; see the [decision](docs/M3_A1_Texture_Mips_Decision.md).
M3-A2 adds single-source alpha coverage, editable fallback views, vertex alpha,
pinned MikkTSpace and normal/reflected-culling contracts;
[19/19 software regressions pass](docs/M3_A2_Material_Implementation.md).
M3-B adds GGX cubemap prefiltering, a split-sum BRDF LUT, calibrated SH and
versioned environment baking caches; [20/20 software regressions pass](docs/M3_B_IBL_Implementation.md).
M3-C1 adds KHR_materials_specular (factor, linear alpha/sRGB color textures,
independent UV/samplers, direct and IBL response) and a UI-loadable
[reference grid](assets/render_tests/specular_reference.gltf);
[20/20 software regressions pass](docs/M3_C1_Specular_Implementation.md).
M3-C2 adds normal-variance filtering and specular antialiasing; M4-A imports/edits
point, spot and directional lights with independent per-frame SSBOs. M4-B adds
GPU clustered assignment and a full-light comparison switch; see the
[cluster contract](docs/M4_B_Clustered_Implementation.md). A kitchen correctness repair adds four spot-shadow slots and one on-demand room
probe before stable CSM. Point/directional local shadows, CSM and SSR remain pending.
See [indoor contract](docs/Indoor_Lighting_Implementation.md).

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
Specular IBL samples a GGX prefiltered cubemap and split-sum BRDF LUT;
see the [IBL decision](docs/M3_B_IBL_Decision.md). Lighting's PBR Debug selector exposes base
color, metallic, roughness, normals, AO and environment-light components.


## PBR test scenes

Download the pinned assets once (about 109.0 MB total); subsequent runs verify and
reuse them. Sources, licenses, file sizes and SHA-256 are recorded in
`tools/pbr_test_assets.json`. Downloaded payloads stay out of Git history.

```bash
python3 tools/fetch_pbr_test_assets.py
python3 tools/fetch_pbr_test_assets.py --verify-only   # local integrity, no network
./run.sh --no-build assets/models/pbr_kitchen/kitchen_cutaway.gltf
./run.sh --no-build assets/models/pbr_flight_helmet/source/FlightHelmet.gltf
```

[Country Kitchen](assets/models/pbr_kitchen/README.md) supplies a complete static
interior with 90 materials and approximately 1.44 million triangles. Its generated
cutaway opens the walls/ceiling for the current bounds-fit camera; the complete
core scene remains in `source/kitchen_core.gltf`. Use the complete scene for enclosed
room coverage, and fly inside with right mouse + WASD/Space/Ctrl. The viewer does
not yet use imported glTF cameras. The cutaway changes occlusion and is intended
for material inspection. Glass is an alpha blend approximation; emissive meshes
do not illuminate nearby objects in the current renderer.

[Flight Helmet](assets/models/pbr_flight_helmet/README.md) complements it with
normal and ORM textures, leather/rubber/wood/metal and detailed specular surfaces.
It uses the original upstream core version preceding the transmission extension.
Both load through the existing Scene panel and file-drop path.

Additional extension references are available:
[Anisotropy Barn Lamp](assets/models/pbr_anisotropy_barn_lamp/README.md) and
[Green Glass Dragon](assets/models/pbr_green_glass_dragon/README.md).
The green dragon is a labelled volume-absorption variant of upstream DragonAttenuation;
its original thickness map, geometry and white surface colour are retained.
Current unsupported transmission/volume/anisotropy extensions use reported core
fallbacks: these assets expose missing material features, rather than proving
that glass or anisotropic reflections are already implemented.

```bash
./run.sh --no-build assets/models/pbr_green_glass_dragon/green_glass_dragon.gltf
./run.sh --no-build assets/models/pbr_anisotropy_barn_lamp/source/AnisotropyBarnLamp.gltf
```

 Keep the courtyard
for existing asset compatibility and the analytic grids for numeric correctness;
future coverage should include these assets instead of relying on one courtyard.
Asset selection follows RTR4 Chapters 5/6/9: distinguish texture/color/tangent
semantics from scene lighting and visibility.

Software Vulkan smoke checks cover loading, uploads, submission, reports and clean
shutdown. These checks establish compatibility; RTX 4060 Ti image quality,
temporal behavior, GPU budgets and RenderDoc captures remain deferred.

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
scene color format, load time, draw counts and p50/p95/p99. Current schema 4 adds
`frames_in_flight`, `swapchain_image_count` and `frame_target_policy`. Schema 3 added
`gpu_output_ms`, `exposure_ev`, `tone_mapping_enabled` and `scene_color_format`;
`environment_intensity` replaces the misleading `environment_exposure` key.
CPU frame/preparation, fence/acquire/submit/present call time and GPU total/shadow/
main/output/UI intervals are recorded separately. GPU query results are matched to their
submitted frame through a completion callback after its fence completes. All
completed slots are delivered once, including final drain. Unsupported timing is marked
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
invalid sampler indices/filter/wrap values produce errors. M3-A1 now generates
complete typed mip chains: color RGB is filtered in linear space and re-encoded;
data channels average independently; normal vectors are averaged/renormalized.
NPOT edge texels contribute through area weights. Encoded content, color space,
mip policy and algorithm version identify shared images; samplers remain separate.
Omitted filters default to linear/trilinear; explicit glTF modes remain intact.
Mip sampling uses the image view's complete chain. Non-mip modes use nearest mip
and maxLod=0.25 to preserve different min/mag filters. Eligible linear/trilinear
requests use up to 8x anisotropy when the enabled device feature/limit permits it.
M3-A2 adds cutoff/factor-aware coverage mips for a single opacity source:
baseColor A or separate OBJ opacity R. Material edits, compound sources,
non-unit vertex alpha and active parallax use prebound LOD 0 views sharing the
same storage; restoring parameters restores coverage filtering. COLOR_0 alpha
is preserved. Pinned MikkTSpace splits incompatible mirrored-UV corners;
normalScale, backface TBN, nonuniform/negative transforms and single-sided
reflected culling have production shader pixel regressions. See the
[decision](docs/M3_A2_Material_Decision.md) and
[implementation](docs/M3_A2_Material_Implementation.md): 19/19 CPU/software
Vulkan tests pass. These approximations do not complete motion stability.
M3-B retains the original HDR sky and uploads an immutable GGX cube/LUT in one
cold-start batch. Its CPU cache at `.cache/ibl` validates source/settings/version
and recovers from corrupted or unwritable files. Roughness selects the cube LOD;
exposure remains in the display output. Normal variance/specular AA, necessary
specular extensions and final hardware quality remain open.

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


## Minimal Render Graph (M2-B)

`Application` submits scene draw lists once; `Renderer` organizes passes. The graph
imports shadow/depth/HDR/swapchain images, checks initialized contents and
attachment load/store contracts, infers RAW/WAR/WAW dependencies, and records
synchronization2 barriers and dynamic-rendering boundaries. Main depth is stored
and exported read-only. UI uses a separate LOAD/STORE pass with a same-layout
color dependency. Initially disabled shadows get one far-depth clear; later
frames reuse the defined read-only image. State is published after successful
submission. The Render Debug panel exposes the resource/pass/barrier plan.

Eleven CTests pass (nine CPU and two llvmpipe/X11 GPU), including numeric HDR,
UI pixel preservation, opaque depth beneath alpha blend, shadow toggles and
independent visible/caster lists, failed target replacement, actual resize,
asynchronous ImGui scene loading and final resource zero. The window manager did
not confirm native iconify; explicit restore/recreation and rendering passed,
while native minimize and zero-size waiting still need acceptance. No aliasing,
multiple queues or temporal-history allocation is included in this increment.
See the [decision](docs/M2_B_Graph_Decision.md) and
[implementation contract](docs/M2_B_Graph_Implementation.md).


## Frame contexts (M2-C1)

```bash
./run.sh --no-build --frames-in-flight 2 assets/models/siheyuan/source/siheyuan.glb
```

Default is one frame; startup accepts only one or two. Uniforms, descriptor sets,
commands, query pools, acquire semaphores and submit fences are independent per
slot. HDR/depth/shadow images stay shared with graph barriers across submissions;
render-finished semaphores remain per swapchain image. This is a correctness and
measurement configuration; throughput and latency require RTX 4060 Ti measurements.
Report schema 4 and Render Debug identify the active configuration.

One/two-frame WSI tests cover HDR, real ImGui loading/preview retirement, resize
and shutdown. A timeline-gated offscreen scene verifies distinct pending uniforms,
query delivery without loss/duplicates, slot reuse, reverse completion order and
scene/UI lifetime. See the [implementation contract](docs/M2_C_Frames_Implementation.md).
Presentation objects now belong to SwapChain; see the M2-C2 contract below.


## Presentation resources (M2-C2)

```bash
./run.sh --no-build --frames-in-flight 2 --present-sync fence assets/models/siheyuan/source/siheyuan.glb
```

Default `--present-sync auto` enables KHR/EXT swapchain maintenance only when its
instance dependencies, device extension and feature are available. `fence` fails
startup if unavailable; `legacy` deliberately uses the idle fallback. Present mode
(`--present fifo|mailbox|immediate|auto`) remains a separate choice.
SwapChain owns per-image finished semaphores and present fences. Renderer rebuilds
preserve those handles; resize/exit drain known present requests before destroying
the old generation. Enqueued WSI errors retain fence ownership; rejected OOM never
waits an unsubmitted fence. Render Debug and schema 4 report backend/reason,
pending/completed counts and resource-release proof. Legacy reports proof false;
a present fence does not measure screen display or latency.

The software validation command on this host is:

```bash
env -u WAYLAND_DISPLAY \
  VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  MESA_VK_WSI_DEBUG=sw,noshm \
  ctest --test-dir build-linux --output-on-failure
```

Seventeen tests cover CPU contracts and real one/two-frame KHR/EXT/legacy WSI,
HDR/graph, ImGui loading, queries, resize and shutdown. The software settings avoid
the current isolated session's MIT-SHM/DRI3 `FenceFromFD` failure, reproduced during
the full EXT test sequence. They are only a validation environment; normal startup
keeps its native driver path. That native shared-image failure, actual OUT_OF_DATE,
device-lost, native minimize, validation and RTX 4060 Ti acceptance remain open.
See the [decision](docs/M2_C2_Presentation_Decision.md) and
[implementation/evidence](docs/M2_C2_Presentation_Implementation.md).


## Clustered lighting (M4-B)

```bash
./run.sh --no-build --light-culling clustered assets/render_tests/punctual_reference.gltf
./run.sh --no-build --light-culling full assets/render_tests/punctual_reference.gltf
```

The default requests clustered assignment. Lighting's **Clustered Lights** switch
selects it at runtime; **Cluster Light Count** shows count/64 in gray and full-light
fallback in magenta. The 64×64 / 24-slice grid covers transparent surfaces too.
Overflow above 64 lights per cell falls back to the complete light table. Zero
punctual lights, unsupported projections or resource limits use the full path.
Render Debug shows the actual grid/pass/barriers. Reports add completed-frame
`gpu_culling_ms` and `clustered_active`; requested mode alone does not prove activation.
Performance acceptance and the low-light crossover await RTX 4060 Ti measurements.
This stage adds light assignment; local shadows follow in M5.

### Kitchen lighting and room reflection preview

`./run.sh assets/models/pbr_kitchen/source/kitchen_core.gltf` recognizes the pinned
fixture and applies two explicit ceiling spots plus a captured room probe. The
asset itself has no punctual lights. `--lighting-preset asset` preserves asset
lighting; `auto` is the default, `kitchen` requests this known fixture preset.
In Lighting, **Kitchen lighting preview** and **Restore asset lighting** switch
the setup. **Cast shadow** allocates up to four eligible spots; unassigned lights
remain lit. **Room reflection probe → Capture / refresh probe** updates the
room's static SH/GGX environment. Changes to lights, geometry or materials show
a refresh warning; this is an approximation with one box and no dynamic GI.

### Stable cascaded sun shadows (M5-A)

The main view defaults to four 1024² sun cascades in the left half of the existing
4096×2048 atlas. The four spot tiles retain the right half. In **Lighting → Sun
Shadow**, change **Stable CSM**, **Cascades** (1/2/4), **Shadow distance**, **Split
lambda**, **Cascade blend**, **Caster extension**, world/texel bias, and PCF radius.
**Inspect cascade coverage** shows the selected levels and blend bands; **Depth
debug cascade** selects the tile used by depth debug. Disabling CSM restores the
previous scene-fitted single map for comparison.

Sphere fitting and world texel snapping stabilize sampling; overlap and the last
cascade fade soften transitions. Receiver-plane depth correction handles tilted
surfaces under PCF without a large global bias. Beyond the configured shadow
range, direct sunlight stays enabled. Room captures retain a separate fixed scene
map. Benchmark schema 4 records actual `sun_cascade_count`/`sun_shadow_distance`.

28 CPU/software GPU CTests pass; camera motion quality and cost on the 4060 Ti
remain pending. TAA and GTAO are subsequent stages. See the
[CSM implementation](docs/M5_A_CSM_Implementation.md) and
[decision](docs/M5_A_CSM_Decision.md).


### Motion vectors and jitter (M6-A)

PBR Debug now includes **Motion UV** and **Motion History Validity**. The main
scene writes a second RGBA16F target with unjittered current-minus-previous UV,
validity and diagnostic previous depth. Camera/object state follows the previous
successful submission, including with two frame slots; anonymous compatibility
draws have no object history. Sky motion ignores translation. Transparent coverage
reduces validity for the upcoming temporal resolve.

**Jitter preview (TAA pending)** in Render Debug is off by default. The preview
changes sampling only: TAA accumulation/reprojection is the next increment, and
GTAO comes later. CSM uses the unjittered camera. Camera/scene/resize/jitter resets
invalidate previous state; offline probe capture leaves submission history alone.

Benchmark schema 4 reports `motion_vectors_enabled`, `temporal_jitter_enabled`,
`temporal_camera_history_valid`, `taa_enabled=false` and
`frame_target_policy=shared_hdr_depth_motion_shadow`. Motion adds 8 bytes/pixel;
UBO is now 608 bytes per slot, push remains 128 bytes. The frame/material layout
requires five total storage descriptors, four per shader stage, 16 sampled
images/samplers and 22 per-stage resources; unsupported limits are rejected early. FP16 projected previous depth
is diagnostic; M6-B will use suitable linear depth for disocclusion rejection.
See the [implementation](docs/M6_A_Motion_Implementation.md) and
[decision](docs/M6_A_Motion_Decision.md).


### Native-resolution TAA (M6-B)

The viewer now enables TAA by default. Use **TAA** in Render Debug or `--no-taa`
for comparison; disabling it also disables jitter. Linear HDR resolve precedes
exposure/filmic/sRGB and UI. History uses RGBA16F color and R32F linear depth,
with submitted-order ping-pong, invalid/depth/transparent rejection and YCoCg
neighborhood limits. Motion's fourth component is now previous linear clip.w;
zero represents infinite sky. Unsupported projections and data debug bypass TAA.

Scene/camera/resize/AA resets invalidate history, and offline probes leave it alone.
`gpu_taa_ms` is available in UI, CSV and summary. Current bilinear reconstruction
softens fine detail; M6-C will tune reconstruction/reactivity and motion quality.
32 RTX 4060 Ti/X11 regressions pass, including a two-pending-submission resolve test.
RenderDoc confirms the HDR/resolve/display order. Native Wayland startup tests,
validation-layer acceptance, compositor-dependent acquisition stalls and final
quality/performance acceptance remain open. See [implementation](docs/M6_B_TAA_Implementation.md).


### TAA reconstruction and validity (M6-C)

History now stays on an unjittered output lattice; sampling jitter does not move
old color each frame. **History reconstruction** in Render Debug and
`--taa-history bilinear|catmull-rom` compare the two modes (default Catmull-Rom).
Cubic taps validate depth and history eligibility, with a trusted bilinear
fallback. Historical RGB is bounded to valid evidence to limit negative-lobe halos.

Motion z is now signed: +1 has a valid previous correspondence, -1 is an opaque
current sample without correspondence, and mixed transparency/debug has magnitude
below one. History color alpha records eligibility; display alpha stays opaque.
This fixes reusing transparent composites after they move away. Changed material
or reconstruction settings invalidate color history; exposure remains display-only.

Static Halton fine-signal RMS versus a 64-sample reference improves from .159873
to .00148403. RTX quality fixtures and software resolve pass. Full native X11
regression has 30/32 passes: legacy/EXT generic presentation tests timed out twice;
they remain open, along with final broad motion-quality/performance acceptance.
See [implementation](docs/M6_C_TAA_Implementation.md).
