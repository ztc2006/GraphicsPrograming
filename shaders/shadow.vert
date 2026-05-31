#version 450

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
} pushConstants;

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inUv;

layout(location = 0) out vec2 outUv;

void main() {
  gl_Position = ubo.lightViewProj * pushConstants.transform *
                vec4(inPosition, 1.0);
  outUv = inUv;
}
