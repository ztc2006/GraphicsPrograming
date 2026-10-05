#version 450

layout(set = 1, binding = 0) uniform sampler2D albedoTexture;
layout(set = 1, binding = 3) uniform sampler2D alphaMaskTexture;
layout(set = 1, binding = 8) uniform sampler2D baseAlbedoTexture;
layout(set = 1, binding = 9) uniform sampler2D baseAlphaMaskTexture;

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
  vec4 surfaceParams;
  vec4 alphaParams;
} pushConstants;

layout(location = 0) in vec2 inUv;
layout(location = 1) in float inAlpha;

void main() {
  int alphaMode = int(round(pushConstants.alphaParams.x));
  if (alphaMode != 1) {
    return;
  }

  bool useBase = pushConstants.alphaParams.z != 1.0 ||
                 pushConstants.alphaParams.w < 0.5;
  vec4 texel = useBase ? texture(baseAlbedoTexture, inUv)
                      : texture(albedoTexture, inUv);
  vec4 alphaTexel = useBase ? texture(baseAlphaMaskTexture, inUv)
                           : texture(alphaMaskTexture, inUv);
  float alpha =
      texel.a * alphaTexel.r * pushConstants.materialTint.a * inAlpha;
  if (alpha < pushConstants.alphaParams.y) {
    discard;
  }
}
