#version 450

layout(set = 1, binding = 0) uniform sampler2D albedoTexture;
layout(set = 1, binding = 1) uniform sampler2D normalTexture;
layout(set = 1, binding = 2) uniform sampler2D heightTexture;
layout(set = 1, binding = 3) uniform sampler2D alphaMaskTexture;
layout(set = 1, binding = 4) uniform sampler2D metallicRoughnessTexture;
layout(set = 1, binding = 5) uniform sampler2D occlusionTexture;
layout(set = 1, binding = 6) uniform sampler2D emissiveTexture;
layout(set = 1, binding = 7) uniform MaterialUbo {
  vec4 pbrParams;
  vec4 emissiveFactor;
} material;

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
  mat4 inverseViewProj;
  vec4 environmentParams;
  vec4 environmentSh[9];
}
ubo;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 0, binding = 2) uniform sampler2D shadowDebugMap;
layout(set = 0, binding = 3) uniform sampler2D environmentTexture;

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec3 inWorldPos;
layout(location = 3) in vec3 inWorldNormal;
layout(location = 4) in vec4 inLightClipPos;
layout(location = 5) in vec4 inWorldTangent;
layout(location = 6) in vec2 inNormalUv;
layout(location = 7) in vec2 inMetallicRoughnessUv;
layout(location = 8) in vec2 inOcclusionUv;
layout(location = 9) in vec2 inEmissiveUv;
layout(location = 0) out vec4 outFragColor;

const float PI = 3.14159265359;
const vec2 INV_ATAN = vec2(0.15915494309, 0.31830988618);

float distributionGGX(vec3 N, vec3 H, float roughness) {
  float alpha = roughness * roughness;
  float alphaSquared = alpha * alpha;
  float nDotH = max(dot(N, H), 0.0);
  float denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
  return alphaSquared / max(PI * denominator * denominator, 0.000001);
}

float geometrySchlickGGX(float nDotDirection, float roughness) {
  float remapped = roughness + 1.0;
  float k = remapped * remapped / 8.0;
  return nDotDirection /
         max(nDotDirection * (1.0 - k) + k, 0.000001);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
  return geometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
         geometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
  return F0 + (1.0 - F0) *
                  pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
  return F0 + (max(vec3(1.0 - roughness), F0) - F0) *
                  pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 rotateEnvironmentDirection(vec3 direction) {
  float rotation = ubo.environmentParams.y;
  float cosine = cos(rotation);
  float sine = sin(rotation);
  direction.xz = mat2(cosine, -sine, sine, cosine) * direction.xz;
  return direction;
}

vec2 directionToEquirectangularUv(vec3 direction) {
  vec2 uv = vec2(atan(direction.z, direction.x), asin(direction.y));
  return uv * INV_ATAN + 0.5;
}

vec3 evaluateIrradianceSh(vec3 direction) {
  float basis[9] = float[](
      0.28209479,
      0.48860251 * direction.y,
      0.48860251 * direction.z,
      0.48860251 * direction.x,
      1.09254843 * direction.x * direction.y,
      1.09254843 * direction.y * direction.z,
      0.31539156 * (3.0 * direction.z * direction.z - 1.0),
      1.09254843 * direction.x * direction.z,
      0.54627421 *
          (direction.x * direction.x - direction.y * direction.y));

  vec3 irradiance = ubo.environmentSh[0].rgb * basis[0] * PI;
  for (int coefficient = 1; coefficient <= 3; ++coefficient) {
    irradiance += ubo.environmentSh[coefficient].rgb * basis[coefficient] *
                  (2.0 * PI / 3.0);
  }
  for (int coefficient = 4; coefficient < 9; ++coefficient) {
    irradiance += ubo.environmentSh[coefficient].rgb * basis[coefficient] *
                  (PI / 4.0);
  }
  return max(irradiance, vec3(0.0));
}



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
    vec3 tangentNormal = texture(normalTexture, inNormalUv + (uv - inUv)).xyz * 2.0 - 1.0;
    tangentNormal.xy *= pushConstants.surfaceParams.x;
    N = normalize(tangentToWorld * normalize(tangentNormal));
  } else if (pushConstants.surfaceParams.w > 0.5) {
    N = normalize(tangentToWorld * normalFromHeight(uv));
  }

  vec4 metallicRoughness = texture(metallicRoughnessTexture, inMetallicRoughnessUv + (uv - inUv));
  float metallic = clamp(metallicRoughness.b * material.pbrParams.x, 0.0, 1.0);
  float roughness = clamp(metallicRoughness.g * material.pbrParams.y, 0.04, 1.0);
  float sampledAo = texture(occlusionTexture, inOcclusionUv + (uv - inUv)).r;
  float ao = mix(1.0, sampledAo, clamp(material.pbrParams.z, 0.0, 1.0));
  vec3 emissive = texture(emissiveTexture, inEmissiveUv + (uv - inUv)).rgb * material.emissiveFactor.rgb;

  vec3 H = safeNormalize(L + V, N);
  float nDotL = max(dot(N, L), 0.0);
  float nDotV = max(dot(N, V), 0.0);
  vec3 F0 = mix(vec3(0.04), albedo, metallic);
  float D = distributionGGX(N, H, roughness);
  float G = geometrySmith(N, V, L, roughness);
  vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
  vec3 specular = D * G * F / max(4.0 * nDotV * nDotL, 0.0001);
  vec3 diffuseWeight = (vec3(1.0) - F) * (1.0 - metallic);
  vec3 diffuse = diffuseWeight * albedo / PI;

  vec3 environmentNormal = rotateEnvironmentDirection(N);
  vec3 irradiance = evaluateIrradianceSh(environmentNormal);
  vec3 environmentFresnel =
      fresnelSchlickRoughness(nDotV, F0, roughness);
  vec3 environmentDiffuseWeight =
      (vec3(1.0) - environmentFresnel) * (1.0 - metallic);
  vec3 diffuseIbl = environmentDiffuseWeight * albedo * irradiance / PI;

  vec3 reflectionDirection = reflect(-V, N);
  vec3 environmentReflection =
      rotateEnvironmentDirection(reflectionDirection);
  vec3 reflectedRadiance =
      texture(environmentTexture,
              directionToEquirectangularUv(environmentReflection)).rgb;
  // Temporary roughness approximation until GGX prefiltering and the split-sum
  // BRDF LUT replace this first environment-lighting slice.
  vec3 lowFrequencyRadiance = evaluateIrradianceSh(environmentReflection) / PI;
  vec3 prefilteredRadiance =
      mix(reflectedRadiance, lowFrequencyRadiance, roughness * roughness);
  vec3 specularIbl = prefilteredRadiance * environmentFresnel;

  diffuseIbl *= ubo.environmentParams.x * ubo.environmentParams.z * ao;
  specularIbl *= ubo.environmentParams.x * ubo.environmentParams.w * ao;

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

  int pbrDebugMode = int(round(ubo.lightingParams.w));
  if (pbrDebugMode == 1) {
    outFragColor = vec4(albedo, alpha);
    return;
  }
  if (pbrDebugMode == 2) {
    outFragColor = vec4(vec3(metallic), alpha);
    return;
  }
  if (pbrDebugMode == 3) {
    outFragColor = vec4(vec3(roughness), alpha);
    return;
  }
  if (pbrDebugMode == 4) {
    outFragColor = vec4(N * 0.5 + 0.5, alpha);
    return;
  }
  if (pbrDebugMode == 5) {
    outFragColor = vec4(vec3(ao), alpha);
    return;
  }
  if (pbrDebugMode == 6) {
    outFragColor = vec4(emissive, alpha);
    return;
  }
  if (pbrDebugMode == 7) {
    outFragColor = vec4(diffuseIbl, alpha);
    return;
  }
  if (pbrDebugMode == 8) {
    outFragColor = vec4(specularIbl, alpha);
    return;
  }

  vec3 direct = ubo.lightColor.rgb * nDotL *
                (diffuse * ubo.lightingParams.x +
                 specular * ubo.lightingParams.y);
  vec3 lit = diffuseIbl + specularIbl + visibility * direct + emissive;

  outFragColor = vec4(lit, alpha);
}
