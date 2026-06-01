#pragma once

#include <filesystem>
#include <string>

#include "imported_scene.hpp"

ImportedScene loadStaticGltfScene(std::filesystem::path const &path,
                                  std::string fallbackAlbedoPath);
ImportedScene loadStaticGlbScene(std::filesystem::path const &path,
                                 std::string fallbackAlbedoPath);
