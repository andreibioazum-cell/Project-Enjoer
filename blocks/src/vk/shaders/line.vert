#version 450
/* Block selection outline: a handful of coloured segments drawn over the world
 * with a slight depth bias so the wireframe never z-fights with the block face
 * it wraps (the software renderer biases the depth test in the same way). */
layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 camPosAlpha;
    vec4 fog;
    vec4 fogColor;
    vec4 flags;   /* x = viewmodel, y = depth bias in NDC units */
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 color;

void main() {
    vec4 clip = pc.viewProj * vec4(inPos, 1.0);
    /* Bias after the perspective divide, in the same units the depth buffer uses. */
    clip.z -= pc.flags.y * clip.w;
    gl_Position = clip;
    color = inColor;
}
