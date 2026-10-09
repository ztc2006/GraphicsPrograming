#ifndef GTAO_COMMON_GLSL
#define GTAO_COMMON_GLSL
layout(set=0,binding=0) uniform sampler2D sceneDepth;
layout(set=0,binding=1) uniform sampler2D rawAo;
layout(set=0,binding=2) uniform sampler2D filteredAo;
layout(set=0,binding=3) uniform sampler2D sceneHdr;
layout(set=0,binding=4) uniform sampler2D indirectDiffuse;
layout(push_constant) uniform GtaoPush {
  mat4 inverseRaster;
  vec4 camera;
  vec4 settings;
  vec4 screen;
} ao;
bool inside(ivec2 p) { return all(greaterThanEqual(p,ivec2(0))) && all(lessThan(p,ivec2(ao.screen.xy))); }
float depthAt(ivec2 p) { return inside(p) ? texelFetch(sceneDepth,p,0).r : 1.0; }
vec3 positionAt(ivec2 p,float d) {
  vec2 uv=(vec2(p)+.5)/ao.screen.xy;
  vec4 w=ao.inverseRaster*vec4(uv*2.0-1.0,d,1.0);
  return w.xyz/w.w;
}
// Choose the shorter one-sided derivative at discontinuities. Missing sky/edge
// evidence cannot become a clamped repeated occluder.
vec3 normalAt(ivec2 p,float centerDepth,vec3 center,vec3 view,out bool valid) {
  vec3 derivatives[4];
  ivec2 offsets[4]=ivec2[4](ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
  vec4 depths;
  for(int i=0;i<4;++i) depths[i]=depthAt(p+offsets[i]);
  vec4 differences=depths-vec4(centerDepth);
  // Hardware NDC depth is affine over a plane. A pair of opposing slopes
  // cancels, while the two background samples around a 1px foreground do not.
  vec2 residual=abs(vec2(differences.x+differences.y,differences.z+differences.w))*.5;
  float tolerance=max(2e-6,(1.0-centerDepth)*.02);
  for(int i=0;i<4;++i){
    ivec2 q=p+offsets[i]; float d=depths[i];
    bool continuous=min(abs(differences[i]),residual[i/2])<=tolerance;
    derivatives[i]=d<1.0 && continuous ? (positionAt(q,d)-center)*(i%2==0?-1.0:1.0) : vec3(1e10);
  }
  vec3 dx=dot(derivatives[0],derivatives[0])<dot(derivatives[1],derivatives[1])?derivatives[0]:derivatives[1];
  vec3 dy=dot(derivatives[2],derivatives[2])<dot(derivatives[3],derivatives[3])?derivatives[2]:derivatives[3];
  vec3 n=cross(dx,dy);
  valid=dot(n,n)>=1e-18 && max(dot(dx,dx),dot(dy,dy))<=1e18;
  if(!valid) return view;
  n=normalize(n); return dot(n,view)<0.0?-n:n;
}
#endif
