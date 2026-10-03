#pragma once

#include "scene.hpp"

enum class RasterPass {
  Main,
  Shadow,
};

enum class MaterialPipelineVariant {
  OpaqueSingleSided,
  OpaqueDoubleSided,
  TransparentSingleSided,
  TransparentDoubleSided,
  ShadowSingleSided,
  ShadowDoubleSided,
};

constexpr MaterialPipelineVariant selectMaterialPipeline(
    AlphaMode alphaMode, bool doubleSided, RasterPass pass) {
  if (pass == RasterPass::Shadow) {
    return doubleSided ? MaterialPipelineVariant::ShadowDoubleSided
                       : MaterialPipelineVariant::ShadowSingleSided;
  }
  if (alphaMode == AlphaMode::Blend) {
    return doubleSided ? MaterialPipelineVariant::TransparentDoubleSided
                       : MaterialPipelineVariant::TransparentSingleSided;
  }
  return doubleSided ? MaterialPipelineVariant::OpaqueDoubleSided
                     : MaterialPipelineVariant::OpaqueSingleSided;
}
