layout(set = 0, binding = 7, std430) readonly buffer ClusterConfig {
  mat4 inverseViewProj;
  vec4 cameraNear;
  vec4 forwardFar;
  uvec4 grid;
  uvec4 screen;
} clusters;
#ifdef CLUSTER_CULL_STAGE
layout(set = 0, binding = 8, std430) buffer ClusterIndices { uint words[]; } clusterIndices;
#else
layout(set = 0, binding = 8, std430) readonly buffer ClusterIndices { uint words[]; } clusterIndices;
// Return an all-light sentinel whenever list coverage is not proven.
uint fragmentCluster(vec3 position, vec2 pixel) {
  if (clusters.screen.z == 0u) return 0xffffffffu;
  float depth = dot(position - clusters.cameraNear.xyz, clusters.forwardFar.xyz);
  if (isnan(depth) || isinf(depth) || depth < clusters.cameraNear.w || depth > clusters.forwardFar.w)
    return 0xffffffffu;
  uvec2 tile = min(uvec2(max(pixel, vec2(0))) / 64u, clusters.grid.xy - 1u);
  float slice = log(depth / clusters.cameraNear.w) / log(clusters.forwardFar.w / clusters.cameraNear.w);
  uint z = min(uint(max(slice, 0.0) * float(clusters.grid.z)), clusters.grid.z - 1u);
  return tile.x + clusters.grid.x * (tile.y + clusters.grid.y * z);
}
#endif
