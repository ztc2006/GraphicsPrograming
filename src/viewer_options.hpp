#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

struct ViewerOptions {
  std::optional<std::filesystem::path> scene;
  std::optional<std::filesystem::path> benchmarkDirectory;
  unsigned width = 800, height = 600;
  unsigned framesInFlight = 1;
  double warmupSeconds = 30, durationSeconds = 120;
  std::string cameraPath = "static", gpu;
  std::string present = "auto", presentSync = "auto";
  bool ui = true, validation = false, help = false;
};

ViewerOptions parseViewerOptions(std::span<std::string_view const> args);
std::string_view viewerUsage();
