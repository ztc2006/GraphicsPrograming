#version 450

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
  vec4 surfaceParams;
  vec4 alphaParams;
} pushConstants;

layout(location = 0) out vec4 outFragColor;
layout(location=1) out vec4 outMotion;
layout(location=2) out vec4 outIndirectDiffuse;

void main() {
  outIndirectDiffuse=vec4(0,0,0,pushConstants.materialTint.a);
  outMotion = vec4(0,0,0,pushConstants.materialTint.a);
  outFragColor = pushConstants.materialTint;
}
