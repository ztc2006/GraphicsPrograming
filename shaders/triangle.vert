#version 450

layout(set = 0, binding = 0) uniform FrameUbo {
  mat4 viewProj;
} ubo;

layout(push_constant) uniform PushConstants {
  mat4 transform;
  vec4 materialTint;
} pushConstants;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUv;

layout(location = 0) out vec3 outColor;
layout(location = 1) out vec2 outUv;
layout(location = 2) out vec3 outWorldPos;
layout(location = 3) out vec3 outWorldNormal;

void main()
{
  vec4 worldPos = pushConstants.transform * vec4(inPosition, 1.0f);
  mat3 normalMatrix = transpose(inverse(mat3(pushConstants.transform)));

  gl_Position = ubo.viewProj * worldPos;
  outColor = inColor;
  outUv = inUv;
  outWorldPos = worldPos.xyz;
  outWorldNormal = normalize(normalMatrix * inNormal);
}
