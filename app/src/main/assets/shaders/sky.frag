#version 300 es
precision highp float;
in vec3 vN, vP;
uniform vec3 camP;
uniform vec4 uC;
out vec4 fC;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

vec3 get_sky(vec3 d) {
    float t = clamp(d.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 base = mix(vec3(0.005, 0.01, 0.03), vec3(0.0, 0.2, 0.5), t);
    
    // Рисуем звёзды только в верхней полусфере
    if(d.y > 0.02) {
        // Проекция на небесную сферу
        vec2 uv = d.xz / (d.y + 0.001);
        vec2 id = floor(uv * 85.0);
        float star = hash(id);
        if(star > 0.97) {
            vec2 gv = fract(uv * 85.0) - 0.5;
            float twinkle = sin(star * 100.0) * 0.5 + 0.7;
            float s = smoothstep(0.15, 0.0, length(gv)) * twinkle;
            base += vec3(1.0, 0.95, 0.9) * s;
        }
    }
    return base;
}

void main() {
    vec3 vD = normalize(camP - vP);
    vec3 n = normalize(vN);
    // Отражение неба в блоках и само небо
    fC = vec4(get_sky(reflect(-vD, n)), 1.0);
}
