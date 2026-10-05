#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(push_constant) uniform Probe { vec2 uv; float lod; } probe;
layout(location=0) out vec4 color;
void main() { color = textureLod(image, probe.uv, probe.lod); }
