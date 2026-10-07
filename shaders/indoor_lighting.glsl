layout(std430, set=0, binding=9) readonly buffer IndoorLighting {
  mat4 spotViewProj[4];
  vec4 spotRects[4];
  vec4 spotBias[4];
  vec4 sunRect;
  uvec4 counts;
  vec4 probeMin;
  vec4 probeMax;
  vec4 probePosition;
  vec4 probeSh[9];
  mat4 sunViewProj[4];
  vec4 sunRects[4];
  vec4 sunBias[4];
  vec4 sunSplits;
  vec4 sunForwardNear;
  vec4 sunParams;
} indoor;
