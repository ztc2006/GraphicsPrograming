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
} ubo;

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
  vec4 surfaceParams;
} pushConstants;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUv;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec3 outColor;
layout(location = 1) out vec2 outUv;
layout(location = 2) out vec3 outWorldPos;
layout(location = 3) out vec3 outWorldNormal;
layout(location = 4) out vec4 outLightClipPos;
layout(location = 5) out vec4 outWorldTangent;

void main()
{
  vec4 worldPos = pushConstants.transform * vec4(inPosition, 1.0);
  mat3 normalMatrix = transpose(inverse(mat3(pushConstants.transform)));

  gl_Position = ubo.viewProj * worldPos;
  outColor = inColor;
  outUv = inUv;
  outWorldPos = worldPos.xyz;
  outWorldNormal = normalize(normalMatrix * inNormal);
  outLightClipPos = ubo.lightViewProj * worldPos;
  outWorldTangent =
      vec4(normalize(mat3(pushConstants.transform) * inTangent.xyz),
           inTangent.w);
}
