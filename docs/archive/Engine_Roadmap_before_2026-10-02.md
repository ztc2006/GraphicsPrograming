# Vulkan Engine Roadmap

Last Updated: 2026-05-25
Primary Branch: `archlinux`

## 1. Current Project State

The project is now past the earliest triangle-only stage. The renderer has a
transactional swapchain recreation path, frame-scoped rendering entry points,
scene/camera data, mesh GPU upload, uniform descriptors, and multi-object draw
submission.

Current verified-from-code state:
- `Application` owns window lifecycle, scene setup, main loop, and swapchain
  recreation.
- `Renderer` owns GPU-side mesh resources, frame resources, descriptors,
  pipeline creation, command recording, and depth resources.
- `Scene` owns meshes, objects, and cameras.
- `SceneObject` is intentionally minimal: `Transform` plus `MeshId`.
- Dynamic Rendering is the active render path.
- Depth image, memory, image view, rendering attachment, and pipeline depth
  state are implemented in code.

Current caveat:
- Depth is code-complete but not accepted until runtime occlusion, validation
  output, resize, and minimize/restore behavior are checked.

## 2. Guiding Strategy

The project should move in two tracks:

1. Ship visible rendering effects first.
2. Only extract larger engine systems after the effect path proves the data,
   pass, UI, and resource boundaries.

This avoids building ECS, material systems, render graphs, or asset pipelines
before the renderer has enough real pressure to justify their shape.

Confirmed decisions:
- Keep Dynamic Rendering for the current stage.
- Do not introduce FrameGraph/RenderGraph yet.
- Do not introduce ECS during the current effect milestone.
- Build a minimal material path before a formal `MaterialSystem`.
- Keep temporary technical debt local to scene/material binding code.
- Prefer small verifiable milestones over broad architecture rewrites.
- After the current material GPU resource split, make rendering quality the main
  direction again: directional shadow mapping first, then PCF and bias tuning.
- Treat logging and ImGui cleanup as support work for shadow debugging, not as
  separate large milestones.

## 3. Milestone M1: Effect-First Rendering

Goal: reach a small but real 3D rendering loop with correct depth, textures,
multiple objects/materials, basic lighting, and stable resize behavior.

Status:
- S1 depth path: implemented and accepted.
- S2 texture sampling: external file-backed texture loading is active.
- S3 multi-object/multi-material: implemented through `Scene::materials` and
  `SceneObject::materialId`.
- S4 Blinn-Phong lighting: implemented with editable ImGui light controls.
- S5 camera/debug UI: free-fly camera and quick ImGui docking shell are active.
- S6 resource cleanup: texture extraction is done; material GPU resource
  extraction is the next small cleanup.

### M1-S1 Depth Path

Implemented:
- Depth image, memory, image view, and layout state are owned by `Renderer`.
- Depth resources are recreated with the swapchain.
- Dynamic Rendering uses `pDepthAttachment`.
- Pipeline depth test and depth write are enabled.
- Current scene has 3 objects with different z offsets for occlusion checks.

Acceptance tasks:
- Build and run on Arch.
- Confirm the 3-object scene occludes correctly.
- Check validation-layer output during normal rendering.
- Resize repeatedly and verify depth resources are recreated correctly.
- Minimize/restore and confirm rendering recovers.

### M1-S2 Texture Sampling

Target:
- Load albedo textures.
- Upload texture image data through staging buffers.
- Create image view and sampler resources.
- Add combined image sampler descriptor binding.
- Sample albedo in the fragment shader.

Implementation notes:
- Use a minimal dependency such as `stb_image` unless project constraints change.
- Add texture upload helpers before extracting a full resource manager.
- Keep descriptor expansion focused on the current single-texture albedo case.

### M1-S3 Minimal Material Binding

Target:
- Add minimal material data: `baseColor` plus optional `albedoTexture`.
- Allow each `SceneObject` to reference material data.
- Draw multiple objects with distinct visible material output.

Recommended temporary shape:
- Add `MaterialId`.
- Store materials in `Scene`.
- Add `materialId` to `SceneObject`.
- Keep formal material system extraction for M2.

### M1-S4 Basic Lighting

Target:
- Add directional light direction, color, and intensity.
- Add normals to vertex/mesh data.
- Implement Blinn-Phong diffuse and specular terms.
- Verify specular response changes with camera/object motion.

Out of scope for this slice:
- PBR.
- IBL.
- Shadows.
- Multiple dynamic lights.

### M1-S5 Camera, Debug UI, And Runtime Inspection

Implemented:
- Free-fly camera for inspecting lighting and material response.
- ImGui docking shell.
- Camera, light, material, and render debug panels.
- Rasterizer debug controls for cull mode and front face.

Purpose:
- Give the renderer enough runtime inspection before starting multi-pass
  rendering features.
- Keep debug UI practical without turning it into an editor framework.

### M1-S6 Stability And Cleanup

Target:
- Stress resize and minimize/restore.
- Confirm no validation errors in the M1 path.
- Finish small resource extractions that are already justified by the code:
  `TextureLoader` / `TextureResources` first, then `MaterialGpuStore`.
- Keep `cmake --build` and clangd non-blocking on Arch.

M1 done means:
- At least 3 objects render at once.
- Objects have visibly distinct material output.
- Depth occlusion is correct.
- Basic lighting is visible.
- Resize/minimize restore remains stable.

## 4. Milestone M2: Material And Resource Structure

Goal: turn only the proven M1 resource flow into small engine-facing structure.

Scope:
- Extract `MaterialGpuStore` from `Renderer` before the shadow milestone.
- Introduce stable handles for textures/materials.
- Separate CPU-side material descriptions from GPU-side bound resources.
- Centralize texture destruction and recreation rules.
- Keep renderer submission simple and explicit.

Recommended order:
1. Keep `Scene::materials` as the CPU-side material source for now.
2. Move GPU-side material resources, descriptor pool allocation, descriptor set
   writes, texture loading, and tint sync into `MaterialGpuStore`.
3. Keep `Renderer` responsible for frame lifecycle, pipeline, draw submission,
   descriptor binding, and push constants.
4. Add invalid material/resource checks where they help catch real mistakes.
5. Reconsider a formal `MaterialSystem` after shadow mapping creates more
   concrete pressure.

Non-goals:
- Full asset database.
- Hot reload.
- Editor-facing material graph.
- Full material graph or PBR material model.

## 5. Milestone M3: Scene And ECS Foundation

Goal: move from minimal `SceneObject` storage toward an engine-style entity
model without disrupting the renderer.

Recommended ECS shape:
- Sparse set storage.
- Entity ID plus generation.
- Components for transform, mesh renderer, and camera.
- Keep rendering extraction explicit: scene data is gathered into renderable
  submissions each frame.

Recommended order:
1. Introduce entity ID/generation.
2. Add transform component storage.
3. Add mesh-renderer component storage.
4. Migrate current `SceneObject` data.
5. Keep old scene path until parity is proven, then remove it.

M3 done means:
- Existing M1 scene can be represented through entities/components.
- Renderer still draws the same objects with the same visible output.
- Invalid entity/component access is detected instead of silently corrupting
  state.

## 6. Milestone M4: Renderer Architecture Upgrade

Goal: prepare the renderer for larger features without prematurely adding a full
render graph.

Scope:
- Clarify render submission structures.
- Separate frame resources, swapchain resources, and persistent GPU resources.
- Add reusable helpers for buffer/image creation where duplication is now real.
- Review command recording boundaries.
- Keep Dynamic Rendering unless a concrete feature requires otherwise.

Possible later additions:
- Multiple render passes or passes encoded through explicit functions.
- Offscreen render target support.
- MSAA.
- Render-to-texture.

Non-goal:
- A generic FrameGraph before there are multiple real passes to schedule.

## 7. Milestone M5: Shadow Mapping And Rendering Quality

Goal: move from single lit pass rendering into a small multi-pass renderer with
visible quality improvements.

Primary target:
- Directional shadow map.
- PCF filtering.
- Depth bias tuning.
- Shadow map visualization in ImGui.

Recommended priority:
1. Add a light-space camera/matrix for the directional light.
2. Create a depth-only shadow image, view, sampler, and framebuffer-equivalent
   dynamic rendering setup.
3. Add a shadow depth pass before the main lit pass.
4. Sample the shadow map in the Blinn-Phong fragment shader.
5. Expose shadow enable, bias, filter radius, and debug visualization in ImGui.
6. Add PCF after the hard-shadow path is correct.

Supporting work before or during this milestone:
- Add lightweight logging/error reporting only where it makes Vulkan resource
  creation and pass setup easier to diagnose.
- Extract small ImGui helper structure only if `Application` debug UI becomes
  hard to read.

Deferred candidates:
- Model loading with material preservation.
- Skybox.
- Normal mapping.
- Post-processing.
- GPU timing markers.
- Debug draw utilities.

## 8. Milestone M6: Tooling And Workflow

Goal: make development predictable and easy to resume.

Scope:
- Keep `build-linux` as the active Arch/clangd build directory.
- Keep project documentation current after each milestone.
- Add small sample scenes for regression checks.
- Add scripts for shader compilation and local build/run.
- Keep GitHub branch state clear: commit and push complete roadmap/code slices.

Potential checks:
- Build passes.
- Shader compilation passes.
- Validation output has no errors in known regression scenarios.
- Resize/minimize smoke test is performed after swapchain-related work.

## 9. Near-Term Execution Plan

The next concrete sequence is:

1. Finish the current small material GPU resource extraction:
   `MaterialGpuStore` owns GPU-side material resources and descriptor writes;
   `Renderer` keeps frame/pipeline/draw responsibilities.
2. Add minimal debug support only where needed:
   lightweight logging/error reporting for Vulkan setup, and small ImGui
   organization if the debug UI becomes too noisy.
3. Start directional shadow mapping with a hard-shadow path first.
4. Add ImGui controls for shadow enable, bias, and debug texture visualization.
5. Add PCF after the hard-shadow path is visually correct.
6. Revisit larger material/render architecture only after shadow work exposes
   concrete pressure.

Recommended next commit after this roadmap:
- `refactor`: extract material GPU resources from renderer.
- Then `feat`: add directional shadow map.

## 10. Long-Term Direction

The long-term direction is a small Vulkan engine with:
- Stable rendering foundations.
- Explicit resource ownership.
- Minimal but useful material and texture systems.
- ECS-based scene representation.
- Debug UI that helps inspect real rendering state.
- Rendering quality features that justify each architecture step.
- A renderer architecture that grows from actual feature pressure.

The important constraint is sequencing: visible renderer behavior first,
architecture extraction second.
