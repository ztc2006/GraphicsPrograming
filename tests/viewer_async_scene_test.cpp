#include "application.hpp"
#include "renderer.hpp"

#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

// Drive the same request queue and cancel operation as the Scene panel. This
// needs a real ImGui/Vulkan backend so preview lifetime errors remain observable.
class ApplicationSceneLoadTest {
  static void require(bool condition, char const *message) {
    if (!condition)
      throw std::runtime_error(message);
  }
  static void frame(Application &app) {
    glfwPollEvents();
    app.processPendingScene();
    app.beginImGuiFrame();
    auto const extent = app.swapChain_->extent();
    auto const &camera = app.scene_.cameras.front();
    auto lighting = app.scene_.lighting;
    lighting.shadowDebugMode = 0;
    auto result = app.renderer_->beginFrame(
        camera.viewProj(float(extent.width) / extent.height), camera.position,
        lighting, false);
    require(result == Renderer::FrameResult::eSuccess, "Unexpected swapchain change");
    app.renderer_->drawEnvironment();
    if (!app.assets_.meshes.empty())
      app.renderer_->drawObject(0, 0, glm::mat4{1.0f});
    require(app.renderer_->endFrame() == Renderer::FrameResult::eSuccess,
            "Frame failed during async scene loading");
  }
  template <class Predicate> static void pump(Application &app, Predicate done) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done()) {
      require(std::chrono::steady_clock::now() < deadline, "Scene operation timed out");
      frame(app);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

public:
  static void run() {
    ViewerOptions options;
    options.width = 640;
    options.height = 480;
    options.present = "fifo";
    Application app(options);
    try {
      app.initWindow();
      app.initVulkan();
      auto const path = std::filesystem::path{TEST_FIXTURE}.lexically_normal();
      app.pendingScenePath_ = path;
      pump(app, [&] { return app.loadedScenePath_ == path && !app.sceneLoad_; });
      require(app.renderer_->resourceStatistics().sceneCommits == 1,
              "Initial request did not commit once");
      auto preview = app.materialAlbedoPreviewTexture(0);
      require(preview != 0, "Real ImGui material preview was not allocated");
      frame(app); // Make the old preview a submitted frame user.
      auto originalImage = app.renderer_->materialAlbedoTexture(0).image();
      auto const imageCopies = app.renderer_->resourceStatistics().sceneImageCopies;
      app.pendingScenePath_ = path;
      pump(app, [&] { return app.renderer_->resourceStatistics().sceneCommits == 2; });
      require(app.materialAlbedoPreviewTexture(0) != 0, "Reload lost preview binding");
      require(app.renderer_->materialAlbedoTexture(0).image() == originalImage,
              "Reload failed to reuse image storage");
      require(app.renderer_->resourceStatistics().sceneImageCopies == imageCopies,
              "Reload unnecessarily uploaded shared images");
      pump(app, [&] { return app.renderer_->resourceStatistics().retiredScenes == 0; });
      std::cout << "PASS queued load/reload with real ImGui preview retirement\n";

      auto unchanged = [&] {
        require(app.loadedScenePath_ == path && !app.assets_.meshes.empty(),
                "Failed/canceled request replaced live CPU assets");
        require(app.renderer_->resourceStatistics().sceneCommits == 2,
                "Failed/canceled request committed a GPU scene");
        require(app.renderer_->materialAlbedoTexture(0).image() == originalImage,
                "Failed/canceled request changed the live material");
        require(app.materialAlbedoPreviewTexture(0) != 0,
                "Failed/canceled request lost the live preview");
      };
      app.loadScene(path);
      app.pendingScenePath_ = TEST_BAD_FIXTURE; // Supersede before any poll/commit.
      pump(app, [&] { return !app.sceneLoad_ && !app.pendingScenePath_ &&
                            !app.sceneLoadError_.empty(); });
      unchanged();
      std::cout << "PASS latest request wins; import failure preserves live scene\n";
      app.loadScene(path);
      app.cancelSceneLoad();
      pump(app, [&] { return !app.sceneLoad_; });
      unchanged();
      require(app.sceneLoadError_.empty(), "Cancellation produced a load error");

      app.loadScene(path);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (app.renderer_->resourceStatistics().pendingSceneUploads == 0) {
        require(std::chrono::steady_clock::now() < deadline, "Upload submission timed out");
        glfwPollEvents();
        app.processPendingScene();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      // First commit attempt always returns without publishing; cancel at that
      // boundary even on drivers that complete the upload immediately.
      app.cancelSceneLoad();
      pump(app, [&] { return !app.sceneLoad_ &&
                            app.renderer_->resourceStatistics().pendingSceneUploads == 0; });
      unchanged();
      auto const &stats = app.renderer_->resourceStatistics();
      require(stats.sceneUploadFenceWaits == 0, "Runtime load waited an upload fence");
      require(stats.environmentUploads == 1 && stats.pipelineBuilds == 9,
              "Runtime load rebuilt persistent renderer resources");
      std::cout << "PASS cancellation before and after upload submission, zero upload waits\n";

      auto const optionalPath = std::filesystem::path{TEST_ADAPTER_FIXTURES} / "optional_extension.gltf";
      app.pendingScenePath_ = optionalPath;
      pump(app, [&] { return app.loadedScenePath_ == optionalPath && !app.sceneLoad_; });
      require(app.sceneLoadWarnings_.size() == 1 &&
              app.sceneLoadWarnings_[0].find("TEST_optional") != std::string::npos,
              "Committed scene lost importer warnings");
      auto const warning = app.sceneLoadWarnings_[0];
      auto const committed = app.renderer_->resourceStatistics().sceneCommits;
      app.pendingScenePath_ = std::filesystem::path{TEST_ADAPTER_FIXTURES} / "required_extension.gltf";
      pump(app, [&] { return !app.sceneLoad_ && !app.pendingScenePath_ && !app.sceneLoadError_.empty(); });
      require(app.loadedScenePath_ == optionalPath && app.sceneLoadWarnings_[0] == warning &&
              app.renderer_->resourceStatistics().sceneCommits == committed,
              "Rejected required extension changed live scene or warnings");
      std::cout << "PASS importer warnings publish with the scene; required extension rolls back\n";

      app.loadScene(TEST_SIHEYUAN_PATH);
      require(bool(app.sceneLoad_), "Shutdown test did not start preparation");
      auto ledger = app.device_->resourceLedger(); // Counters do not own GPU resources.
      app.cleanup(); // Joins preparation before borrowed Device/Renderer destruction.
      require(!app.device_ && !app.renderer_ && !app.window_, "Shutdown left consumers alive");
      require(ledger.snapshot().current == ResourceLedger::Footprint{},
              "Application shutdown left preparation or scene resources in the ledger");
      std::cout << "PASS shutdown with preparation task and preview backend alive\n";
    } catch (...) {
      app.cleanup();
      throw;
    }
  }
};

int main() {
  if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
    std::cout << "SKIP: no graphical session\n";
    return 77;
  }
  try {
    ApplicationSceneLoadTest::run();
    return 0;
  } catch (std::exception const &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
