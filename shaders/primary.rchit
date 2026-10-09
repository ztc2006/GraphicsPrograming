#version 460
#extension GL_GOOGLE_include_directive : require
#include "primary_common.glsl"
layout(location=0) rayPayloadInEXT Surface result;
hitAttributeEXT vec2 hit;
void main(){
  InstanceData data=objects[gl_InstanceCustomIndexEXT];
  Vertices vertices=Vertices(data.vertices);Indices indices=Indices(data.indices);
  uvec3 ids=uvec3(indices.i[3*gl_PrimitiveID],indices.i[3*gl_PrimitiveID+1],indices.i[3*gl_PrimitiveID+2]);
  vec3 b=barycentric(hit);
  Vertex a=vertices.v[ids.x],c=vertices.v[ids.y],d=vertices.v[ids.z];
  vec3 ng=safeUnit(cross(c.position-a.position,d.position-a.position),vec3(0,0,1));
  ng=safeUnit(ng*mat3(gl_WorldToObjectEXT),vec3(0,0,1));
  vec3 n=safeUnit((a.normal*b.x+c.normal*b.y+d.normal*b.z)*mat3(gl_WorldToObjectEXT),ng);
  float facing=dot(ng,-gl_WorldRayDirectionEXT);
  if((data.surface.z!=0 || (uint(data.optical.w)&1u)!=0) && facing<0){ng=-ng;n=-n;}
  if(dot(n,-gl_WorldRayDirectionEXT)<0)n=ng;
  float textureKernel=0;
  if(data.textures.w!=0xffffffffu && (uint(lighting.environment.w)&1u)!=0){
    vec4 normalSample=sampleMap(data.textures.w,a.normalUv*b.x+c.normalUv*b.y+d.normalUv*b.z,vec4(.5,.5,1,0));
    vec4 tangent=a.tangent*b.x+c.tangent*b.y+d.tangent*b.z;
    vec3 t=mat3(gl_ObjectToWorldEXT)*tangent.xyz;t=safeUnit(t-n*dot(t,n),safeUnit(cross(abs(n.y)<.99?vec3(0,1,0):vec3(1,0,0),n),vec3(1,0,0)));
    float orientation=determinant(mat3(gl_ObjectToWorldEXT))<0 ? -1 : 1;
    vec3 bitangent=cross(n,t)*tangent.w*orientation;
    vec3 mapped=normalSample.xyz*2-1;mapped.xy*=data.surface.w;
    n=safeUnit(mat3(t,bitangent,n)*safeUnit(mapped,vec3(0,0,1)),n);
    if(dot(n,ng)<=0 || dot(n,-gl_WorldRayDirectionEXT)<=0)n=ng;
    float loss=clamp(normalSample.a,0,1);
    textureKernel=min(2*loss/max(1-loss,1.0/255.0)*data.surface.w*data.surface.w,1);
  }
  vec4 orm=sampleMap(data.moreTextures.x,a.metallicRoughnessUv*b.x+c.metallicRoughnessUv*b.y+d.metallicRoughnessUv*b.z,vec4(1));
  float rough=clamp(orm.g*data.pbr.y,.04,1);
  if((uint(lighting.environment.w)&2u)!=0)rough=sqrt(sqrt(min(1,pow(rough,4)+textureKernel)));
  float weight=clamp(data.specular.w*sampleMap(data.moreTextures.w,a.specularUv*b.x+c.specularUv*b.y+d.specularUv*b.z,vec4(1)).a,0,1);
  float ior=data.optical.x>0?data.optical.x:1.5;
  float baseF0=pow((ior-1)/(ior+1),2);
  vec3 f0=min(baseF0*data.specular.rgb*sampleMap(data.maps.x,a.specularColorUv*b.x+c.specularColorUv*b.y+d.specularColorUv*b.z,vec4(1)).rgb,vec3(1));
  result.position=gl_WorldRayOriginEXT+gl_HitTEXT*gl_WorldRayDirectionEXT;
  result.distance=gl_HitTEXT;result.normal=n;result.geometricNormal=ng;
  result.roughness=rough;result.metallic=clamp(orm.b*data.pbr.x,0,1);
  result.albedo=surfaceColor(data,hit,uint(gl_PrimitiveID)).rgb;
  result.specularWeight=weight;result.dielectricF0=f0;result.F90=mix(weight,1,result.metallic);
  result.emission=(data.surface.z!=0 || facing>0) ? emitterColor(gl_InstanceCustomIndexEXT,gl_PrimitiveID,b) : vec3(0);
  result.ids=uvec2(gl_InstanceCustomIndexEXT,gl_PrimitiveID);result.found=1;result.proxy=uint(data.pbr.w);result.unused=uint(facing>0);
  result.optical=data.optical;result.absorptionThickness=data.absorptionThickness;
}
