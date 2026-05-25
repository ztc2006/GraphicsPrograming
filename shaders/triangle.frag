#version 450

layout(set = 1, binding = 0) uniform sampler2D albedoTexture;

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
} pushConstants;

layout(set = 0, binding = 0) uniform FrameUbo {
  mat4 viewProj;
  vec4 cameraPosition;
  vec4 lightDirection;
  vec4 lightColor;
  vec4 ambientColor;
} ubo;

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec3 inWorldPos;
layout(location = 3) in vec3 inWorldNormal;
layout(location = 0) out vec4 outFragColor;

void main()
{
  vec4 texel = texture(albedoTexture, inUv);
  vec3 albedo = texel.rgb * inColor * pushConstants.materialTint.rgb;

  vec3 N = normalize(inWorldNormal);
  if (!gl_FrontFacing) {
    N = -N;
  }
  vec3 L = normalize(ubo.lightDirection.xyz);
  vec3 V = normalize(ubo.cameraPosition.xyz - inWorldPos);
  vec3 H = normalize(L + V);

  float nDotL = dot(N, L);
  float diffuse = max(nDotL, 0.0);
  float specular = nDotL > 0.0 ? pow(max(dot(N, H), 0.0), 32.0) : 0.0;

  vec3 lit = albedo * (ubo.ambientColor.rgb + ubo.lightColor.rgb * diffuse)
      + ubo.lightColor.rgb * specular;

  outFragColor = vec4(lit, texel.a * pushConstants.materialTint.a);
}
