#version 450
/* World / first-person-hand vertex stage.
 *
 * Vertices are already in world space (the hand is emitted in camera space and
 * uses the viewmodel flag). The CPU does the frustum and chunk culling exactly
 * as the software renderer does, so no geometry work happens here beyond the
 * transform. `shade` is the baked light value (face orientation times the
 * skylight at that corner) and already matches the software picture. */
layout(push_constant) uniform Push {
    mat4 viewProj;     /* world: projection * view, viewmodel: projection only */
    vec4 camPosAlpha;  /* xyz = eye position, w = alpha multiplier for the pass */
    vec4 fog;          /* x = fade start, y = 1/(end-start), z = fog enabled */
    vec4 fogColor;     /* rgb, w unused */
    vec4 flags;        /* x = viewmodel, y = depth bias, z/w reserved */
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;   /* multiplied over the texture */
layout(location = 3) in float inShade;  /* baked light, 0..1 */
layout(location = 4) in float inLayer;  /* texture array layer, 0..1 */

layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 color;
layout(location = 2) out float shade;
layout(location = 3) out float layer;
layout(location = 4) out float viewDistance;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    uv = inUv;
    color = inColor;
    shade = inShade;
    layer = floor(inLayer * 255.0 + 0.5);
    /* Fog is radial in the software renderer; the eye position is the camera. */
    viewDistance = length(inPos - pc.camPosAlpha.xyz);
}
