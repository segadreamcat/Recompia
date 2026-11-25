#version 450
layout(location=0) in vec4 position;
layout(location=1) in vec4 color;
layout(location=2) in vec3 uv_picture;
layout(location=0) out vec4 tint;
layout(location=1) out vec3 sample_uv;
void main() { gl_Position=position; tint=color; sample_uv=uv_picture; }
