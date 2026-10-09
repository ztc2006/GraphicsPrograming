#version 450
#extension GL_GOOGLE_include_directive : require
#include "gtao_common.glsl"
layout(location=0) out float visibility;
void main(){
  ivec2 p=ivec2(gl_FragCoord.xy); float depth=depthAt(p);
  visibility=1.0; if(depth>=1.0) return;
  vec3 center=positionAt(p,depth),V=normalize(ao.camera.xyz-center);
  bool normalValid;
  vec3 N=normalAt(p,depth,center,V,normalValid);
  if(!normalValid){visibility=texelFetch(rawAo,p,0).r;return;}
  float sigma=max(ao.settings.x*.05,length(ao.camera.xyz-center)*.001);
  float total=0.0,weightSum=0.0;
  for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){
    ivec2 q=p+ivec2(x,y);float d=depthAt(q); if(d>=1.0)continue;
    vec3 delta=positionAt(q,d)-center;
    float weight=exp(-abs(dot(delta,N))/sigma)/(1.0+float(x*x+y*y));
    total+=texelFetch(rawAo,q,0).r*weight;weightSum+=weight;
  }
  visibility=clamp(total/max(weightSum,1e-6),0.0,1.0);
}
