#version 450
/* Linear upscale of the internal 3D resolution onto the frame, which is what
 * the software renderer's bilinear upscale does. At scale 1 the sample lands on
 * texel centres, so the copy is exact. */
layout(set = 0, binding = 0) uniform sampler2D scene;

layout(push_constant) uniform Push {
    vec4 screen;   /* xy = frame size, zw = scene size */
    vec4 tint;
    vec4 mode;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = gl_FragCoord.xy / pc.screen.zw;
    outColor = vec4(texture(scene, uv).rgb, 1.0);
}
