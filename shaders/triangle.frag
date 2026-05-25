#version 450

layout(set = 1, binding = 0) uniform sampler2D albedoTexture;

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
} pushConstants;

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec4 outFragColor;

void main()
{
  vec4 texel = texture(albedoTexture, inUv);
  outFragColor = vec4(texel.rgb * inColor * pushConstants.materialTint.rgb,
      texel.a * pushConstants.materialTint.a);
}
