#version 450

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
} pushConstants;

layout(location = 0) out vec4 outFragColor;

void main() {
  outFragColor = pushConstants.materialTint;
}
