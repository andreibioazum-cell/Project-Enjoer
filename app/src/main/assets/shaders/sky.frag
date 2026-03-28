#version 300 es
precision highp float;
in vec3 vN, vP;
uniform vec3 camP;
out vec4 fC;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
    vec3 d = normalize(vP - camP);
    // Глубокий синий градиент
    vec3 col = mix(vec3(0.0, 0.02, 0.05), vec3(0.0, 0.1, 0.25), clamp(d.y + 0.2, 0.0, 1.0));
    
    // Рисуем звезды только вверху
    if(d.y > 0.05) {
        // Полярная проекция для звезд сверху
        vec2 uv = d.xz / (d.y + 0.8); 
        vec2 id = floor(uv * 100.0);
        if(hash(id) > 0.98) {
            float s = smoothstep(0.1, 0.0, length(fract(uv * 100.0) - 0.5));
            col += vec4(vec3(s), 1.0).rgb;
        }
    }
    fC = vec4(col, 1.0);
}
