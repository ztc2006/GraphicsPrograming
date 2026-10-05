#include "presentation_regression.hpp"
#include "renderer.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, char const *message) { if (!ok) throw std::runtime_error(message); }
}
struct SwapChainPresentationTestAccess {
  // Exercise production result bookkeeping with an actual unsubmitted fence.
  // This does not induce real driver OOM and is reported separately.
  static void rejected(SwapChain const &s, Device const &device, vk::Result result) {
    auto &record = s.presentation_.front();
    require(!record.pending, "Result injection started with a pending present");
    if (*record.fence) device.logicalDevice().resetFences({*record.fence});
    s.recordPresentResult(0, result);
  }
};
void exercisePresentation(Device const &device, SwapChain const &swapchain, unsigned framesInFlight) {
  {
    Renderer renderer(device, framesInFlight); renderer.recreateForSwapChain(swapchain);
    std::vector<vk::Semaphore> semaphores;
    for (unsigned i = 0; i < swapchain.images().size(); ++i) {
      auto semaphore = swapchain.renderFinishedSemaphore(i);
      require(std::find(semaphores.begin(), semaphores.end(), semaphore) == semaphores.end(), "Present semaphore aliases another image");
      semaphores.push_back(semaphore);
    }
    auto start = swapchain.presentationStatistics();
    std::vector<std::uint64_t> ids;
    renderer.setGpuTimingCallback([&](auto const &s) { ids.push_back(s.frameId); });
    try {
      for (unsigned i = 0; i < 12; ++i) {
        // Match Application's event service before WSI acquire.
        glfwPollEvents();
        require(renderer.renderFrame({.sky = true}, glm::mat4(1), {0,0,2}, {}, false) == Renderer::FrameResult::eSuccess,
                "Presentation reuse frame failed");
      }
      auto pending = swapchain.presentationStatistics();
      require(pending.queued == start.queued + 12, "Present bookkeeping lost a WSI request");
      if (device.presentationSupport().fencesEnabled())
        require(pending.pendingFences > 0 && !swapchain.presentationReleaseProven(), "Undrained presentation reported resource proof");
      device.logicalDevice().waitIdle();
      renderer.collectCompletedWork();
      renderer.recreateForSwapChain(swapchain); // Renderer pipeline replacement leaves WSI owners alone.
      for (unsigned i = 0; i < semaphores.size(); ++i)
        require(swapchain.renderFinishedSemaphore(i) == semaphores[i], "Renderer reconstruction replaced present semaphore");
      glfwPollEvents();
      require(renderer.renderFrame({.sky = true}, glm::mat4(1), {0,0,2}, {}, false) == Renderer::FrameResult::eSuccess,
              "Post-rebuild presentation failed");
      swapchain.drainPresentations(); renderer.collectCompletedWork();
      auto drained = swapchain.presentationStatistics();
      require(drained.pendingFences == 0 && drained.queued == start.queued + 13 && ids.size() == 13,
              "Final drain lost present/frame completion");
      std::sort(ids.begin(), ids.end());
      for (unsigned i = 0; i < 13; ++i) require(ids[i] == i+1, "Present path duplicated/lost frame queries");
      require(swapchain.presentationReleaseProven() == device.presentationSupport().fencesEnabled(), "Legacy/fenced release proof incorrect");
      if (device.presentationSupport().fencesEnabled())
        require(drained.completed == drained.queued && drained.legacyDrains == 0, "Known present fences were not all completed");
      else require(drained.legacyDrains > 0 && drained.completed == 0, "Legacy drain impersonated fence completion");
      swapchain.drainPresentations();
      auto twice = swapchain.presentationStatistics();
      require(twice.completed == drained.completed && twice.fenceWaits == drained.fenceWaits &&
                  twice.legacyDrains == drained.legacyDrains, "Idempotent drain repeated completion");
      for (auto result : {vk::Result::eErrorOutOfHostMemory, vk::Result::eErrorOutOfDeviceMemory}) {
        SwapChainPresentationTestAccess::rejected(swapchain, device, result);
        swapchain.drainPresentations();
        auto rejected = swapchain.presentationStatistics();
        require(rejected.pendingFences == 0 && rejected.queued == drained.queued &&
                    rejected.fenceWaits == drained.fenceWaits, "Rejected result waited on an unsubmitted fence");
      }
      std::cout << "PASS WSI present: " << presentationBackendName(device.presentationSupport().backend)
                << ", 13 requests/queries, per-image reuse, Renderer rebuild preserves semaphores, idempotent drain; "
                   "injected OOM bookkeeping does not wait unsubmitted fences\n";
    } catch (...) { swapchain.drainPresentations(); throw; }
  }
  require(device.resourceLedger().snapshot().current == ResourceLedger::Footprint{}, "Presentation renderer leaked tracked storage");
}
