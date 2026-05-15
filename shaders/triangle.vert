#version 450

layout(push_constant) uniform PushConstants {
  mat4 transform;
} pushConstants;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 outColor;

void main()
{
  gl_Position = pushConstants.transform * vec4(inPosition, 0.0, 1.0);
  outColor = inColor;
}
