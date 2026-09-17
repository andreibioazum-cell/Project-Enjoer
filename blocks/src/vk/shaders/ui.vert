#version 450
/* HUD vertex stage: pixel coordinates with the origin in the top-left corner,
 * the same space the software HUD primitives fill. */
layout(push_constant) uniform Push {
    vec4 screen;   /* xy = frame size in pixels, zw = font atlas size in texels */
    vec4 tint;     /* rgba multiplier (text colour) */
    vec4 mode;     /* x = 0 solid, 1 font atlas, 2 block image */
} pc;

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in float inLayer;

layout(location = 0) out vec4 color;
layout(location = 1) out vec2 uv;
layout(location = 2) out float layer;

void main() {
    gl_Position = vec4(inPos.x / pc.screen.x * 2.0 - 1.0,
                       inPos.y / pc.screen.y * 2.0 - 1.0, 0.0, 1.0);
    color = inColor * pc.tint;
    uv = inUv;
    layer = inLayer;
}
