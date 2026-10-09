#version 450
#extension GL_GOOGLE_include_directive : require
#include "temporal_motion.glsl"

layout(set = 0, binding = 0) uniform FrameUbo {
  mat4 viewProj;
  vec4 cameraPosition;
  vec4 lightDirection;
  vec4 lightColor;
  vec4 ambientColor;
  vec4 lightingParams;
  mat4 lightViewProj;
  vec4 shadowParams;
  mat4 inverseViewProj;
  vec4 environmentParams;
  vec4 environmentSh[9];
  mat4 currentViewProj;
  mat4 previousViewProj;
  vec4 previousCamera;
  vec4 jitterUv;
} ubo;

layout(set = 0, binding = 3) uniform sampler2D environmentTexture;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outFragColor;
layout(location=1) out vec4 outMotion;
layout(location=2) out vec4 outIndirectDiffuse;

const vec2 INV_ATAN = vec2(0.15915494309, 0.31830988618);

vec2 directionToEquirectangularUv(vec3 direction) {
  vec2 uv = vec2(atan(direction.z, direction.x), asin(direction.y));
  return uv * INV_ATAN + 0.5;
}



void main() {
  outIndirectDiffuse=vec4(0);
  vec2 ndc = inUv * 2.0 - 1.0;
  vec4 nearPoint = ubo.inverseViewProj * vec4(ndc, 0.0, 1.0);
  nearPoint /= nearPoint.w;
  vec3 direction = normalize(nearPoint.xyz - ubo.cameraPosition.xyz);

  vec4 current = ubo.currentViewProj * vec4(direction,0);
  vec4 previous = ubo.previousViewProj * vec4(direction,0);
  // Infinite environment has depth 1 regardless of finite camera projection.
  current.z = current.w; previous.z = previous.w;
  outMotion = temporalMotion(current, previous, ubo.previousCamera.w);
  outMotion.w=0; // Infinite sky depth sentinel.
  int debugMode = int(round(ubo.lightingParams.w));
  if (debugMode == 18 || debugMode == 19) {
    outFragColor = vec4(temporalMotionDebug(outMotion,debugMode),1); return;
  }
  float rotation = ubo.environmentParams.y;
  float cosine = cos(rotation);
  float sine = sin(rotation);
  direction.xz = mat2(cosine, -sine, sine, cosine) * direction.xz;

  vec3 hdr = texture(environmentTexture,
                     directionToEquirectangularUv(direction)).rgb;
  outFragColor = vec4(hdr * ubo.environmentParams.x, 1.0);
}
