#include "presentation.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

PresentationPolicy parsePresentationPolicy(std::string const &value) {
  if (value == "auto") return PresentationPolicy::Automatic;
  if (value == "fence") return PresentationPolicy::RequireFence;
  if (value == "legacy") return PresentationPolicy::Legacy;
  throw std::invalid_argument("Present synchronization: auto, fence or legacy");
}
char const *presentationBackendName(PresentationBackend backend) {
  switch (backend) {
  case PresentationBackend::KhrFence: return "KHR_present_fence";
  case PresentationBackend::ExtFence: return "EXT_present_fence";
  case PresentationBackend::Legacy: return "legacy_wait_idle";
  }
  throw std::invalid_argument("Invalid presentation backend");
}
PresentationInstanceSupport enablePresentationInstanceExtensions(
    std::span<vk::ExtensionProperties const> available,
    std::vector<char const *> &enabled, PresentationPolicy policy) {
  if (policy == PresentationPolicy::Legacy) return {};
  auto has = [&](char const *name) {
    return std::any_of(available.begin(), available.end(), [&](auto const &p) {
      return std::strcmp(p.extensionName, name) == 0;
    });
  };
  auto add = [&](char const *name) {
    if (std::none_of(enabled.begin(), enabled.end(), [&](auto p) { return std::strcmp(p, name) == 0; }))
      enabled.push_back(name);
  };
  bool surfaceEnabled = std::any_of(enabled.begin(), enabled.end(), [](auto p) {
    return std::strcmp(p, vk::KHRSurfaceExtensionName) == 0;
  });
  if (!surfaceEnabled || !has(vk::KHRGetSurfaceCapabilities2ExtensionName)) return {};
  PresentationInstanceSupport support{has(vk::KHRSurfaceMaintenance1ExtensionName),
                                       has(vk::EXTSurfaceMaintenance1ExtensionName)};
  if (support.khr || support.ext) add(vk::KHRGetSurfaceCapabilities2ExtensionName);
  if (support.khr) add(vk::KHRSurfaceMaintenance1ExtensionName);
  if (support.ext) add(vk::EXTSurfaceMaintenance1ExtensionName);
  return support;
}
PresentationSupport choosePresentationSupport(PresentationInstanceSupport instance,
    PresentationDeviceSupport device, PresentationPolicy policy) {
  PresentationSupport support;
  if (policy == PresentationPolicy::Legacy) support.reason = "legacy explicitly requested";
  else if (!instance.khr && !instance.ext) support.reason = "surface maintenance instance dependencies not enabled";
  else if (!(instance.khr && device.khr) && !(instance.ext && device.ext))
    support.reason = "matching swapchain maintenance device extension unavailable";
  else if (!device.feature) support.reason = "swapchainMaintenance1 feature unavailable";
  else if (instance.khr && device.khr)
    support = {PresentationBackend::KhrFence, "KHR extension, instance dependencies and feature enabled"};
  else support = {PresentationBackend::ExtFence, "EXT extension, instance dependencies and feature enabled"};
  if (policy == PresentationPolicy::RequireFence && !support.fencesEnabled())
    throw std::runtime_error("Present fence required: " + support.reason);
  return support;
}
PresentEnqueueState classifyPresentResult(vk::Result result) {
  switch (result) {
  case vk::Result::eSuccess:
  case vk::Result::eSuboptimalKHR:
  case vk::Result::eErrorOutOfDateKHR:
  case vk::Result::eErrorSurfaceLostKHR:
#ifdef VK_USE_PLATFORM_WIN32_KHR
  case vk::Result::eErrorFullScreenExclusiveModeLostEXT:
#endif
  case vk::Result::eErrorPresentTimingQueueFullEXT:
    return PresentEnqueueState::Enqueued;
  case vk::Result::eErrorOutOfHostMemory:
  case vk::Result::eErrorOutOfDeviceMemory:
    return PresentEnqueueState::Rejected;
  case vk::Result::eErrorDeviceLost: return PresentEnqueueState::DeviceLost;
  default: return PresentEnqueueState::Unknown;
  }
}
