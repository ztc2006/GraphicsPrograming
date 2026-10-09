#ifndef DIELECTRIC_SHARED_GLSL
#define DIELECTRIC_SHARED_GLSL
float dielectricFresnel(float cosine,float etaI,float etaT){
  if(etaI==etaT)return 0;
  float c=clamp(abs(cosine),0,1),ratio=etaI/etaT;
  float sin2=ratio*ratio*max(0,1-c*c);
  if(sin2>=1)return 1;
  float t=sqrt(max(0,1-sin2));
  float rs=(etaI*c-etaT*t)/max(etaI*c+etaT*t,1e-20);
  float rp=(etaT*c-etaI*t)/max(etaT*c+etaI*t,1e-20);
  return .5*(rs*rs+rp*rp);
}
float thinDielectricReflection(float cosine,float ior){float f=dielectricFresnel(cosine,1,ior);return 2*f/(1+f);}
vec3 dielectricAttenuation(vec3 sigma,float distance){return exp(-max(sigma,vec3(0))*max(distance,0));}
#endif
