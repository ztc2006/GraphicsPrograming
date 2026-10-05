#include "presentation.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

void require(bool ok, char const *message) { if (!ok) throw std::runtime_error(message); }
int main() {
  try {
    std::vector<vk::ExtensionProperties> available;
    for (auto name : {vk::KHRSurfaceExtensionName, vk::KHRGetSurfaceCapabilities2ExtensionName,
                      vk::KHRSurfaceMaintenance1ExtensionName, vk::EXTSurfaceMaintenance1ExtensionName}) {
      vk::ExtensionProperties p{}; std::strcpy(p.extensionName, name); available.push_back(p);
    }
    std::vector<char const *> enabled{vk::KHRSurfaceExtensionName};
    auto instance = enablePresentationInstanceExtensions(available, enabled, PresentationPolicy::Automatic);
    require(instance.khr && instance.ext && enabled.size() == 4, "Instance dependency families incomplete");
    enablePresentationInstanceExtensions(available, enabled, PresentationPolicy::Automatic);
    require(enabled.size() == 4, "Duplicate instance extensions appended");
    enabled = {vk::KHRSurfaceExtensionName};
    auto legacy = enablePresentationInstanceExtensions(available, enabled, PresentationPolicy::Legacy);
    require(!legacy.khr && !legacy.ext && enabled.size() == 1, "Legacy enabled optional dependencies");
    enabled.clear();
    auto noSurface = enablePresentationInstanceExtensions(available, enabled, PresentationPolicy::Automatic);
    require(!noSurface.khr && !noSurface.ext && enabled.empty(), "Missing base surface dependency ignored");
    enabled = {vk::KHRSurfaceExtensionName}; available.erase(available.begin() + 1);
    auto noCaps = enablePresentationInstanceExtensions(available, enabled, PresentationPolicy::Automatic);
    require(!noCaps.khr && !noCaps.ext && enabled.size() == 1, "Missing capabilities2 dependency ignored");
    auto choose = [](auto i, auto d, auto p) { return choosePresentationSupport(i, d, p).backend; };
    require(choose(PresentationInstanceSupport{true,true}, PresentationDeviceSupport{true,true,true}, PresentationPolicy::Automatic) == PresentationBackend::KhrFence,
            "Both families must prefer KHR");
    require(choose(PresentationInstanceSupport{false,true}, PresentationDeviceSupport{true,true,true}, PresentationPolicy::RequireFence) == PresentationBackend::ExtFence,
            "EXT alias family failed");
    for (auto pair : {std::pair{PresentationInstanceSupport{}, PresentationDeviceSupport{true,true,true}},
                      std::pair{PresentationInstanceSupport{true,false}, PresentationDeviceSupport{false,true,true}},
                      std::pair{PresentationInstanceSupport{true,true}, PresentationDeviceSupport{true,true,false}}}) {
      require(choose(pair.first, pair.second, PresentationPolicy::Automatic) == PresentationBackend::Legacy, "Missing dependency/feature selected fences");
      bool rejected = false;
      try { choose(pair.first, pair.second, PresentationPolicy::RequireFence); } catch (std::runtime_error const &) { rejected = true; }
      require(rejected, "Required fence silently fell back");
    }
    require(choose(instance, PresentationDeviceSupport{true,true,true}, PresentationPolicy::Legacy) == PresentationBackend::Legacy, "Legacy policy ignored");
    for (auto result : {vk::Result::eSuccess, vk::Result::eSuboptimalKHR, vk::Result::eErrorOutOfDateKHR,
                        vk::Result::eErrorSurfaceLostKHR,
                        vk::Result::eErrorPresentTimingQueueFullEXT})
      require(classifyPresentResult(result) == PresentEnqueueState::Enqueued, "Enqueued WSI error loses present fence");
    for (auto result : {vk::Result::eErrorOutOfHostMemory, vk::Result::eErrorOutOfDeviceMemory})
      require(classifyPresentResult(result) == PresentEnqueueState::Rejected, "Rejected OOM waits an unsubmitted fence");
    require(classifyPresentResult(vk::Result::eErrorDeviceLost) == PresentEnqueueState::DeviceLost &&
                classifyPresentResult(vk::Result::eErrorUnknown) == PresentEnqueueState::Unknown,
            "Lost/unknown errors claim completion");
    require(parsePresentationPolicy("auto") == PresentationPolicy::Automatic &&
                parsePresentationPolicy("fence") == PresentationPolicy::RequireFence &&
                parsePresentationPolicy("legacy") == PresentationPolicy::Legacy, "Policy mapping broken");
    std::cout << "PASS instance/device/feature dependency matrix, required/legacy policy, present result enqueue classification\n";
  } catch (std::exception const &e) { std::cerr << e.what() << '\n'; return 1; }
}
