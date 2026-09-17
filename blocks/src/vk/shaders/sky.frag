#version 450
/* Sky gradient matching the software renderer: the horizon follows the camera
 * pitch and the blend is a smoothstep over 90% of the internal frame height. */
layout(push_constant) uniform Sky {
    vec4 geometry;  /* x = focal length, y = internal height, z = sin(pitch), w = cos(pitch) */
    vec4 topColor;
    vec4 bottomColor;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    float height = pc.geometry.y;
    float horizon = height * 0.5 + pc.geometry.x * pc.geometry.z / max(0.05, pc.geometry.w);
    float y = gl_FragCoord.y;
    float k = clamp(1.0 - (horizon - y) / (height * 0.9), 0.0, 1.0);
    k = k * k * (3.0 - 2.0 * k);
    outColor = vec4(mix(pc.topColor.rgb, pc.bottomColor.rgb, k), 1.0);
}
