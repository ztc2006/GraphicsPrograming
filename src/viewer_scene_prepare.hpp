#pragma once
#include "imported_scene.hpp"
struct ViewerSceneRepair {
  std::size_t zeroArea = 0, duplicates = 0, lightCards = 0;
};
ViewerSceneRepair prepareImportedSceneForViewer(ImportedScene &);
