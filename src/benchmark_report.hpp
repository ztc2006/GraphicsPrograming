#pragma once

#include "measurement.hpp"
#include "resource_ledger.hpp"
#include <array>
#include <filesystem>
#include <span>

struct BenchmarkFrame {
  std::uint64_t id = 0;
  double elapsedSeconds = 0, cpuFrameMs = 0, cpuPrepareMs = 0;
  double fenceMs = 0, acquireMs = 0, submitMs = 0, presentMs = 0;
  std::size_t shadowDraws = 0, mainDraws = 0;
  GpuTimings gpu;
};

struct BenchmarkMetadata {
  std::string projectSourceSha256;
  std::string gpu, deviceType, driver, api, scene, cameraPath, lightCulling, presentMode,
      buildType;
  std::string renderMethod = "raster";
  std::string lightingPreset = "asset";
  unsigned punctualLightCount = 0, spotShadowCount = 0, sunCascadeCount = 0;
  float sunShadowDistance = 0;
  bool localProbeValid = false, detailReflectionProbeValid = false;
  bool cameraCulling=true, shadowCulling=true;
  std::string taaHistoryFilter="catmull-rom";
  bool taaEnabled = false, aoEnabled = false;
  float aoRadius = .5f, aoStrength = 1.f;
  std::string aoDebug = "none";
  bool temporalJitterEnabled = false, temporalHistoryValid = false;
  unsigned width = 0, height = 0;
  unsigned framesInFlight = 1, swapchainImageCount = 0;
  std::string presentSyncBackend = "legacy_wait_idle", presentSyncReason;
  bool presentFencesEnabled = false, presentationReleaseProven = false;
  std::uint64_t presentQueued = 0, presentCompleted = 0, pendingPresentFences = 0,
                presentFenceWaits = 0, legacyPresentDrains = 0;
  std::array<float, 3> cameraPosition{}, cameraTarget{};
  float fovRadians = 0, nearPlane = 0, farPlane = 0, environmentIntensity = 1, exposureEv = 0;
  bool toneMappingEnabled = true;
  double warmupSeconds = 0, requestedSeconds = 0, measuredSeconds = 0,
         loadMs = 0;
  std::uint64_t deviceLocalBytes = 0, sampledPeakHeapUsage = 0,
                lastHeapBudget = 0;
  bool software = false, validation = false, memoryBudget = false, ui = true,
       completed = false;
  unsigned validationErrors = 0, validationWarnings = 0;
  ResourceLedger::Snapshot engineResources;
};

void writeBenchmarkReport(std::filesystem::path const &directory,
                          BenchmarkMetadata const &metadata,
                          std::span<BenchmarkFrame const> frames);
