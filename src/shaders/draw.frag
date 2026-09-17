#version 450
// Enjoer fragment stage: a shape is its vertex colour, a sprite multiplies the
// sampled texel by the current render colour, so tinting costs nothing.  The
// colour carries a straight (non-premultiplied) alpha and the pipeline blends
// src-over-dst: a translucent shadow, a dim screen fade or a soft dust puff
// are all just the same triangles.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in float v_layer;
layout(location = 0) out vec4 out_color;
// Every image.load("x.png") in the game becomes one layer of this array, bound
// once per frame through descriptor set 0.
layout(set = 0, binding = 0) uniform sampler2DArray u_images;
void main() {
    if (v_layer < 0.0) {
        out_color = v_color;
        return;
    }
    vec4 texel = texture(u_images, vec3(v_uv, v_layer));
    out_color = vec4(texel.rgb * v_color.rgb, texel.a * v_color.a);
}
