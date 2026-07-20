package game

import "core:math"
import gl "vendor:gles2"
import stbtt "vendor:stb/truetype"

cPrg: gl.uint
cPL, cCL: gl.int

UI :: struct {
	move_l, move_r, jump, start_p: bool,
	screen_w, screen_h: i32,
	btn_s: f32,
}

ui_init_circle_shader :: proc() {
	vs := "attribute vec2 p;void main(){gl_Position=vec4(p,0,1);}"
	fs := "precision mediump float;uniform vec4 c;void main(){gl_FragColor=c;}"
	cPrg = ui_link_program(vs, fs)
	cPL = gl.GetAttribLocation(cPrg, "p")
	cCL = gl.GetUniformLocation(cPrg, "c")
}

draw_c :: proc(cx, cy, r: f32, color: [4]f32, sw, sh: i32) {
	gl.UseProgram(cPrg)
	gl.Uniform4f(cCL, color.r, color.g, color.b, color.a)
	vt: [102]f32
	vt[0], vt[1] = cx/f32(sw)*2-1, 1-cy/f32(sh)*2
	for i in 0..=49 {
		a := f32(i) / 49.0 * math.TAU
		vt[(i+1)*2] = (cx + math.cos(a)*r)/f32(sw)*2-1
		vt[(i+1)*2+1] = 1-(cy + math.sin(a)*r)/f32(sh)*2
	}
	gl.EnableVertexAttribArray(gl.uint(cPL))
	gl.VertexAttribPointer(gl.uint(cPL), 2, gl.FLOAT, false, 0, &vt)
	gl.DrawArrays(gl.TRIANGLE_FAN, 0, 51)
	gl.DisableVertexAttribArray(gl.uint(cPL))
}

ui_handle_input :: proc(u: ^UI, action: i32, x, y: f32) {
	s, sw, sh := u.btn_s, f32(u.screen_w), f32(u.screen_h)
	r := s * 0.8
	is_down := (action & 0xff == 0 || action & 0xff == 5)
	
	lx, ly := s * 1.5, sh - s * 1.5
	rx, ry := s * 3.5, sh - s * 1.5
	jx, jy := sw - s * 1.5, sh - s * 1.5

	dist :: proc(x1, y1, x2, y2: f32) -> f32 { return (x1-x2)*(x1-x2) + (y1-y2)*(y1-y2) }

	if is_down {
		if dist(x, y, lx, ly) < r*r*2 do u.move_l = true
		if dist(x, y, rx, ry) < r*r*2 do u.move_r = true
		if dist(x, y, jx, jy) < r*r*2 do u.jump = true
		if x > sw/2-200 && x < sw/2+200 && y > sh/2-100 && y < sh/2+100 do u.start_p = true
	} else {
		u.move_l, u.move_r, u.jump = false, false, false
	}
}

// Вспомогательная функция для линковки шейдеров
ui_link_program :: proc(vs_src, fs_src: string) -> gl.uint {
	v := gl.CreateShader(gl.VERTEX_SHADER)
	f := gl.CreateShader(gl.FRAGMENT_SHADER)
	c_vs := cstring(raw_data(vs_src))
	c_fs := cstring(raw_data(fs_src))
	gl.ShaderSource(v, 1, &c_vs, nil)
	gl.ShaderSource(f, 1, &c_fs, nil)
	gl.CompileShader(v)
	gl.CompileShader(f)
	p := gl.CreateProgram()
	gl.AttachShader(p, v)
	gl.AttachShader(p, f)
	gl.LinkProgram(p)
	return p
}
