#version 450
layout(set = 0, binding = 0) uniform sampler2D sceneHdr;
layout(push_constant) uniform DisplayPush {
  float exposure;
  uint toneMap;
  uint encodeSrgb;
} display;
layout(location = 0) out vec4 outColor;
// Retain the project's filmic fit. This is not the complete ACES color pipeline.
vec3 filmicFit(vec3 c) {
  return clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
}
vec3 linearToSrgb(vec3 c) {
  return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
             12.92 * c, lessThanEqual(c, vec3(0.0031308)));
}
void main() {
  vec3 c = max(texelFetch(sceneHdr, ivec2(gl_FragCoord.xy), 0).rgb, vec3(0.0));
  c *= display.exposure;
  c = display.toneMap != 0u ? filmicFit(c) : clamp(c, 0.0, 1.0);
  if (display.encodeSrgb != 0u) c = linearToSrgb(c);
  outColor = vec4(c, 1.0);
}
