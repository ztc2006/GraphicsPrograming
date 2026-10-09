#include "benchmark_report.hpp"

#include <fstream>
#include <iomanip>
#include <locale>
#include <numeric>

void writeBenchmarkReport(std::filesystem::path const &directory,
                          BenchmarkMetadata const &m,
                          std::span<BenchmarkFrame const> frames) {
  if (std::filesystem::exists(directory / "frames.csv") ||
      std::filesystem::exists(directory / "summary.json"))
    throw std::runtime_error(
        "Benchmark output already exists; choose a new directory");
  std::filesystem::create_directories(directory);
  std::ofstream csv(directory / "frames.csv"), json(directory / "summary.json");
  csv.exceptions(std::ios::failbit | std::ios::badbit);
  json.exceptions(std::ios::failbit | std::ios::badbit);
  csv.imbue(std::locale::classic());
  json.imbue(std::locale::classic());
  csv << std::setprecision(9)
      << "frame_id,elapsed_s,cpu_frame_ms,cpu_prepare_ms,fence_wait_ms,acquire_"
         "ms,submit_ms,present_call_ms,gpu_valid,gpu_total_ms,gpu_shadow_ms,"
         "gpu_main_ms,gpu_output_ms,gpu_ui_ms,gpu_culling_ms,clustered_active,"
         "shadow_draws,main_draws,gpu_taa_ms,gpu_ao_ms,gpu_ao_horizon_ms,gpu_"
         "ao_filter_ms,gpu_ao_composite_ms,gpu_ray_tracing_active\n";
  std::vector<double> cpu, gpu, prepare, shadow, main, output, ui, culling, taa,
      ao, aoHorizon, aoFilter, aoComposite, acquire;
  std::size_t slowAcquireFrames=0;
  for (auto const &f : frames) {
    cpu.push_back(f.cpuFrameMs);
    prepare.push_back(f.cpuPrepareMs);
    acquire.push_back(f.acquireMs);
    slowAcquireFrames+=f.acquireMs>100;
    csv << f.id << ',' << f.elapsedSeconds << ',' << f.cpuFrameMs << ','
        << f.cpuPrepareMs << ',' << f.fenceMs << ',' << f.acquireMs << ','
        << f.submitMs << ',' << f.presentMs << ',' << f.gpu.valid << ',';
    if (f.gpu.valid) {
      gpu.push_back(f.gpu.totalMs);
      shadow.push_back(f.gpu.shadowMs);
      main.push_back(f.gpu.mainMs);
      output.push_back(f.gpu.outputMs);
      ui.push_back(f.gpu.uiMs);
      culling.push_back(f.gpu.cullingMs);
      taa.push_back(f.gpu.taaMs);
      ao.push_back(f.gpu.aoMs);
      aoHorizon.push_back(f.gpu.aoHorizonMs);
      aoFilter.push_back(f.gpu.aoFilterMs);
      aoComposite.push_back(f.gpu.aoCompositeMs);
      csv << f.gpu.totalMs << ',' << f.gpu.shadowMs << ',' << f.gpu.mainMs
          << ',' << f.gpu.outputMs << ',' << f.gpu.uiMs << ',' << f.gpu.cullingMs;
    } else
      csv << ",,,,,";
    csv << ',' << f.gpu.clustered << ',' << f.shadowDraws << ',' << f.mainDraws << ',';
    if(f.gpu.valid)csv<<f.gpu.taaMs;
    csv << ',';
    if (f.gpu.valid)
      csv << f.gpu.aoMs;
    csv << ',';
    if (f.gpu.valid)
      csv << f.gpu.aoHorizonMs;
    csv << ',';
    if (f.gpu.valid)
      csv << f.gpu.aoFilterMs;
    csv << ',';
    if (f.gpu.valid)
      csv << f.gpu.aoCompositeMs;
    csv << ',' << f.gpu.rayTracing << '\n';
  }
  json << std::boolalpha << std::setprecision(9) << "{\n  \"schema\": 4,\n";
  auto text = [&](char const *key, std::string const &v) {
    json << "  \"" << key << "\": " << jsonString(v) << ",\n";
  };
  text("gpu", m.gpu);
  text("project_source_sha256", m.projectSourceSha256);
  text("device_type", m.deviceType);
  text("driver_version", m.driver);
  text("vulkan_api", m.api);
  text("scene", m.scene);
  text("camera_path", m.cameraPath);
  text("render_method", m.renderMethod);
  text("selected_hdr_format", m.renderMethod=="ray-tracing" ? "R32G32B32A32_SFLOAT" : "R16G16B16A16_SFLOAT");
  text("gpu_main_semantics", m.renderMethod=="ray-tracing" ? "TLAS build + path tracing + accumulation" : "raster main scene");
  text("light_culling_requested", m.lightCulling);
  text("lighting_preset", m.lightingPreset);
  json << "  \"camera_culling_enabled\": " << m.cameraCulling
       << ",\n  \"shadow_caster_culling_enabled\": " << m.shadowCulling
       << ",\n  \"slow_acquire_frame_count\": " << slowAcquireFrames << ",\n";
  json << "  \"punctual_light_count\": " << m.punctualLightCount
       << ",\n  \"spot_shadow_count\": " << m.spotShadowCount
       << ",\n  \"sun_cascade_count\": " << m.sunCascadeCount
       << ",\n  \"sun_shadow_distance\": " << m.sunShadowDistance
       << ",\n  \"local_probe_valid\": " << m.localProbeValid
       << ",\n  \"detail_reflection_probe_valid\": "
       << m.detailReflectionProbeValid << ",\n";
  text("present_mode", m.presentMode);
  text("build_type", m.buildType);
  text("ao_algorithm", "gtao_3x6_spatial");
  text("ao_debug", m.aoDebug);
  json << "  \"ao_enabled\": " << m.aoEnabled
       << ",\n  \"ao_radius\": " << m.aoRadius
       << ",\n  \"ao_strength\": " << m.aoStrength << ",\n";
  text("taa_history_filter",m.taaHistoryFilter);
  text("taa_history_lattice","unjittered_output");
  text("frame_target_policy",
       "shared_hdr_depth_motion_shadow_ao_diffuse_composite");
  json << "  \"motion_vectors_enabled\": true,\n"
       << "  \"taa_enabled\": " << m.taaEnabled << ",\n"
       << "  \"temporal_jitter_enabled\": " << m.temporalJitterEnabled << ",\n"
       << "  \"temporal_camera_history_valid\": " << m.temporalHistoryValid << ",\n";
  text("present_sync_backend", m.presentSyncBackend);
  text("present_sync_reason", m.presentSyncReason);
  json << "  \"present_fences_enabled\": " << m.presentFencesEnabled
       << ",\n  \"presentation_release_proven\": " << m.presentationReleaseProven
       << ",\n  \"present_queued_count\": " << m.presentQueued
       << ",\n  \"present_fence_completed_count\": " << m.presentCompleted
       << ",\n  \"pending_present_fences\": " << m.pendingPresentFences
       << ",\n  \"present_fence_wait_count\": " << m.presentFenceWaits
       << ",\n  \"legacy_present_drain_count\": " << m.legacyPresentDrains << ",\n";
  json << "  \"frames_in_flight\": " << m.framesInFlight
       << ",\n  \"swapchain_image_count\": " << m.swapchainImageCount << ",\n";
  json << "  \"width\": " << m.width << ",\n  \"height\": " << m.height
       << ",\n  \"software_device\": " << m.software
       << ",\n  \"validation_enabled\": " << m.validation
       << ",\n  \"validation_errors\": " << m.validationErrors
       << ",\n  \"validation_warnings\": " << m.validationWarnings
       << ",\n  \"ui_enabled\": " << m.ui
       << ",\n  \"completed\": " << m.completed
       << ",\n  \"warmup_seconds\": " << m.warmupSeconds
       << ",\n  \"requested_seconds\": " << m.requestedSeconds
       << ",\n  \"measured_seconds\": " << m.measuredSeconds
       << ",\n  \"scene_load_ms\": " << m.loadMs
       << ",\n  \"device_local_heap_bytes\": " << m.deviceLocalBytes
       << ",\n  \"memory_budget_supported\": " << m.memoryBudget
       << ",\n  \"sampled_peak_device_local_heap_usage_bytes\": "
       << (m.memoryBudget ? std::to_string(m.sampledPeakHeapUsage) : "null")
       << ",\n  \"last_device_local_heap_budget_bytes\": "
       << (m.memoryBudget ? std::to_string(m.lastHeapBudget) : "null")
       << ",\n  \"frame_count\": " << frames.size()
       << ",\n  \"gpu_sample_count\": " << gpu.size()
       << ",\n  \"average_loop_fps\": "
       << (m.measuredSeconds > 0 ? frames.size() / m.measuredSeconds : 0)
       << ",\n";
  auto footprint = [&](ResourceLedger::Footprint const &f) {
    json << "{\"allocations\":" << f.allocations << ",\"buffers\":" << f.buffers
         << ",\"images\":" << f.images << ",\"image_views\":" << f.imageViews
         << ",\"samplers\":" << f.samplers << ",\"descriptor_pools\":" << f.descriptorPools
         << ",\"descriptor_sets\":" << f.descriptorSets
         << ",\"payload_bytes\":" << f.payloadBytes
         << ",\"allocated_bytes\":" << f.allocatedBytes
         << ",\"device_local_bytes\":" << f.deviceLocalBytes
         << ",\"host_visible_bytes\":" << f.hostVisibleBytes
         << ",\"suballocations\":" << f.suballocations
         << ",\"suballocated_bytes\":" << f.suballocatedBytes
         << ",\"suballocation_device_local_bytes\":" << f.suballocationDeviceLocalBytes
         << ",\"suballocation_host_visible_bytes\":" << f.suballocationHostVisibleBytes << '}';
  };
  json << "  \"engine_owned_resources\": {\"current\":";
  footprint(m.engineResources.current);
  json << ",\"peak\":";
  footprint(m.engineResources.peak);
  json << ",\"domains\":{";
  for (std::size_t i = 0; i < ResourceLedger::domainCount; ++i) {
    if (i) json << ',';
    json << jsonString(std::string(ResourceLedger::name(static_cast<ResourceLedger::Domain>(i)))) << ":";
    footprint(m.engineResources.domains[i]);
  }
  json << "},\"domain_peaks\":{";
  for (std::size_t i = 0; i < ResourceLedger::domainCount; ++i) {
    if (i) json << ',';
    json << jsonString(std::string(ResourceLedger::name(static_cast<ResourceLedger::Domain>(i)))) << ":";
    footprint(m.engineResources.domainPeaks[i]);
  }
  json << "},\"scope\":\"Engine buffers, images/views, samplers and frame/material descriptor pools/sets. "
          "Excludes swapchain storage, ImGui backend allocations, driver object overhead and CPU assets. "
          "Allocation bytes count unique VMA backing blocks for buffers and images, not residency. Suballocation bytes belong to lifecycle domains and are not added to backing bytes. "
          "Memory property byte counts may overlap; peak fields are independent high-water marks.\"},\n";
  json << "  \"camera_position\": [" << m.cameraPosition[0] << ','
       << m.cameraPosition[1] << ',' << m.cameraPosition[2] << "],\n"
       << "  \"camera_target\": [" << m.cameraTarget[0] << ','
       << m.cameraTarget[1] << ',' << m.cameraTarget[2] << "],\n"
       << "  \"fov_radians\": " << m.fovRadians
       << ",\n  \"near_plane\": " << m.nearPlane
       << ",\n  \"far_plane\": " << m.farPlane
       << ",\n  \"environment_intensity\": " << m.environmentIntensity << ",\n"
       << "  \"exposure_ev\": " << m.exposureEv << ",\n"
       << "  \"tone_mapping_enabled\": " << m.toneMappingEnabled << ",\n"
       << "  \"scene_color_format\": \"R16G16B16A16_SFLOAT\",\n";
  auto stats = [&](char const *key, std::vector<double> const &v) {
    json << "  \"" << key << "\": ";
    if (v.empty())
      json << "null,\n";
    else
      json << "{\"p50\":" << percentile(v, .5)
           << ",\"p95\":" << percentile(v, .95)
           << ",\"p99\":" << percentile(v, .99) << "},\n";
  };
  stats("cpu_frame_ms", cpu);
  stats("cpu_acquire_ms", acquire);
  stats("cpu_prepare_ms", prepare);
  stats("gpu_total_ms", gpu);
  stats("gpu_shadow_ms", shadow);
  stats("gpu_main_ms", main);
  stats("gpu_culling_ms", culling);
  stats("gpu_taa_ms", taa);
  stats("gpu_ao_ms", ao);
  stats("gpu_ao_horizon_ms", aoHorizon);
  stats("gpu_ao_filter_ms", aoFilter);
  stats("gpu_ao_composite_ms", aoComposite);
  stats("gpu_output_ms", output);
  stats("gpu_ui_ms", ui);
  json
      << "  \"hardware_target_accepted\": false,\n"
      << "  \"notes\": \"Pass timestamps are pipeline boundaries, not isolated "
         "shader costs. CPU frame includes waits. Heap usage is "
         "driver-reported, sampled, and may include other processes. Hardware "
         "acceptance requires separate quality and repeated-run review.\"\n}\n";
}
