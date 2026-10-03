#version 450

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
} ubo;

layout(set = 0, binding = 3) uniform sampler2D environmentTexture;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outFragColor;

const vec2 INV_ATAN = vec2(0.15915494309, 0.31830988618);

vec2 directionToEquirectangularUv(vec3 direction) {
  vec2 uv = vec2(atan(direction.z, direction.x), asin(direction.y));
  return uv * INV_ATAN + 0.5;
}



void main() {
  vec2 ndc = inUv * 2.0 - 1.0;
  vec4 nearPoint = ubo.inverseViewProj * vec4(ndc, 0.0, 1.0);
  nearPoint /= nearPoint.w;
  vec3 direction = normalize(nearPoint.xyz - ubo.cameraPosition.xyz);

  float rotation = ubo.environmentParams.y;
  float cosine = cos(rotation);
  float sine = sin(rotation);
  direction.xz = mat2(cosine, -sine, sine, cosine) * direction.xz;

  vec3 hdr = texture(environmentTexture,
                     directionToEquirectangularUv(direction)).rgb;
  outFragColor = vec4(hdr * ubo.environmentParams.x, 1.0);
}
