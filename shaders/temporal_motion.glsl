// Unjittered current - previous UV, positive down for a positive Vulkan viewport.
// z is signed validity (+1 previous valid, -1 opaque current/no previous); w is previous linear clip.w depth. Transparent blending invalidates z.
vec4 temporalMotion(vec4 currentClip, vec4 previousClip, float valid) {
  if (valid < .5 || currentClip.w <= 0.0 || previousClip.w <= 0.0 ||
      any(isnan(currentClip)) || any(isinf(currentClip)) ||
      any(isnan(previousClip)) || any(isinf(previousClip))) return vec4(0,0,-1,0);
  vec3 current = currentClip.xyz / currentClip.w;
  vec3 previous = previousClip.xyz / previousClip.w;
  if (any(isnan(current)) || any(isinf(current)) || any(isnan(previous)) || any(isinf(previous)) ||
      any(lessThan(previous.xy, vec2(-1))) || any(greaterThan(previous.xy, vec2(1))) ||
      previous.z < 0.0 || previous.z > 1.0) return vec4(0,0,-1,0);
  return vec4((current.xy - previous.xy) * .5, 1.0, previousClip.w);
}
vec3 temporalMotionDebug(vec4 motion, int mode) {
  return mode == 19 ? vec3(motion.z) : (motion.z > .5 ? vec3(.5 + motion.xy * 16.0, .5) : vec3(1,0,1));
}
