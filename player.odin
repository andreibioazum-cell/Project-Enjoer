package game

import "core:mem"
import gl "vendor:gles2"
import stbi "vendor:stb/image"

HB :: 48.0   // Хитбокс
SZ :: 128.0  // Визуал спрайта
OFF :: (SZ - HB) / 2.0

Player :: struct {
	x, y, vy: f32,
	speed:    f32,
	is_left:  bool,
	on_ground: bool,
	tex_r, tex_l, prog: gl.uint,
	pos_l, uv_l: gl.int,
}

player_init :: proc(p: ^Player) {
	p.speed = 550.0
	p.x, p.y = 200, 200
}

player_load :: proc(p: ^Player, data: []byte, is_left: bool) {
	w, h, c: i32
	pixels := stbi.load_from_memory(raw_data(data), i32(len(data)), &w, &h, &c, 4)
	if pixels == nil do return
	
	tex: gl.uint
	gl.GenTextures(1, &tex)
	gl.BindTexture(gl.TEXTURE_2D, tex)
	gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST)
	gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST)
	gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
	gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
	gl.TexImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, pixels)
	stbi.image_free(pixels)

	if is_left do p.tex_l = tex
	else do p.tex_r = tex

	if p.prog == 0 {
		vs := "attribute vec2 aP;attribute vec2 aU;varying vec2 vU;void main(){gl_Position=vec4(aP,0,1);vU=aU;}"
		fs := "precision mediump float;varying vec2 vU;uniform sampler2D s;void main(){vec4 c=texture2D(s,vU);if(c.a<0.1)discard;gl_FragColor=c;}"
		p.prog = ui_link_program(vs, fs)
		p.pos_l = gl.GetAttribLocation(p.prog, "aP")
		p.uv_l = gl.GetAttribLocation(p.prog, "aU")
	}
}

player_update :: proc(p: ^Player, dir: f32, jump: bool, dt: f32, sw, sh: i32) {
	p.x += dir * p.speed * dt
	if dir != 0 do p.is_left = dir < 0
	
	p.vy += 3000.0 * dt
	p.y += p.vy * dt
	
	ground := f32(sh) - 150.0
	if p.y + HB > ground {
		p.y = ground - HB
		p.vy = 0
		p.on_ground = true
	} else do p.on_ground = false
	
	if jump && p.on_ground {
		p.vy = -1200.0
		p.on_ground = false
	}
	if p.x < 0 do p.x = 0
	if p.x > f32(sw) - HB do p.x = f32(sw) - HB
}

player_draw :: proc(p: ^Player, sw, sh: i32) {
	tex := p.is_left ? p.tex_l : p.tex_r
	if tex == 0 do tex = p.tex_r // фоллбэк
	if p.prog == 0 || tex == 0 do return

	gl.UseProgram(p.prog)
	gl.Enable(gl.BLEND)
	gl.BindTexture(gl.TEXTURE_2D, tex)

	vx, vy := p.x - OFF, p.y - (SZ - HB)
	nx, ny := vx/f32(sw)*2-1, 1-vy/f32(sh)*2
	nw, nh := SZ/f32(sw)*2, SZ/f32(sh)*2

	vt := [16]f32{ nx, ny, 0, 0, nx+nw, ny, 1, 0, nx, ny-nh, 0, 1, nx+nw, ny-nh, 1, 1 }
	gl.EnableVertexAttribArray(gl.uint(p.pos_l))
	gl.VertexAttribPointer(gl.uint(p.pos_l), 2, gl.FLOAT, false, 16, &vt[0])
	gl.EnableVertexAttribArray(gl.uint(p.uv_l))
	gl.VertexAttribPointer(gl.uint(p.uv_l), 2, gl.FLOAT, false, 16, &vt[2])
	gl.DrawArrays(gl.TRIANGLE_STRIP, 0, 4)
	gl.DisableVertexAttribArray(gl.uint(p.pos_l))
	gl.DisableVertexAttribArray(gl.uint(p.uv_l))
}
