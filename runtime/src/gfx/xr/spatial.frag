#version 450
layout(set=0,binding=0) uniform sampler2D picture;
layout(location=0) in vec4 tint;
layout(location=1) in vec3 sample_uv;
layout(location=0) out vec4 output_color;
void main() {
    output_color = tint;
    if (sample_uv.z > 1.5) {   // picture 2: only what the game's 2D drew over the region's background
        vec4 ui = texture(picture, sample_uv.xy);
        vec4 under = texture(picture, sample_uv.xy + vec2(0.0, 1.0 / 3.0));
        vec3 d = abs(ui.rgb - under.rgb);
        if (max(d.r, max(d.g, d.b)) < 0.04) discard;
        output_color *= ui;
    } else if (sample_uv.z > 0.5) {
        output_color *= texture(picture, sample_uv.xy);
    }
}
