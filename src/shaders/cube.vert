#version 450
// Enjoer vertex stage.  One shader serves both pipelines: the cube and the 2D
// batch share the vertex format (position, colour, uv, layer) and the pushed
// matrix, so a script's sprite and the 3D cube go through exactly the same
// vertex shader.
layout(push_constant) uniform Push { mat4 mvp; } push;
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
