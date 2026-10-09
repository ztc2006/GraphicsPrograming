#version 450
#extension GL_GOOGLE_include_directive : require
#include "gtao_common.glsl"
layout(location=0) out vec4 color;
void main(){
  ivec2 p=ivec2(gl_FragCoord.xy);
  float visibility=texelFetch(filteredAo,p,0).r;
  if(ao.settings.z>.5){
    if(ao.settings.z<1.5)visibility=texelFetch(rawAo,p,0).r;
    color=vec4(vec3(visibility),1.0);return;
  }
  vec4 hdr=texelFetch(sceneHdr,p,0);
  vec3 diffuse=texelFetch(indirectDiffuse,p,0).rgb;
  float occlusion=clamp((1.0-visibility)*ao.settings.y,0.0,1.0);
  color=vec4(max(hdr.rgb-diffuse*occlusion,vec3(0)),hdr.a);
}
