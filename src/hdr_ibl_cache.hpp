#pragma once
#include "hdr_ibl.hpp"
#include <filesystem>

struct EnvironmentBakeResult {
  BakedEnvironment data;
  bool cacheHit = false;
  bool cacheStored = false;
};
// CPU-only cache. Hashes locate files; exact source/settings comparison decides
// identity. Missing/corrupt/unwritable caches fall back to deterministic
// baking.
EnvironmentBakeResult
loadOrBakeEnvironment(HdrImage const &, EnvironmentBakeSettings const &,
                      std::filesystem::path const &cacheDirectory);
