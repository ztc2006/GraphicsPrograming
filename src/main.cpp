#include "application.hpp"
#include "pch.hpp"
#include "viewer_options.hpp"
#include <cstdlib>
#include <iostream>
int main(int argc, char **argv) {
  try {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i)
      args.emplace_back(argv[i]);
    auto options = parseViewerOptions(args);
    if (options.help) {
      std::cout << viewerUsage();
      return EXIT_SUCCESS;
    }
    std::filesystem::current_path(VULKAN_RESOURCE_ROOT);
    Application app(std::move(options));
    app.run();
  } catch (std::exception const &error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
