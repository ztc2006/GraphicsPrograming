#version 460
#extension GL_GOOGLE_include_directive : require
#include "primary_common.glsl"
layout(location=0) rayPayloadInEXT Surface result;
void main(){result.found=0;}
