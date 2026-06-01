#pragma once

#include <filesystem>
#include <string>

#include "imported_scene.hpp"

ImportedScene loadStaticObjScene(std::filesystem::path const &path,
                                 std::string fallbackAlbedoPath);
