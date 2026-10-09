#include "viewer_options.hpp"

#include <charconv>
#include <cmath>
#include <stdexcept>

namespace {
double number(std::string_view text) {
  double value{};
  auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() ||
      !std::isfinite(value))
    throw std::invalid_argument("Invalid numeric argument: " +
                                std::string(text));
  return value;
}
unsigned dimension(std::string_view text) {
  double n = number(text);
  if (n < 1 || n > 16384 || std::floor(n) != n)
    throw std::invalid_argument(
        "Window dimensions must be integers in 1..16384");
  return static_cast<unsigned>(n);
}
} // namespace

ViewerOptions parseViewerOptions(std::span<std::string_view const> args) {
  ViewerOptions o;
  bool sizeSet = false, positionalOnly = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto arg = args[i];
    auto value = [&]() {
      if (++i == args.size())
        throw std::invalid_argument("Missing value for " + std::string(arg));
      return args[i];
    };
    if (!positionalOnly && arg == "--") {
      positionalOnly = true;
      continue;
    }
    if (!positionalOnly && arg == "--help")
      o.help = true;
    else if (!positionalOnly && arg == "--benchmark")
      o.benchmarkDirectory = std::filesystem::absolute(value());
    else if (!positionalOnly && arg == "--warmup")
      o.warmupSeconds = number(value());
    else if (!positionalOnly && arg == "--duration")
      o.durationSeconds = number(value());
    else if (!positionalOnly && arg == "--camera-path")
      o.cameraPath = value();
    else if (!positionalOnly && arg == "--light-culling")
      o.lightCulling = value();
    else if (!positionalOnly && arg == "--lighting-preset")
      o.lightingPreset = value();
    else if (!positionalOnly && (arg == "--camera-culling" || arg == "--shadow-culling")) {
      auto mode=value();
      if(mode!="on" && mode!="off")throw std::invalid_argument("Geometry culling: on or off");
      (arg=="--camera-culling" ? o.cameraCulling : o.shadowCulling)=mode=="on";
    } else if (!positionalOnly && arg == "--render-method")
      o.renderMethod = value();
    else if (!positionalOnly && arg == "--ao") {
      auto mode = value();
      if (mode != "on" && mode != "off")
        throw std::invalid_argument("AO: on or off");
      o.ao = mode == "on";
    } else if (!positionalOnly && arg == "--ao-radius")
      o.aoRadius = float(number(value()));
    else if (!positionalOnly && arg == "--ao-strength")
      o.aoStrength = float(number(value()));
    else if (!positionalOnly && arg == "--ao-debug")
      o.aoDebug = value();
    else if (!positionalOnly && arg == "--taa-history")
      o.taaHistory = value();
    else if (!positionalOnly && arg == "--gpu")
      o.gpu = value();
    else if (!positionalOnly && arg == "--present")
      o.present = value();
    else if (!positionalOnly && arg == "--present-sync")
      o.presentSync = value();
    else if (!positionalOnly && arg == "--frames-in-flight") {
      auto count = number(value());
      if (count != 1 && count != 2)
        throw std::invalid_argument("Frames in flight must be 1 or 2");
      o.framesInFlight = static_cast<unsigned>(count);
    } else if (!positionalOnly && arg == "--no-taa")
      o.taa = false;
    else if (!positionalOnly && arg == "--no-ui")
      o.ui = false;
    else if (!positionalOnly && arg == "--validation")
      o.validation = true;
    else if (!positionalOnly && arg == "--size") {
      auto s = value();
      auto x = s.find('x');
      if (x == std::string_view::npos)
        throw std::invalid_argument("Use --size WIDTHxHEIGHT");
      o.width = dimension(s.substr(0, x));
      o.height = dimension(s.substr(x + 1));
      sizeSet = true;
    } else {
      if (!positionalOnly && arg.starts_with('-'))
        throw std::invalid_argument("Unknown option: " + std::string(arg));
      if (o.scene)
        throw std::invalid_argument("Only one scene may be loaded");
      o.scene = std::filesystem::absolute(arg);
    }
  }
  if (!std::isfinite(o.aoRadius) || o.aoRadius < .001f || o.aoRadius > 100.f ||
      !std::isfinite(o.aoStrength) || o.aoStrength < 0 || o.aoStrength > 2)
    throw std::invalid_argument("AO radius: .001..100; strength: 0..2");
  if (o.aoDebug != "none" && o.aoDebug != "raw" && o.aoDebug != "filtered")
    throw std::invalid_argument("AO debug: none, raw or filtered");
  if (o.renderMethod != "raster" && o.renderMethod != "ray-tracing")
    throw std::invalid_argument("Render method must be raster or ray-tracing");
  if (o.taaHistory != "bilinear" && o.taaHistory != "catmull-rom")
    throw std::invalid_argument("TAA history filter: bilinear or catmull-rom");
  if (o.warmupSeconds < 0 || o.durationSeconds <= 0)
    throw std::invalid_argument(
        "Warmup must be nonnegative and duration positive");
  if (o.lightingPreset != "auto" && o.lightingPreset != "asset" &&
      o.lightingPreset != "kitchen")
    throw std::invalid_argument(
        "Lighting preset must be auto, asset or kitchen");
  if (o.lightCulling != "full" && o.lightCulling != "clustered")
    throw std::invalid_argument("Light culling must be full or clustered");
  if (o.cameraPath != "static" && o.cameraPath != "orbit")
    throw std::invalid_argument("Camera path: static or orbit");
  if (o.present != "auto" && o.present != "fifo" && o.present != "mailbox" &&
      o.present != "immediate")
    throw std::invalid_argument(
        "Present mode: auto, fifo, mailbox or immediate");
  if (o.presentSync != "auto" && o.presentSync != "fence" &&
      o.presentSync != "legacy")
    throw std::invalid_argument(
        "Present synchronization: auto, fence or legacy");
  if (o.benchmarkDirectory && !sizeSet) {
    o.width = 1920;
    o.height = 1080;
  }
  if (o.benchmarkDirectory && !o.scene && !o.help)
    throw std::invalid_argument("Benchmark requires a scene path");
  return o;
}

std::string_view viewerUsage() {
  return "Usage: vulkan [options] [scene.gltf|scene.glb|scene.obj]\n"
         "  --taa-history bilinear|catmull-rom (default catmull-rom)\n"
         "  --ao on|off --ao-radius R --ao-strength S (default on, .5, 1)\n"
         "  --ao-debug none|raw|filtered\n"
         "  --render-method raster|ray-tracing (default raster)\n"
         "  --no-taa           Disable temporal resolve and jitter\n"
         "  --benchmark DIR    Save frames.csv and summary.json; then exit\n"
         "  --warmup SECONDS   Default 30; excluded from measurement\n"
         "  --duration SECONDS Default 120\n"
         "  --size WIDTHxHEIGHT (benchmark default: 1920x1080)\n"
         "  --camera-path static|orbit (default static)\n"
         "  --light-culling full|clustered (default clustered)\n"
         "  --camera-culling on|off --shadow-culling on|off (default on)\n"
         "  --lighting-preset auto|asset|kitchen (auto: kitchen preview for "
         "known fixture)\n"
         "  --present auto|fifo|mailbox|immediate\n"
         "  --present-sync auto|fence|legacy (default auto)\n"
         "  --frames-in-flight 1|2 (default 1; shared frame targets)\n"
         "  --gpu NAME         Require a matching Vulkan device name\n"
         "  --no-ui            Disable UI during measurement\n"
         "  --validation       Request validation even in Release\n";
}
