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
  std::string gpu, deviceType, driver, api, scene, cameraPath, presentMode,
      buildType;
  unsigned width = 0, height = 0;
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
