# Local cgltf adapter patch (2026-10-05)

The upstream version/commit in CMake remains pinned. The original header hash
is retained in project dependency provenance. This local patch adds only
`cgltf_light.has_range` and `has_spot`, set on parsing those properties.
It distinguishes omitted range (infinite) from explicit zero (invalid), and
rejects a spot without its required object without a second JSON parser.
All users and cgltf implementation compile against this same header; this
changes the local C structure ABI. Other upstream bytes/semantics unchanged.
Importer/default/invalid/GLB regressions and sanitizers cover the patch.
