// History is a fixed unjittered output lattice. alpha records whether the last
// composite is eligible for reuse, independently of previous motion validity.
vec4 cubicWeights(float t) {
  float t2=t*t,t3=t2*t;
  return vec4(-.5*t+t2-.5*t3,1.-2.5*t2+1.5*t3,.5*t+2.*t2-1.5*t3,-.5*t2+.5*t3);
}
bool trustedHistory(ivec2 q,ivec2 size,float expected,float tolerance,bool sky) {
  q=clamp(q,ivec2(0),size-1);
  vec4 c=texelFetch(historyColor,q,0);
  float d=texelFetch(historyDepth,q,0).r;
  if(c.a<.999 || any(isnan(c)) || any(isinf(c)) || isnan(d) || isinf(d))return false;
  return sky ? d==0. : d>0. && abs(d-expected)<=tolerance;
}
bool reconstructHistory(vec2 uv,ivec2 size,float expected,float tolerance,bool sky,out vec3 color) {
  vec2 point=uv*vec2(size)-.5,base=floor(point),f=point-base;
  ivec2 origin=ivec2(base);
  if(p.jitterWeight.w>.5) {
    vec4 wx=cubicWeights(f.x),wy=cubicWeights(f.y);
    bool complete=true;vec3 lo=vec3(1e30),hi=vec3(-1e30);
    // Validate every contributing texel, including the two negative outer lobes.
    for(int y=0;y<4;++y)for(int x=0;x<4;++x) {
      if(abs(wx[x]*wy[y])<1e-5)continue;
      ivec2 q=clamp(origin+ivec2(x-1,y-1),ivec2(0),size-1);
      if(!trustedHistory(q,size,expected,tolerance,sky))complete=false;
      vec3 c=texelFetch(historyColor,q,0).rgb;lo=min(lo,c);hi=max(hi,c);
    }
    if(complete) {
      vec3 ax=vec3(wx.x,wx.y+wx.z,wx.w),ay=vec3(wy.x,wy.y+wy.z,wy.w);
      vec3 px=vec3(base.x-.5,base.x+.5+wx.z/ax.y,base.x+2.5)/float(size.x);
      vec3 py=vec3(base.y-.5,base.y+.5+wy.z/ay.y,base.y+2.5)/float(size.y);
      color=vec3(0);
      for(int y=0;y<3;++y)for(int x=0;x<3;++x)
        color+=textureLod(historyColor,vec2(px[x],py[y]),0.).rgb*ax[x]*ay[y];
      // Negative lobes must not invent a halo beyond trusted history evidence.
      color=clamp(color,lo,hi);return true;
    }
  }
  color=vec3(0);float coverage=0.;
  for(int y=0;y<2;++y)for(int x=0;x<2;++x) {
    float w=(x==0?1.-f.x:f.x)*(y==0?1.-f.y:f.y);
    if(w<1e-5)continue;
    ivec2 q=clamp(origin+ivec2(x,y),ivec2(0),size-1);
    if(trustedHistory(q,size,expected,tolerance,sky)) {
      color+=texelFetch(historyColor,q,0).rgb*w;coverage+=w;
    }
  }
  if(coverage<.25)return false;
  color/=coverage;return true;
}
