#version 450

layout(set = 0, binding = 1) uniform sampler2D albedoTexture;

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec4 outFragColor;

void main()
{
  vec3 texel = texture(albedoTexture, inUv).rgb;
  outFragColor = vec4(texel * inColor, 1.0);
}
