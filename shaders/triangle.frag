#version 450
#extension GL_GOOGLE_include_directive : require
#include "cluster_grid.glsl"
#include "indoor_lighting.glsl"
#include "temporal_motion.glsl"

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
  vec4 specularColorAndWeight;
  vec4 optical;
  vec4 absorptionThickness;
} material;
layout(set = 1, binding = 8) uniform sampler2D baseAlbedoTexture;
layout(set = 1, binding = 9) uniform sampler2D baseAlphaMaskTexture;
layout(set = 1, binding = 10) uniform sampler2D specularTexture;
layout(set = 1, binding = 11) uniform sampler2D specularColorTexture;

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
  vec4 surfaceParams;
  vec4 alphaParams;
  ivec4 passData;
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
  mat4 currentViewProj;
  mat4 previousViewProj;
  vec4 previousCamera;
  vec4 jitterUv;
}
ubo;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 0, binding = 2) uniform sampler2D shadowDebugMap;
layout(set = 0, binding = 4) uniform samplerCubeArray environmentPrefilter;
layout(set = 0, binding = 5) uniform sampler2D environmentBrdfLut;

struct PunctualLight {
  vec4 positionRange;
  vec4 directionType;
  vec4 colorIntensity;
  vec4 cones;
};
layout(std430, set = 0, binding = 6) readonly buffer PunctualLights {
  uvec4 counts;
  PunctualLight lights[];
} punctual;

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec3 inWorldPos;
layout(location = 3) in vec3 inWorldNormal;
layout(location = 4) in vec4 inLightClipPos;
layout(location = 5) in vec4 inWorldTangent;
layout(location = 6) in vec2 inNormalUv;
layout(location = 7) in vec2 inMetallicRoughnessUv;
layout(location = 8) in vec2 inOcclusionUv;
layout(location = 9) in vec2 inEmissiveUv;
layout(location = 10) in vec2 inSpecularUv;
layout(location = 11) in vec2 inSpecularColorUv;
layout(location=12) in vec4 inCurrentClip;
layout(location=13) in vec4 inPreviousClip;
layout(location=14) flat in float inMotionValid;
layout(location=1) out vec4 outMotion;
layout(location=2) out vec4 outIndirectDiffuse;
layout(location = 0) out vec4 outFragColor;

const float PI = 3.14159265359;

#include "pbr_shared.glsl"
#include "dielectric_shared.glsl"

vec3 rotateEnvironmentDirection(vec3 direction) {
  float rotation = ubo.environmentParams.y;
  float cosine = cos(rotation);
  float sine = sin(rotation);
  direction.xz = mat2(cosine, -sine, sine, cosine) * direction.xz;
  return direction;
}

vec3 evaluateIrradianceSh(vec3 direction, bool local) {
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

  vec3 irradiance = (local ? indoor.probeSh[0].rgb : ubo.environmentSh[0].rgb) * basis[0] * PI;
  for (int coefficient = 1; coefficient <= 3; ++coefficient) {
    irradiance += (local ? indoor.probeSh[coefficient].rgb : ubo.environmentSh[coefficient].rgb) * basis[coefficient] *
                  (2.0 * PI / 3.0);
  }
  for (int coefficient = 4; coefficient < 9; ++coefficient) {
    irradiance += (local ? indoor.probeSh[coefficient].rgb : ubo.environmentSh[coefficient].rgb) * basis[coefficient] *
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

vec3 fallbackTangent(vec3 N) {
  vec3 axis = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  return normalize(cross(axis, N));
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

vec3 receiverDx, receiverDy;

float atlasShadowVisibility(vec4 lightClipPos, vec3 N, vec3 L, vec4 rect, vec4 params, vec2 planeGradient) {
  if (lightClipPos.w <= 0.0) return 1.0;
  vec3 proj = lightClipPos.xyz / lightClipPos.w;
  vec2 uv = proj.xy * 0.5 + 0.5;

  if (proj.z < 0.0 || proj.z > 1.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 ||
      uv.y > 1.0) {
    return 1.0;
  }

  float bias =
      max(params.x * (1.0 - max(dot(N, L), 0.0)), params.y);
  float radius = clamp(params.z, 0.0, 4.0);
  int sampleRadius = int(floor(radius));
  float blend = fract(radius);
  vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
  // Residual footprint of hardware bilinear comparisons is half a texel.
  bias += dot(abs(planeGradient), texel * .5);

  uv = rect.xy + uv * rect.zw;
  vec2 lo = rect.xy + texel * .5, hi = rect.xy + rect.zw - texel * .5;
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
      vec2 tap = clamp(uv + offset * texel, lo, hi);
      float compareDepth = proj.z + dot(tap - uv, planeGradient) - bias;
      visible += weight * texture(shadowMap, vec3(tap, compareDepth));
      sampleCount += weight;
    }
  }
  return visible / max(sampleCount, 1.0);
}

// Legacy and perspective spot paths retain their existing bias contract.
float atlasShadowVisibility(vec4 clip, vec3 N, vec3 L, vec4 rect, vec4 params) {
  return atlasShadowVisibility(clip, N, L, rect, params, vec2(0));
}
float sunCascadeVisibility(int index, vec3 N, vec3 L) {
  mat4 matrix = indoor.sunViewProj[index];
  vec4 rect = indoor.sunRects[index];
  vec3 dx = mat3(matrix) * receiverDx, dy = mat3(matrix) * receiverDy;
  vec2 ux = dx.xy * .5 * rect.zw, uy = dy.xy * .5 * rect.zw;
  float determinant = ux.x * uy.y - ux.y * uy.x;
  vec2 gradient = vec2(0);
  if (abs(determinant) > 1e-20)
    gradient = vec2(dx.z * uy.y - dy.z * ux.y,
                    dy.z * ux.x - dx.z * uy.x) / determinant;
  return atlasShadowVisibility(matrix * vec4(inWorldPos, 1), N, L, rect,
                               indoor.sunBias[index], gradient);
}
float sunViewDepth() {
  return dot(inWorldPos - ubo.cameraPosition.xyz, indoor.sunForwardNear.xyz);
}
int sunCascadeIndex(float depth) {
  int count = int(indoor.sunParams.x);
  int index = 0;
  while (index < count && depth > indoor.sunSplits[index]) ++index;
  return index;
}
float sunBlendWeight(int index, float depth) {
  float start = index == 0 ? indoor.sunForwardNear.w : indoor.sunSplits[index - 1];
  float end = indoor.sunSplits[index];
  float band = (end - start) * indoor.sunParams.y;
  return band > 0.0 ? clamp((depth - (end - band)) / band, 0.0, 1.0) : 0.0;
}
float sunVisibility(vec3 N, vec3 L) {
  int count = int(indoor.sunParams.x);
  if (count == 0)
    return atlasShadowVisibility(inLightClipPos, N, L, indoor.sunRect, ubo.shadowParams);
  float depth = sunViewDepth();
  int index = sunCascadeIndex(depth);
  if (index == count || depth < indoor.sunForwardNear.w) return 1.0;
  float visible = sunCascadeVisibility(index, N, L);
  float weight = sunBlendWeight(index, depth);
  if (weight > 0.0) {
    float next = index + 1 == count ? 1.0 :
        sunCascadeVisibility(index + 1, N, L);
    visible = mix(visible, next, weight);
  }
  return visible;
}
vec3 sunCascadeColor(int index) {
  const vec3 colors[4] = vec3[4](vec3(1,.15,.15), vec3(.15,1,.15),
                               vec3(.15,.35,1), vec3(1,.8,.15));
  return index < int(indoor.sunParams.x) ? colors[index] : vec3(.15);
}

// Return reflected radiance per unit incident irradiance, including NoL.
vec3 directBrdf(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic,
                float roughness, vec3 dielectricF0, float weight, vec3 F0, float F90) {
  vec3 H = safeNormalize(L + V, N);
  float nl = max(dot(N, L), 0.0), nv = max(dot(N, V), 0.0);
  float vh = max(dot(H, V), 0.0);
  vec3 F = fresnelSchlick(vh, F0, F90);
  vec3 dielectricF = fresnelSchlick(vh, dielectricF0 * weight, weight);
  vec3 specular = distributionGGX(N, H, roughness) * geometrySmith(N, V, L, roughness) * F /
                  max(4.0 * nv * nl, 0.0001);
  float diffuseWeight = (1.0 - max(max(dielectricF.r, dielectricF.g), dielectricF.b)) * (1.0 - metallic);
  return nl * (diffuseWeight * albedo / PI * ubo.lightingParams.x + specular * ubo.lightingParams.y);
}

vec3 punctualDirect(vec3 N, vec3 V, vec3 albedo, float metallic,
                     float roughness, vec3 dielectricF0, float weight, vec3 F0, float F90) {
  vec3 result = vec3(0);
  uint cell = fragmentCluster(inWorldPos, gl_FragCoord.xy);
  uint base = cell == 0xffffffffu ? 0u : cell * (clusters.screen.w + 1u);
  uint count = cell == 0xffffffffu ? 0xffffffffu : clusterIndices.words[base];
  bool full = count == 0xffffffffu;
  uint total = full ? punctual.counts.x : count;
  for (uint entry = 0u; entry < total; ++entry) {
    uint i = full ? entry : clusterIndices.words[base + 1u + entry];
    PunctualLight light = punctual.lights[i];
    int type = int(light.directionType.w);
    vec3 L = -light.directionType.xyz;
    float attenuation = 1.0;
    if (type != 0) {
      vec3 delta = light.positionRange.xyz - inWorldPos;
      float distanceSquared = dot(delta, delta);
      // The punctual emitter singularity has no defined surface direction.
      if (distanceSquared < 1e-8) continue;
      float distance = sqrt(distanceSquared);
      L = delta / distance;
      float range = light.positionRange.w;
      float fade = range > 0.0 ? clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0) : 1.0;
      attenuation = fade / max(distanceSquared, 1e-8);
      if (type == 2) {
        float cosine = dot(light.directionType.xyz, -L);
        float inner = light.cones.x, outer = light.cones.y;
        // Very narrow legal cones can collapse to equal cosines in FP32.
        float ramp = inner <= outer ? (cosine >= inner ? 1.0 : 0.0) :
                     clamp((cosine - outer) / (inner - outer), 0.0, 1.0);
        attenuation *= ramp * ramp;
      }
    }
    int shadowIndex = int(light.cones.z);
    if (shadowIndex >= 0 && uint(shadowIndex) < indoor.counts.x)
      attenuation *= atlasShadowVisibility(indoor.spotViewProj[shadowIndex] * vec4(inWorldPos, 1),
          N, L, indoor.spotRects[shadowIndex], indoor.spotBias[shadowIndex]);
    result += light.colorIntensity.rgb * light.colorIntensity.w * attenuation *
              directBrdf(N, V, L, albedo, metallic, roughness, dielectricF0, weight, F0, F90);
  }
  return result;
}

void main() {
  outIndirectDiffuse=vec4(0);
  outMotion = temporalMotion(inCurrentClip, inPreviousClip, inMotionValid);
  // Compute derivatives before discard and divergent cascade selection.
  receiverDx = dFdx(inWorldPos); receiverDy = dFdy(inWorldPos);
  vec3 N = safeNormalize(inWorldNormal, vec3(0.0, 0.0, 1.0));
  vec3 T = safeNormalize(inWorldTangent.xyz - N * dot(N, inWorldTangent.xyz),
                         fallbackTangent(N));
  vec3 B = cross(N, T) * inWorldTangent.w;
  // A reflected model reverses winding, but retains its authored front side.
  bool mirrored = determinant(mat3(pushConstants.transform)) < 0.0;
  if (gl_FrontFacing == mirrored) {
    T = -T;
    B = -B;
    N = -N;
  }
  mat3 tangentToWorld = mat3(T, B, N);
  // Take derivatives before alpha discard and divergent POM traversal.
  // Geometry only: texture-mip variance is a separate contribution.
  vec3 normalDx = dFdx(N), normalDy = dFdy(N);
  float geometryKernel = min(0.3 * (dot(normalDx, normalDx) + dot(normalDy, normalDy)), 0.2);
  vec2 normalUvDx = dFdx(inNormalUv), normalUvDy = dFdy(inNormalUv);

  vec3 L = normalize(ubo.lightDirection.xyz);
  vec3 V = normalize(ubo.cameraPosition.xyz - inWorldPos);
  vec3 tangentViewDirection = normalize(transpose(tangentToWorld) * V);
  vec2 uv = parallaxOcclusionUv(inUv, tangentViewDirection);

  int alphaMode = int(round(pushConstants.alphaParams.x));
  bool glass=(uint(material.optical.w)&1u)!=0u;
  int coverageMode=glass ? int((uint(material.optical.w)>>1)&3u) : alphaMode;
  bool useBase = pushConstants.alphaParams.z > 1.5 ||
                 (alphaMode == 1 && (pushConstants.alphaParams.z != 1.0 ||
                                     pushConstants.alphaParams.w < 0.5));
  vec4 texel = useBase ? texture(baseAlbedoTexture, uv) : texture(albedoTexture, uv);
  vec4 alphaTexel = useBase ? texture(baseAlphaMaskTexture, uv) : texture(alphaMaskTexture, uv);
  float alpha =
      texel.a * alphaTexel.r * pushConstants.materialTint.a * inColor.a;
  if (coverageMode == 1 && alpha < pushConstants.alphaParams.y) {
    discard;
  }
  if (coverageMode == 0) {
    alpha = 1.0;
  }
  outIndirectDiffuse.a=alpha;
  if (alphaMode == 2) outMotion = vec4(0,0,0,alpha);

  vec3 albedo = texel.rgb * inColor.rgb * pushConstants.materialTint.rgb;
  float textureKernel = 0.0;
  if (pushConstants.surfaceParams.z > 0.5) {
    vec4 normalSample = textureGrad(normalTexture, inNormalUv + (uv - inUv), normalUvDx, normalUvDy);
    float lengthLoss = clamp(normalSample.a, 0.0, 1.0);
    float variance = lengthLoss / max(1.0 - lengthLoss, 1.0 / 255.0);
    float scale = pushConstants.surfaceParams.x;
    textureKernel = min(2.0 * variance * scale * scale, 1.0);
    vec3 tangentNormal = normalSample.xyz * 2.0 - 1.0;
    tangentNormal.xy *= pushConstants.surfaceParams.x;
    N = safeNormalize(tangentToWorld * safeNormalize(tangentNormal, vec3(0.0, 0.0, 1.0)), N);
  } else if (pushConstants.surfaceParams.w > 0.5) {
    N = normalize(tangentToWorld * normalFromHeight(uv));
  }

  vec4 metallicRoughness = texture(metallicRoughnessTexture, inMetallicRoughnessUv + (uv - inUv));
  float metallic = clamp(metallicRoughness.b * material.pbrParams.x, 0.0, 1.0);
  float authoredRoughness = clamp(metallicRoughness.g * material.pbrParams.y, 0.04, 1.0);
  float alphaSquared = authoredRoughness * authoredRoughness;
  alphaSquared *= alphaSquared; // GGX alpha = perceptual roughness squared.
  bool specularAa = ubo.lightingParams.z > 0.5;
  float roughness = specularAa
      ? sqrt(sqrt(min(1.0, alphaSquared + textureKernel + geometryKernel)))
      : authoredRoughness;
  float sampledAo = texture(occlusionTexture, inOcclusionUv + (uv - inUv)).r;
  float ao = mix(1.0, sampledAo, clamp(material.pbrParams.z, 0.0, 1.0));
  vec3 emissive = texture(emissiveTexture, inEmissiveUv + (uv - inUv)).rgb * material.emissiveFactor.rgb;

  float nDotV = max(dot(N, V), 0.0);
  float specularWeight = clamp(material.specularColorAndWeight.a *
      texture(specularTexture, inSpecularUv + (uv - inUv)).a, 0.0, 1.0);
  vec3 specularColor = material.specularColorAndWeight.rgb *
      texture(specularColorTexture, inSpecularColorUv + (uv - inUv)).rgb;
  // Clamp the reflectance product before weighting; author color may exceed 1.
  float ior=material.optical.x>0 ? material.optical.x : 1.5;
  float baseF0=pow((ior-1)/(ior+1),2);
  vec3 dielectricF0 = min(vec3(baseF0) * specularColor, vec3(1.0));
  vec3 F0 = mix(dielectricF0 * specularWeight, albedo, metallic);
  float F90 = mix(specularWeight, 1.0, metallic);

  bool local = (indoor.counts.w & 1u) != 0u &&
      all(greaterThanEqual(inWorldPos, indoor.probeMin.xyz)) &&
      all(lessThanEqual(inWorldPos, indoor.probeMax.xyz));
  vec3 environmentNormal = local ? N : rotateEnvironmentDirection(N);
  vec3 irradiance = evaluateIrradianceSh(environmentNormal, local);
  vec3 environmentFresnel =
      specularWeight * fresnelSchlickRoughness(nDotV, dielectricF0, roughness);
  float environmentDiffuseWeight =
      (1.0 - max(max(environmentFresnel.r, environmentFresnel.g), environmentFresnel.b)) *
      (1.0 - metallic);
  vec3 diffuseIbl = environmentDiffuseWeight * albedo * irradiance / PI;

  vec3 reflectionDirection = reflect(-V, N);
  vec3 environmentReflection=rotateEnvironmentDirection(reflectionDirection);
  bool detail=(indoor.counts.w & 4u)!=0u &&
      all(greaterThanEqual(inWorldPos,indoor.detailMin.xyz)) && all(lessThanEqual(inWorldPos,indoor.detailMax.xyz));
  float reflectionLayer=detail?2.0:local?1.0:0.0;
  if(local || detail){
    vec3 lo=detail?indoor.detailMin.xyz:indoor.probeMin.xyz;
    vec3 hi=detail?indoor.detailMax.xyz:indoor.probeMax.xyz;
    vec3 origin=detail?indoor.detailPosition.xyz:indoor.probePosition.xyz;
    vec3 boundary=mix(lo,hi,greaterThan(reflectionDirection,vec3(0)));
    vec3 t=vec3(1e20);for(int axis=0;axis<3;++axis)if(abs(reflectionDirection[axis])>1e-6)t[axis]=(boundary[axis]-inWorldPos[axis])/reflectionDirection[axis];
    float distance=max(min(t.x,min(t.y,t.z)),0.0);
    environmentReflection=inWorldPos+reflectionDirection*distance-origin;
  }
  float environmentLod=roughness*float(textureQueryLevels(environmentPrefilter)-1);
  vec3 prefilteredRadiance=textureLod(environmentPrefilter,vec4(environmentReflection,reflectionLayer),environmentLod).rgb;
  vec2 environmentBrdf = textureLod(environmentBrdfLut, vec2(nDotV, roughness), 0.0).rg;
  vec3 specularIbl = prefilteredRadiance * (F0 * environmentBrdf.x + F90 * environmentBrdf.y);

  diffuseIbl *= ubo.environmentParams.x * ubo.environmentParams.z * ao;
  specularIbl *= ubo.environmentParams.x * ubo.environmentParams.w * ao;

  if ((indoor.counts.w & 2u) != 0u) {
    diffuseIbl = vec3(0);
    specularIbl = vec3(0);
  }
  int shadowMode = int(round(ubo.shadowParams.w));
  float visibility =
      shadowMode == 0 ? 1.0 : sunVisibility(N, L);

  if (shadowMode == 2) {
    outFragColor = vec4(vec3(visibility), alpha);
    return;
  }

  if (shadowMode == 3) {
    int index = min(int(indoor.sunParams.z), max(int(indoor.sunParams.x) - 1, 0));
    vec4 clip = indoor.sunParams.x > 0 ? indoor.sunViewProj[index] * vec4(inWorldPos, 1) : inLightClipPos;
    vec4 rect = indoor.sunParams.x > 0 ? indoor.sunRects[index] : indoor.sunRect;
    vec3 proj = clip.xyz / clip.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (proj.z < 0.0 || proj.z > 1.0 || uv.x < 0.0 || uv.x > 1.0 ||
        uv.y < 0.0 || uv.y > 1.0) {
      outFragColor = vec4(0.05, 0.05, 0.05, 1.0);
    } else {
      float depth = texture(shadowDebugMap, rect.xy + uv * rect.zw).r;
      outFragColor = vec4(vec3(depth), 1.0);
    }
    return;
  }

  int pbrDebugMode = int(round(ubo.lightingParams.w));
  if (pbrDebugMode == 18 || pbrDebugMode == 19) {
    outFragColor = vec4(temporalMotionDebug(outMotion, pbrDebugMode), alpha);
    return;
  }
  if (pbrDebugMode == 17) {
    float depth = sunViewDepth();
    int index = sunCascadeIndex(depth);
    vec3 color = indoor.sunParams.x == 0 ? vec3(.5) : sunCascadeColor(index);
    if (index < int(indoor.sunParams.x))
      color = mix(color, sunCascadeColor(index + 1), sunBlendWeight(index, depth));
    outFragColor = vec4(color, alpha);
    return;
  }
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

  if (pbrDebugMode == 9) {
    outFragColor = vec4(vec3(specularWeight), alpha);
    return;
  }
  if (pbrDebugMode == 10) {
    outFragColor = vec4(dielectricF0 * specularWeight, alpha);
    return;
  }

  if (pbrDebugMode == 11) {
    outFragColor = vec4(vec3(authoredRoughness), alpha);
    return;
  }
  if (pbrDebugMode == 12 || pbrDebugMode == 13) {
    outFragColor = vec4(vec3(pbrDebugMode == 12 ? textureKernel : geometryKernel), alpha);
    return;
  }

  vec3 direct = visibility * ubo.lightColor.rgb *
      directBrdf(N, V, L, albedo, metallic, roughness, dielectricF0, specularWeight, F0, F90) +
      punctualDirect(N, V, albedo, metallic, roughness, dielectricF0, specularWeight, F0, F90);
  if (pbrDebugMode == 16) {
    uint index = indoor.counts.z;
    vec3 debugL = vec3(0,1,0);
    for (uint i=0u;i<punctual.counts.x;++i)
      if (int(punctual.lights[i].cones.z)==int(index))
        debugL = safeNormalize(punctual.lights[i].positionRange.xyz-inWorldPos, debugL);
    float value = index < indoor.counts.x ? atlasShadowVisibility(
        indoor.spotViewProj[index] * vec4(inWorldPos, 1), N,
        debugL, indoor.spotRects[index], indoor.spotBias[index]) : -1.0;
    outFragColor = value < 0 ? vec4(1,0,1,alpha) : vec4(vec3(value),alpha);
    return;
  }
  if (pbrDebugMode == 15) {
    uint cell = fragmentCluster(inWorldPos, gl_FragCoord.xy);
    uint count = cell == 0xffffffffu ? 0xffffffffu : clusterIndices.words[cell * (clusters.screen.w + 1u)];
    outFragColor = count == 0xffffffffu ? vec4(1, 0, 1, alpha) : vec4(vec3(float(count) / 64.0), alpha);
    return;
  }
  if (pbrDebugMode==0 && alphaMode!=2) outIndirectDiffuse.rgb=diffuseIbl;
  vec3 lit = pbrDebugMode == 14 ? direct : diffuseIbl + specularIbl + direct + emissive;

  if(glass && pbrDebugMode==0) {
    float cosine=max(dot(N,V),0);
    bool solid=pushConstants.passData.w!=0;
    float F=solid ? dielectricFresnel(cosine,1,ior) : thinDielectricReflection(cosine,ior);
    float thickness=max(intBitsToFloat(pushConstants.passData.z),0);
    vec3 direction=solid ? refract(-V,N,1/ior) : -V;
    direction=safeNormalize(direction,-V);
    vec3 sampleDirection=rotateEnvironmentDirection(direction);
    float layer=local ? 1.0 : 0.0;
    if(local) {
      vec3 boundary=mix(indoor.probeMin.xyz,indoor.probeMax.xyz,greaterThan(direction,vec3(0)));
      vec3 t=vec3(1e20);for(int axis=0;axis<3;++axis)if(abs(direction[axis])>1e-6)t[axis]=(boundary[axis]-inWorldPos[axis])/direction[axis];
      sampleDirection=inWorldPos+direction*max(min(t.x,min(t.y,t.z)),0)-indoor.probePosition.xyz;
    }
    vec3 transmitted=textureLod(environmentPrefilter,vec4(sampleDirection,layer),0).rgb*ubo.environmentParams.x;
    float eta=1/ior,cosT=sqrt(max(0,1-eta*eta*(1-cosine*cosine)));
    vec3 attenuation=dielectricAttenuation(material.absorptionThickness.rgb,thickness/max(cosT,1e-4));
    vec3 reflected=prefilteredRadiance*ubo.environmentParams.x*F;
    if((indoor.counts.w&2u)!=0u) {
      // Capture is direct-only: alpha proxy excludes recursive probe reads.
      float opacity=clamp(F+(1-F)*(1-material.optical.y),0,1);
      outFragColor=vec4(opacity>1e-6 ? (direct*(1-material.optical.y)+emissive)/opacity : vec3(0),opacity*alpha);
      return;
    }
    lit=reflected+(1-F)*material.optical.y*transmitted*attenuation*albedo+
        (1-material.optical.y)*(diffuseIbl+direct)+emissive;
  }
  outFragColor = vec4(lit, alpha);
}
