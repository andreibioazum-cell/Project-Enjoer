#version 300 es
precision highp float;
in vec3 vN, vP;
uniform vec3 camP;
uniform vec4 uC;
uniform float uSel, isFloor;
out vec4 fC;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

vec3 sky(vec3 d) {
    float t = clamp(d.y * 0.5 + 0.5, 0., 1.1); // Смещение градиента выше
    vec3 base = mix(vec3(.002, .005, .02), vec3(.0, .15, .4), t);
    
    // Звёзды теперь привязаны к высоте (d.y > 0)
    if(d.y > -0.05) { 
        vec2 uv = d.xz / (d.y + 0.6); // Изменен коэффициент проекции для "верха"
        vec2 id = floor(uv * 120.);
        float star = hash(id);
        if(star > 0.98) {
            vec2 gv = fract(uv * 120.) - 0.5;
            float dist = length(gv);
            float twinkle = sin(hash(id) * 6.28 + star * 20.0) * 0.5 + 0.5;
            base += vec3(1.0) * smoothstep(0.2, 0.0, dist) * twinkle;
        }
    }
    return base;
}

void main() {
    vec3 n = normalize(vN), vD = normalize(camP - vP), lD = normalize(vec3(.4, 1., .2));
    float diff = max(dot(n, lD), 0.), ao = clamp(vP.y * .08 + .6, .3, 1.);
    vec3 col = uC.rgb; if(uSel > .5) col += vec3(0., .5, .8);
    vec3 amb = vec3(.06, .09, .15) * ao;
    vec3 fin = col * (diff + amb);
    if(isFloor < 0.5) fin += sky(reflect(-vD, n)) * 0.3; // Отражение неба в блоках
    
    if(isFloor > .5) {
        vec2 g = fract(vP.xz * .15);
        float ln = step(.97, g.x) + step(.97, g.y);
        fin = mix(fin, vec3(0., .4, .8), clamp(ln, 0., 1.) * 0.5);
    }
    fC = vec4(fin, uC.a);
}
