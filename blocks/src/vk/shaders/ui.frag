#version 450
/* HUD fragment stage: flat colour for the shapes, the baked glyph atlas for
 * text and the block texture array for the hotbar icons. */
layout(set = 0, binding = 0) uniform sampler2DArray tiles;
layout(set = 1, binding = 0) uniform sampler2D glyphs;

layout(push_constant) uniform Push {
    vec4 screen;
    vec4 tint;
    vec4 mode;
} pc;

layout(location = 0) in vec4 color;
layout(location = 1) in vec2 uv;
layout(location = 2) in float layer;

layout(location = 0) out vec4 outColor;

void main() {
    if (pc.mode.x < 0.5) {
        outColor = color;
    } else if (pc.mode.x < 1.5) {
        float coverage = texture(glyphs, uv).r;
        outColor = vec4(color.rgb, color.a * coverage);
    } else {
        outColor = texture(tiles, vec3(uv, layer)) * color;
    }
}
