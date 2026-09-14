#version 450
// Enjoer cube: vertex stage. The MVP matrix arrives through push constants.
layout(push_constant) uniform Push { mat4 mvp; } push;
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 0) out vec3 v_color;
void main() {
    gl_Position = push.mvp * vec4(in_position, 1.0);
    v_color = in_color;
}
