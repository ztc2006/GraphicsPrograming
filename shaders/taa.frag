#version 450
#extension GL_GOOGLE_include_directive : require
layout(set=0,binding=0) uniform sampler2D currentColor;
layout(set=0,binding=1) uniform sampler2D currentDepth;
layout(set=0,binding=2) uniform sampler2D motionTexture;
layout(set=0,binding=3) uniform sampler2D historyColor;
layout(set=0,binding=4) uniform sampler2D historyDepth;
layout(push_constant) uniform Params {mat4 inverseRaster;vec4 depthRow;vec4 jitterWeight;vec4 options;} p;
layout(location=0) out vec4 resolved;
layout(location=1) out float linearDepth;
#include "taa_history.glsl"
vec3 yc(vec3 c){return vec3(dot(c,vec3(.25,.5,.25)),.5*(c.r-c.b),dot(c,vec3(-.25,.5,-.25)));}
vec3 rgb(vec3 c){return vec3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
void main(){
 ivec2 size=textureSize(currentColor,0),pos=ivec2(gl_FragCoord.xy);
 vec2 uv=gl_FragCoord.xy/vec2(size);vec4 current=texelFetch(currentColor,pos,0);
 float z=texelFetch(currentDepth,pos,0).r;bool sky=z>=1.;
 vec4 world=p.inverseRaster*vec4(uv*2.-1.,z,1.);
 linearDepth=sky?0.:max(dot(p.depthRow,world)/world.w,0.);
 vec4 motion=texelFetch(motionTexture,pos,0);
 bool finiteMotion=!any(isnan(motion))&&!any(isinf(motion));
 bool reusable=finiteMotion && abs(abs(motion.z)-1.)<.001;
 resolved=vec4(current.rgb,reusable?1.:0.);
 // Jitter changes current sampling, never the persistent output/history lattice.
 vec2 previousUv=uv-motion.xy;
 if(any(isnan(motion))||any(isinf(motion))||isnan(linearDepth)||isinf(linearDepth)||p.options.x<.5||motion.z<.999||any(lessThan(previousUv,vec2(.5)/vec2(size)))||any(greaterThan(previousUv,vec2(1.)-vec2(.5)/vec2(size))))return;

 // FP16 previous clip.w has bounded relative error; include footprint slope.
 float slope=0.;
 for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){ivec2 q=clamp(pos+ivec2(x,y),ivec2(0),size-1);float d=texelFetch(currentDepth,q,0).r;
  if(d<1.){vec2 quv=(vec2(q)+.5)/vec2(size);vec4 w=p.inverseRaster*vec4(quv*2.-1.,d,1.);float ld=dot(p.depthRow,w)/w.w;if(abs(ld-linearDepth)<linearDepth*.05)slope=max(slope,abs(ld-linearDepth));}}
 vec3 reconstructed;
 if(!reconstructHistory(previousUv,size,motion.w,max(.001,p.options.y*motion.w+2.*slope),sky,reconstructed))return;
 vec3 lo=vec3(1e30),hi=vec3(-1e30),sum=vec3(0),sum2=vec3(0);
 for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){vec3 c=yc(texelFetch(currentColor,clamp(pos+ivec2(x,y),ivec2(0),size-1),0).rgb);lo=min(lo,c);hi=max(hi,c);sum+=c;sum2+=c*c;}
 vec3 mean=sum/9.,sigma=sqrt(max(sum2/9.-mean*mean,vec3(0)));
 lo=max(lo-.25*sigma,mean-p.options.w*sigma);hi=min(hi+.25*sigma,mean+p.options.w*sigma);
 vec3 rawHistory=yc(reconstructed),old=clamp(rawHistory,lo,hi);
 float weight=mix(p.jitterWeight.z,.4,clamp(length(motion.xy*size)/p.options.z,0.,1.));
 // Contradicted history should settle quickly; do not classify ordinary
 // subpixel variation as lighting change merely from current/history difference.
 float clipped=abs(rawHistory.x-old.x)/max(max(abs(rawHistory.x),abs(old.x)),.01);
 weight=max(weight,mix(p.jitterWeight.z,.8,clamp(clipped*2.,0.,1.)));
 resolved=vec4(max(mix(rgb(old),current.rgb,weight),vec3(0)),reusable?1.:0.);
}
