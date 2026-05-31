#version 450

layout(set = 1, binding = 0) uniform sampler2D albedoTexture;
layout(set = 1, binding = 3) uniform sampler2D alphaMaskTexture;

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
  vec4 surfaceParams;
  vec4 alphaParams;
} pushConstants;

layout(location = 0) in vec2 inUv;

float alphaMaskValue(vec4 alphaTexel) {
  return max(max(alphaTexel.r, alphaTexel.g),
             max(alphaTexel.b, alphaTexel.a));
}

void main() {
  int alphaMode = int(round(pushConstants.alphaParams.x));
  if (alphaMode != 1) {
    return;
  }

  vec4 texel = texture(albedoTexture, inUv);
  vec4 alphaTexel = texture(alphaMaskTexture, inUv);
  float alpha =
      texel.a * alphaMaskValue(alphaTexel) * pushConstants.materialTint.a;
  if (alpha < pushConstants.alphaParams.y) {
    discard;
  }
}
