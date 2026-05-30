#pragma once

#include <filesystem>
#include <string>

#include "loaded_scene.hpp"

LoadedScene loadStaticObjScene(std::filesystem::path const &path,
                               std::string fallbackAlbedoPath);
