#version 450

layout(set = 0, binding = 0) uniform FrameUbo {
  mat4 viewProj;
} ubo;

layout(push_constant) uniform PushConstants {
  mat4 transform;
} pushConstants;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 outColor;
layout(location = 1) out vec2 outUv;

void main()
{
  gl_Position = ubo.viewProj * pushConstants.transform * vec4(inPosition, 1.0);
  outColor = inColor;
  outUv = inUv;
}
