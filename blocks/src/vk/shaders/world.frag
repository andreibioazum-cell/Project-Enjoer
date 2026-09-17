#version 450
/* World / hand fragment stage: block texture, baked light and distance fog.
 * The water pass uses the same shader with a semi-transparent alpha multiplier
 * and blending enabled, which is how the software renderer draws lakes too. */
layout(set = 0, binding = 0) uniform sampler2DArray tiles;

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 camPosAlpha;
    vec4 fog;
    vec4 fogColor;
    vec4 flags;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 2) in float shade;
layout(location = 3) in float layer;
layout(location = 4) in float viewDistance;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(tiles, vec3(uv, layer));
    float light = clamp(shade, 0.0, 1.0);
    vec3 shaded = texel.rgb * color.rgb * light;
    float alpha = texel.a * color.a * pc.camPosAlpha.w;
    if (pc.flags.x < 0.5 && pc.fog.z > 0.5) {
        float t = clamp((viewDistance - pc.fog.x) * pc.fog.y, 0.0, 1.0);
        /* Dark cave surfaces do not glow with the sky colour: the fog itself is
         * dimmed by the local light, exactly like the software shade palette. */
        vec3 fogTint = pc.fogColor.rgb * min(1.0, light * 2.0);
        shaded = mix(shaded, fogTint, t);
    }
    outColor = vec4(shaded, alpha);
}
