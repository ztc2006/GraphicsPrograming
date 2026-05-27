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
  vec4 lightingParams;
  mat4 lightViewProj;
} ubo;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec3 inWorldPos;
layout(location = 3) in vec3 inWorldNormal;
layout(location = 4) in vec4 inLightClipPos;
layout(location = 0) out vec4 outFragColor;

float shadowVisibility(vec4 lightClipPos, vec3 N, vec3 L) {
  vec3 proj = lightClipPos.xyz / lightClipPos.w;
  vec2 uv = proj.xy * 0.5 + 0.5;

  if (proj.z < 0.0 || proj.z > 1.0 ||
      uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
    return 1.0;
  }

  float bias = max(0.0025 * (1.0 - max(dot(N, L), 0.0)), 0.0007);
  vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));

  float visible = 0.0;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      visible += texture(shadowMap, vec3(uv + vec2(x, y) * texel, proj.z - bias));
    }
  }
  return visible / 9.0;
}

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
  float diffuse = max(nDotL, 0.0) * ubo.lightingParams.x;
  float specular = nDotL > 0.0 ? pow(max(dot(N, H), 0.0), ubo.lightingParams.z)
      * ubo.lightingParams.y : 0.0;

  float visibility = shadowVisibility(inLightClipPos, N, L);

  vec3 lit = albedo * ubo.ambientColor.rgb
      + visibility * (albedo * ubo.lightColor.rgb * diffuse
          + ubo.lightColor.rgb * specular);

  outFragColor = vec4(lit, texel.a * pushConstants.materialTint.a);
}
