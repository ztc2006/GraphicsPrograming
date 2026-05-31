#version 450

layout(set = 1, binding = 0) uniform sampler2D albedoTexture;
layout(set = 1, binding = 1) uniform sampler2D normalTexture;
layout(set = 1, binding = 2) uniform sampler2D heightTexture;
layout(set = 1, binding = 3) uniform sampler2D alphaMaskTexture;

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
  vec4 surfaceParams;
  vec4 alphaParams;
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
layout(location = 5) in vec4 inWorldTangent;
layout(location = 0) out vec4 outFragColor;

bool uvInsideUnitSquare(vec2 uv) {
  return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

vec3 safeNormalize(vec3 value, vec3 fallback) {
  float lengthSquared = dot(value, value);
  if (lengthSquared <= 0.000001) {
    return fallback;
  }
  return value * inversesqrt(lengthSquared);
}

float alphaMaskValue(vec4 alphaTexel) {
  return max(max(alphaTexel.r, alphaTexel.g),
             max(alphaTexel.b, alphaTexel.a));
}

vec2 parallaxOcclusionUv(vec2 uv, vec3 tangentViewDirection) {
  if (pushConstants.surfaceParams.w < 0.5 ||
      pushConstants.surfaceParams.y <= 0.0001) {
    return uv;
  }

  if (tangentViewDirection.z <= 0.05) {
    return uv;
  }

  float viewAlignment = clamp(tangentViewDirection.z, 0.05, 1.0);
  float layerCount = mix(32.0, 8.0, viewAlignment);
  float layerDepth = 1.0 / layerCount;
  vec2 rayStep = pushConstants.surfaceParams.y * tangentViewDirection.xy /
                 viewAlignment / layerCount;

  vec2 tileOffset = floor(uv);
  vec2 previousUv = fract(uv);
  vec2 currentUv = previousUv;
  float previousLayerDepth = 0.0;
  float currentLayerDepth = 0.0;
  float currentDepth = 1.0 - texture(heightTexture, currentUv).r;

  for (int layer = 0; layer < 32; ++layer) {
    if (float(layer) >= layerCount || currentLayerDepth >= currentDepth) {
      break;
    }

    previousUv = currentUv;
    previousLayerDepth = currentLayerDepth;
    currentUv -= rayStep;
    if (!uvInsideUnitSquare(currentUv)) {
      return tileOffset + previousUv;
    }
    currentLayerDepth += layerDepth;
    currentDepth = 1.0 - texture(heightTexture, currentUv).r;
  }

  vec2 lowUv = previousUv;
  vec2 highUv = currentUv;
  float lowLayerDepth = previousLayerDepth;
  float highLayerDepth = currentLayerDepth;
  for (int refine = 0; refine < 5; ++refine) {
    vec2 midUv = (lowUv + highUv) * 0.5;
    if (!uvInsideUnitSquare(midUv)) {
      return tileOffset + lowUv;
    }
    float midLayerDepth = (lowLayerDepth + highLayerDepth) * 0.5;
    float midDepth = 1.0 - texture(heightTexture, midUv).r;
    if (midLayerDepth < midDepth) {
      lowUv = midUv;
      lowLayerDepth = midLayerDepth;
    } else {
      highUv = midUv;
      highLayerDepth = midLayerDepth;
    }
  }

  return tileOffset + highUv;
}

vec3 normalFromHeight(vec2 uv) {
  vec2 texel = 1.0 / vec2(textureSize(heightTexture, 0));
  float heightLeft = texture(heightTexture, uv - vec2(texel.x, 0.0)).r;
  float heightRight = texture(heightTexture, uv + vec2(texel.x, 0.0)).r;
  float heightDown = texture(heightTexture, uv - vec2(0.0, texel.y)).r;
  float heightUp = texture(heightTexture, uv + vec2(0.0, texel.y)).r;
  return normalize(vec3((heightLeft - heightRight) * pushConstants.surfaceParams.x,
                        (heightDown - heightUp) * pushConstants.surfaceParams.x,
                        1.0));
}

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
  vec3 N = safeNormalize(inWorldNormal, vec3(0.0, 0.0, 1.0));
  if (!gl_FrontFacing) {
    N = -N;
  }
  vec3 T = safeNormalize(inWorldTangent.xyz, vec3(1.0, 0.0, 0.0));
  T = safeNormalize(T - N * dot(N, T), vec3(1.0, 0.0, 0.0));
  vec3 B = cross(N, T);
  if (dot(B, B) <= 0.000001) {
    B = vec3(0.0, 1.0, 0.0);
  } else {
    B = normalize(B);
  }
  B *= inWorldTangent.w;
  mat3 tangentToWorld = mat3(T, B, N);

  vec3 L = normalize(ubo.lightDirection.xyz);
  vec3 V = normalize(ubo.cameraPosition.xyz - inWorldPos);
  vec3 tangentViewDirection = normalize(transpose(tangentToWorld) * V);
  vec2 uv = parallaxOcclusionUv(inUv, tangentViewDirection);

  vec4 texel = texture(albedoTexture, uv);
  vec4 alphaTexel = texture(alphaMaskTexture, uv);
  float alpha =
      texel.a * alphaMaskValue(alphaTexel) * pushConstants.materialTint.a;
  int alphaMode = int(round(pushConstants.alphaParams.x));
  if (alphaMode == 1 && alpha < pushConstants.alphaParams.y) {
    discard;
  }
  if (alphaMode == 0) {
    alpha = 1.0;
  }

  vec3 albedo = texel.rgb * inColor * pushConstants.materialTint.rgb;
  if (pushConstants.surfaceParams.z > 0.5) {
    vec3 tangentNormal = texture(normalTexture, uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= pushConstants.surfaceParams.x;
    N = normalize(tangentToWorld * normalize(tangentNormal));
  } else if (pushConstants.surfaceParams.w > 0.5) {
    N = normalize(tangentToWorld * normalFromHeight(uv));
  }

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
    outFragColor = vec4(vec3(visibility), alpha);
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

  outFragColor = vec4(lit, alpha);
}
