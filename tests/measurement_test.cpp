#include "benchmark_report.hpp"
#include "measurement.hpp"
#include "viewer_options.hpp"
#include <chrono>
#include <fstream>
#include <iostream>

void require(bool condition, char const *message) {
  if (!condition)
    throw std::runtime_error(message);
}
int main() {
  try {
    require(timestampMilliseconds(250, 5, 8, 1000000) == 11,
            "Timestamp wrap must use valid bits");
    require(timestampMilliseconds(UINT64_MAX - 2, 2, 64, 1000000) == 5,
            "64-bit wrap");
    require(timestampMilliseconds(100, 110, 32, 2) == .00002,
            "Nanoseconds to milliseconds");
    require(percentile({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, .95) == 10,
            "p95 nearest rank");
    require(percentile({1, 2, 3, 4}, .5) == 2, "p50 nearest rank");
    require(jsonString("a\n\"\\") == "\"a\\u000a\\\"\\\\\"",
            "JSON control escaping");
    std::vector<std::string_view> args{"--benchmark",   "results with spaces",
                                       "--gpu",         "RTX 2060",
                                       "--warmup",      "0",
                                       "--duration",    "2",
                                       "--camera-path", "orbit",
                                       "--present",     "fifo",
                                       "scene.glb"};
    auto o = parseViewerOptions(args);
    require(o.width == 1920 && o.height == 1080 && o.gpu == "RTX 2060" &&
                o.scene->is_absolute() && o.benchmarkDirectory->is_absolute(),
            "Benchmark defaults and caller paths");
    for (auto bad : {std::vector<std::string_view>{"--size", "0x1080"},
                     {"--size", "1.5x10"},
                     {"--warmup", "nan"},
                     {"--duration", "-1"},
                     {"--gpu"},
                     {"--frames-in-flight"},
                     {"--frames-in-flight", "0"},
                     {"--frames-in-flight", "3"},
                     {"--frames-in-flight", "1.5"},
                     {"--frames-in-flight", "nan"},
                     {"--camera-path", "bad"},
                     {"--present", "bad"},
                     {"--present-sync"},
                     {"--present-sync", "bad"},
                     {"--benchmark", "out"},
                     {"a.glb", "b.glb"}}) {
      bool rejected = false;
      try {
        (void)parseViewerOptions(bad);
      } catch (std::invalid_argument const &) {
        rejected = true;
      }
      require(rejected, "Invalid CLI input accepted");
    }
    require(parseViewerOptions(std::vector<std::string_view>{"--frames-in-flight", "2"}).framesInFlight == 2, "Two frame CLI rejected");
    require(parseViewerOptions(std::vector<std::string_view>{"--present-sync", "fence"}).presentSync == "fence", "Fence policy rejected");
    require(parseViewerOptions({}).framesInFlight == 1, "Default frame count changed");
    auto dir =
        std::filesystem::temp_directory_path() /
        ("vulkan-measurement-test-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    BenchmarkMetadata metadata{.gpu = "software\nGPU", .software = true};
    metadata.engineResources.current.allocatedBytes = 8192;
    metadata.engineResources.current.suballocatedBytes = 1024;
    std::vector<BenchmarkFrame> frames{
        {.id = 4,
         .cpuFrameMs = 10,
         .gpu = {.frameId = 4, .valid = true, .totalMs = 5}},
        {.id = 5, .cpuFrameMs = 20}};
    writeBenchmarkReport(dir, metadata, frames);
    std::ifstream input(dir / "summary.json");
    std::string report((std::istreambuf_iterator<char>(input)), {});
    require(report.find("\"gpu_sample_count\": 1") != std::string::npos &&
                report.find("\"hardware_target_accepted\": false") !=
                    std::string::npos,
            "Unknown GPU data or software target acceptance");
    require(report.find("\"schema\": 4") != std::string::npos &&
                report.find("\"allocated_bytes\":8192") != std::string::npos &&
                report.find("\"suballocated_bytes\":1024") != std::string::npos &&
                report.find("\"allocator_blocks\":") != std::string::npos,
            "Report lost the backing/suballocation distinction");
    require(report.find("\"frames_in_flight\": 1") != std::string::npos &&
                report.find("shared_hdr_depth_shadow") != std::string::npos,
            "Report lost frame configuration");
    require(report.find("\"presentation_release_proven\": false") != std::string::npos &&
                report.find("legacy_wait_idle") != std::string::npos &&
                report.find("\"pending_present_fences\": 0") != std::string::npos,
            "Report lost presentation fallback diagnostics");
    require(report.find("\"gpu_output_ms\":") != std::string::npos &&
                report.find("\"exposure_ev\":") != std::string::npos &&
                report.find("R16G16B16A16_SFLOAT") != std::string::npos,
            "Report lost HDR output configuration/timing");
    bool rejected = false;
    try {
      writeBenchmarkReport(dir, metadata, frames);
    } catch (std::runtime_error const &) {
      rejected = true;
    }
    require(rejected, "Existing capture silently overwritten");
    std::filesystem::remove_all(dir);
    std::cout << "Measurement regressions passed\n";
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
