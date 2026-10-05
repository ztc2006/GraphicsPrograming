#pragma once
#include "vulkan_include.hpp"
#include <span>
#include <string>
#include <vector>

enum class PresentationPolicy { Automatic, RequireFence, Legacy };
enum class PresentationBackend { Legacy, KhrFence, ExtFence };
struct PresentationInstanceSupport { bool khr = false, ext = false; };
struct PresentationDeviceSupport { bool khr = false, ext = false, feature = false; };
struct PresentationSupport {
  PresentationBackend backend = PresentationBackend::Legacy;
  std::string reason;
  bool fencesEnabled() const { return backend != PresentationBackend::Legacy; }
};
// Record what is actually enabled on the instance; advertised support alone is
// insufficient for enabling a dependent device extension.
PresentationInstanceSupport enablePresentationInstanceExtensions(
    std::span<vk::ExtensionProperties const> available,
    std::vector<char const *> &enabled, PresentationPolicy policy);
PresentationSupport choosePresentationSupport(PresentationInstanceSupport instance,
    PresentationDeviceSupport device, PresentationPolicy policy);
PresentationPolicy parsePresentationPolicy(std::string const &value);
char const *presentationBackendName(PresentationBackend backend);
enum class PresentEnqueueState { Enqueued, Rejected, DeviceLost, Unknown };
PresentEnqueueState classifyPresentResult(vk::Result result);
