#version 450
// Enjoer vertex stage.  The 2D batch is the only thing the engine draws: each
// vertex carries its screen position, tint colour, texture coordinate and
// sprite layer, and the pushed orthographic matrix maps pixels to clip space.
layout(push_constant) uniform Push { mat4 ortho; } push;
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_uv;
// Array layer of the sprite sheet, or negative for an untextured vertex.
layout(location = 3) in float in_layer;
layout(location = 0) out vec3 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out float v_layer;
void main() {
    gl_Position = push.mvp * vec4(in_position, 1.0);
    v_color = in_color;
    v_uv = in_uv;
    v_layer = in_layer;
}
