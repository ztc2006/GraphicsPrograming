#ifndef PBR_SHARED_GLSL
#define PBR_SHARED_GLSL
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

vec3 fresnelSchlick(float cosTheta, vec3 F0, float F90) {
  return F0 + (F90 - F0) *
                  pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
  return F0 + (max(vec3(1.0 - roughness), F0) - F0) *
                  pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}


#endif
