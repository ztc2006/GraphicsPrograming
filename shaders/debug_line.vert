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
} pushConstants;

layout(location = 0) in vec3 inPosition;

void main() {
  gl_Position = ubo.viewProj * pushConstants.transform * vec4(inPosition, 1.0);
}
