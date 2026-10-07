#version 450
#extension GL_GOOGLE_include_directive : require
#include "indoor_lighting.glsl"

layout(set = 0, binding = 0) uniform FrameUbo {
  mat4 viewProj;
  vec4 cameraPosition;
  vec4 lightDirection;
  vec4 lightColor;
  vec4 ambientColor;
  vec4 lightingParams;
  mat4 lightViewProj;
  vec4 shadowParams;
} ubo;

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
  vec4 surfaceParams;
  vec4 alphaParams;
  ivec4 shadowPass;
} pushConstants;

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inUv;
layout(location = 9) in float inAlpha;

layout(location = 0) out vec2 outUv;
layout(location = 1) out float outAlpha;

void main() {
  int index = pushConstants.shadowPass.x;
  mat4 lightMatrix = index >= 0 ? indoor.spotViewProj[index]
      : index == -1 ? ubo.lightViewProj : indoor.sunViewProj[-index - 2];
  gl_Position = lightMatrix * pushConstants.transform *
                vec4(inPosition, 1.0);
  outUv = inUv;
  outAlpha = inAlpha;
}
