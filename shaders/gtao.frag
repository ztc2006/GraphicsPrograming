#version 450
#extension GL_GOOGLE_include_directive : require
#include "gtao_common.glsl"
layout(location=0) out float visibility;
const float PI=3.14159265359;
void main(){
  ivec2 p=ivec2(gl_FragCoord.xy); float depth=depthAt(p);
  visibility=1.0;
  if(depth>=1.0) return;
  vec3 center=positionAt(p,depth), V=normalize(ao.camera.xyz-center);
  bool normalValid;
  vec3 N=normalAt(p,depth,center,V,normalValid);
  if(!normalValid) return; // Insufficient surface evidence: leave visibility white.
  float noise=fract(52.9829189*fract(dot(vec2(p),vec2(.06711056,.00583715))));
  float result=0.0, unoccluded=0.0;
  for(int slice=0;slice<3;++slice){
    float angle=(float(slice)+noise)*PI/3.0;
    vec2 omega=vec2(cos(angle),sin(angle));
    vec4 w=ao.inverseRaster*vec4(((vec2(p)+.5+omega)/ao.screen.xy)*2.0-1.0,depth,1.0);
    vec3 pixelDirection=w.xyz/w.w-center;
    float radiusPx=min(ao.settings.w,ao.settings.x/max(length(pixelDirection),1e-8));
    vec3 D=normalize(pixelDirection-dot(pixelDirection,V)*V);
    vec3 axis=normalize(cross(D,V));
    vec3 projected=N-axis*dot(N,axis);
    float normalLength=length(projected);
    float cosN=clamp(dot(projected,V)/max(normalLength,1e-6),0.0,1.0);
    float n=sign(dot(D,projected))*acos(cosN);
    vec2 low=cos(vec2(n+PI*.5,n-PI*.5)), horizon=low;
    for(int step=0;step<6;++step){
      float t=(float(step)+.5)/6.0;
      float distancePx=max(1.0,t*t*radiusPx);
      ivec2 delta=ivec2(round(omega*distancePx));
      for(int side=0;side<2;++side){
        ivec2 q=p+(side==0?delta:-delta); float sd=depthAt(q);
        if(sd>=1.0||q==p) continue;
        vec3 sampleDelta=positionAt(q,sd)-center;
        float distance=length(sampleDelta);
        if(distance<=1e-7||dot(N,sampleDelta)<=ao.settings.x*.002) continue;
        float weight=1.0-smoothstep(ao.settings.x*.6,ao.settings.x,distance);
        float candidate=mix(low[side],dot(sampleDelta/distance,V),weight);
        horizon[side]=max(horizon[side],candidate);
      }
    }
    // GTAO's two analytic cosine-weighted horizon arcs (Jimenez et al.).
    float h0=-acos(clamp(horizon.y,-1.0,1.0)), h1=acos(clamp(horizon.x,-1.0,1.0));
    h0=n+clamp(h0-n,-PI*.5,PI*.5); h1=n+clamp(h1-n,-PI*.5,PI*.5);
    float sinN=sin(n);
    float arcs=(2.0*cosN+2.0*(h0+h1)*sinN-cos(2.0*h0-n)-cos(2.0*h1-n))*.25;
    float weight=mix(normalLength,1.0,.05);
    result+=weight*arcs;
    // The same cosine integral with open horizons n +/- PI/2. Normalize
    // finite azimuth quadrature so an unobstructed tilted plane stays white.
    unoccluded+=weight*(cosN+n*sinN);
  }
  visibility=clamp(result/max(unoccluded,1e-6),0.0,1.0);
}
