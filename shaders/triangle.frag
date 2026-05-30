#version 450

layout(set = 1, binding = 0) uniform sampler2D albedoTexture;

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
}
pushConstants;

layout(set = 0, binding = 0) uniform FrameUbo {
  mat4 viewProj;
  vec4 cameraPosition;
  vec4 lightDirection;
  vec4 lightColor;
  vec4 ambientColor;
  vec4 lightingParams;
  mat4 lightViewProj;
  vec4 shadowParams;
}
ubo;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 0, binding = 2) uniform sampler2D shadowDebugMap;

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec3 inWorldPos;
layout(location = 3) in vec3 inWorldNormal;
layout(location = 4) in vec4 inLightClipPos;
layout(location = 0) out vec4 outFragColor;

float shadowVisibility(vec4 lightClipPos, vec3 N, vec3 L) {
  vec3 proj = lightClipPos.xyz / lightClipPos.w;
  vec2 uv = proj.xy * 0.5 + 0.5;

  if (proj.z < 0.0 || proj.z > 1.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 ||
      uv.y > 1.0) {
    return 1.0;
  }

  float bias =
      max(ubo.shadowParams.x * (1.0 - max(dot(N, L), 0.0)), ubo.shadowParams.y);
  float radius = max(ubo.shadowParams.z, 0.0);
  int sampleRadius = int(floor(radius));
  float blend = fract(radius);
  vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));

  float visible = 0.0;
  float sampleCount = 0.0;
  for (int y = -4; y <= 4; ++y) {
    for (int x = -4; x <= 4; ++x) {
      vec2 offset = vec2(x, y);
      float dist = max(abs(offset.x), abs(offset.y));
      if (dist > float(sampleRadius) + blend) {
        continue;
      }
      float weight = dist <= float(sampleRadius) ? 1.0 : blend;
      visible +=
          weight * texture(shadowMap, vec3(uv + offset * texel, proj.z - bias));
      sampleCount += weight;
    }
  }
  return visible / max(sampleCount, 1.0);
}

void main() {
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
  float specular = nDotL > 0.0
                       ? pow(max(dot(N, H), 0.0), ubo.lightingParams.z) *
                             ubo.lightingParams.y
                       : 0.0;

  int shadowMode = int(round(ubo.shadowParams.w));
  float visibility =
      shadowMode == 0 ? 1.0 : shadowVisibility(inLightClipPos, N, L);

  if (shadowMode == 2) {
    outFragColor =
        vec4(vec3(visibility), texel.a * pushConstants.materialTint.a);
    return;
  }

  if (shadowMode == 3) {
    vec3 proj = inLightClipPos.xyz / inLightClipPos.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (proj.z < 0.0 || proj.z > 1.0 || uv.x < 0.0 || uv.x > 1.0 ||
        uv.y < 0.0 || uv.y > 1.0) {
      outFragColor = vec4(0.05, 0.05, 0.05, 1.0);
    } else {
      float depth = texture(shadowDebugMap, uv).r;
      outFragColor = vec4(vec3(depth), 1.0);
    }
    return;
  }

  vec3 lit = albedo * ubo.ambientColor.rgb +
             visibility * (albedo * ubo.lightColor.rgb * diffuse +
                           ubo.lightColor.rgb * specular);

  outFragColor = vec4(lit, texel.a * pushConstants.materialTint.a);
}
