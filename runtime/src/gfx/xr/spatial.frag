#version 450
layout(set=0,binding=0) uniform sampler2D picture;
layout(location=0) in vec4 tint;
layout(location=1) in vec3 sample_uv;
layout(location=0) out vec4 output_color;
void main() { output_color=tint; if(sample_uv.z>0.5) output_color*=texture(picture,sample_uv.xy); }
