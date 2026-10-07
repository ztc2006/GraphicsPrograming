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
  vec4 environmentSh[9];
  mat4 currentViewProj;
  mat4 previousViewProj;
  vec4 previousCamera;
  vec4 jitterUv;
} ubo;

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
  vec4 surfaceParams;
  vec4 alphaParams;
  ivec4 passData;
} pushConstants;

struct MotionObject { mat4 previousModel; vec4 flags; };
layout(std430, set=0, binding=10) readonly buffer PreviousObjects { MotionObject objects[]; } previous;
layout(location=12) out vec4 outCurrentClip;
layout(location=13) out vec4 outPreviousClip;
layout(location=14) flat out float outMotionValid;
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUv;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in vec2 inNormalUv;
layout(location = 6) in vec2 inMetallicRoughnessUv;
layout(location = 7) in vec2 inOcclusionUv;
layout(location = 8) in vec2 inEmissiveUv;
layout(location = 9) in float inAlpha;
layout(location = 10) in vec2 inSpecularUv;
layout(location = 11) in vec2 inSpecularColorUv;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUv;
layout(location = 2) out vec3 outWorldPos;
layout(location = 3) out vec3 outWorldNormal;
layout(location = 4) out vec4 outLightClipPos;
layout(location = 5) out vec4 outWorldTangent;
layout(location = 6) out vec2 outNormalUv;
layout(location = 7) out vec2 outMetallicRoughnessUv;
layout(location = 8) out vec2 outOcclusionUv;
layout(location = 9) out vec2 outEmissiveUv;
layout(location = 10) out vec2 outSpecularUv;
layout(location = 11) out vec2 outSpecularColorUv;

void main()
{
  vec4 worldPos = pushConstants.transform * vec4(inPosition, 1.0);
  mat3 normalMatrix = transpose(inverse(mat3(pushConstants.transform)));

  gl_Position = ubo.viewProj * worldPos;
  MotionObject object;
  object.previousModel = pushConstants.transform; object.flags = vec4(0);
  if (pushConstants.passData.y > 0) object = previous.objects[pushConstants.passData.y];
  outCurrentClip = ubo.currentViewProj * worldPos;
  outPreviousClip = ubo.previousViewProj * object.previousModel * vec4(inPosition,1);
  outMotionValid = object.flags.x * ubo.previousCamera.w;
  outColor = vec4(inColor, inAlpha);
  outUv = inUv;
  outNormalUv = inNormalUv;
  outMetallicRoughnessUv = inMetallicRoughnessUv;
  outOcclusionUv = inOcclusionUv;
  outEmissiveUv = inEmissiveUv;
  outSpecularUv = inSpecularUv;
  outSpecularColorUv = inSpecularColorUv;
  outWorldPos = worldPos.xyz;
  outWorldNormal = normalize(normalMatrix * inNormal);
  outLightClipPos = ubo.lightViewProj * worldPos;
  outWorldTangent =
      vec4(normalize(mat3(pushConstants.transform) * inTangent.xyz),
           inTangent.w * sign(determinant(mat3(pushConstants.transform))));
}
