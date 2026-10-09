#ifndef RT_COMMON_GLSL
#define RT_COMMON_GLSL
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
const float PI=3.141592653589793;
#include "pbr_shared.glsl"
struct Vertex {
  vec3 position; vec3 color; vec3 normal; vec2 uv; vec4 tangent;
  vec2 normalUv; vec2 metallicRoughnessUv; vec2 occlusionUv; vec2 emissiveUv;
  float alpha; vec2 specularUv; vec2 specularColorUv;
};
layout(buffer_reference,scalar,buffer_reference_align=4) readonly buffer Vertices {Vertex v[];};
layout(buffer_reference,scalar,buffer_reference_align=4) readonly buffer Indices {uint i[];};
struct InstanceData {
  uint64_t vertices; uint64_t indices; vec4 tint; vec4 surface; uvec4 textures;
  vec4 pbr; vec4 emission; vec4 specular; uvec4 moreTextures; uvec4 maps;
  vec4 optical; vec4 absorptionThickness;
};
layout(set=0,binding=2,scalar) readonly buffer InstanceTable {InstanceData objects[];};
layout(set=0,binding=3) uniform sampler2D textures[];
layout(set=0,binding=5,std430) readonly buffer Lighting {
  vec4 sunDirection; vec4 sunColor; vec4 strengths; vec4 environment;
  uvec4 counts; uvec4 sampling;
} lighting;
layout(push_constant) uniform Frame {mat4 inverseViewProjection; vec4 camera; uvec4 options;} frame;
struct Surface {
  vec3 position; float distance;
  vec3 geometricNormal; float roughness;
  vec3 normal; float metallic;
  vec3 albedo; float specularWeight;
  vec3 dielectricF0; float F90;
  vec3 emission; uint found;
  uvec2 ids; uint proxy; uint unused;
  vec4 optical; vec4 absorptionThickness;
};
uint hash(uint x){x^=x>>16;x*=2246822519u;x^=x>>13;x*=3266489917u;return x^(x>>16);}
float random(inout uint state){state=hash(state+0x9e3779b9u);return float(state>>8)*0.000000059604644775390625;}
vec3 safeUnit(vec3 v,vec3 fallback){float n=dot(v,v);return n>1e-16 ? v*inversesqrt(n) : fallback;}
vec3 barycentric(vec2 hit){return vec3(1.0-hit.x-hit.y,hit);}
vec4 sampleMap(uint id,vec2 uv,vec4 fallback){return id==0xffffffffu ? fallback : textureLod(textures[nonuniformEXT(id)],uv,0);}
vec4 surfaceColor(InstanceData data,vec2 hit,uint primitive){
  Vertices vertices=Vertices(data.vertices); Indices indices=Indices(data.indices);
  uvec3 ids=uvec3(indices.i[3*primitive],indices.i[3*primitive+1],indices.i[3*primitive+2]);
  vec3 b=barycentric(hit);
  vec2 uv=vertices.v[ids.x].uv*b.x+vertices.v[ids.y].uv*b.y+vertices.v[ids.z].uv*b.z;
  vec3 color=vertices.v[ids.x].color*b.x+vertices.v[ids.y].color*b.y+vertices.v[ids.z].color*b.z;
  float alpha=vertices.v[ids.x].alpha*b.x+vertices.v[ids.y].alpha*b.y+vertices.v[ids.z].alpha*b.z;
  vec4 sampleColor=sampleMap(data.textures.x,uv,vec4(1));
  float mask=data.textures.z!=0 ? sampleMap(data.textures.y,uv,vec4(1)).r : 1;
  return vec4(color,alpha)*sampleColor*data.tint*vec4(1,1,1,mask);
}
vec3 emitterColor(uint instance,uint primitive,vec3 bary){
  InstanceData data=objects[instance];Vertices vertices=Vertices(data.vertices);Indices indices=Indices(data.indices);
  vec2 uv=vec2(0);
  for(uint i=0;i<3;++i)uv+=vertices.v[indices.i[3*primitive+i]].emissiveUv*bary[i];
  return sampleMap(data.moreTextures.z,uv,vec4(1)).rgb*data.emission.rgb;
}
#endif
