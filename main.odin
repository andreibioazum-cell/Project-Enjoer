package game

import "core:fmt"
import "core:runtime"
import "core:time"
import "core:mem"
import gl "vendor:gles2"

// Глобальные переменные для удобства (как в C версии)
g_app: ^Game
sw, sh: i32
rect_prg: gl.uint
pos_loc, col_loc: gl.int

// Точка входа для Android (Native App Glue)
@(export)
android_main :: proc "c" (app: ^rawptr) {
	context = runtime.default_context()
	
	// Здесь должна быть инициализация EGL (обычно через Glue)
	// Для краткости предполагаем, что sw/sh обновляются в цикле
	
	g_app = new(Game)
	game_init(g_app)
	ui_init_circle_shader()
	
	// Ректангл шейдер для draw_rect
	vs := "attribute vec2 p;void main(){gl_Position=vec4(p,0,1);}"
	fs := "precision mediump float;uniform vec4 c;void main(){gl_FragColor=c;}"
	rect_prg = ui_link_program(vs, fs)
	pos_loc = gl.GetAttribLocation(rect_prg, "p")
	col_loc = gl.GetUniformLocation(rect_prg, "c")

	last_time := time.now()
	for {
		// Расчет времени
		now := time.now()
		dt := f32(time.duration_seconds(time.diff(last_time, now)))
		last_time = now
		if dt > 0.1 do dt = 0.1 // Защита от рывков

		// Отрисовка
		gl.Viewport(0, 0, sw, sh)
		gl.ClearColor(0.2, 0.4, 0.7, 1.0)
		gl.Clear(gl.COLOR_BUFFER_BIT)

		game_update(g_app, dt, sw, sh)
		game_draw(g_app, sw, sh)
		
		// Тут eglSwapBuffers(dpy, srf) вызывается через Glue
	}
}

// Универсальная функция отрисовки пола и фонов
draw_rect :: proc(x, y, w, h: f32, color: [3]f32) {
	gl.UseProgram(rect_prg)
	nx, ny := x/f32(sw)*2-1, 1-y/f32(sh)*2
	nw, nh := w/f32(sw)*2, h/f32(sh)*2
	vt := [8]f32{nx, ny, nx+nw, ny, nx, ny-nh, nx+nw, ny-nh}
	gl.Uniform4f(col_loc, color.r, color.g, color.b, 1)
	gl.EnableVertexAttribArray(gl.uint(pos_loc))
	gl.VertexAttribPointer(gl.uint(pos_loc), 2, gl.FLOAT, false, 0, &vt)
	gl.DrawArrays(gl.TRIANGLE_STRIP, 0, 4)
	gl.DisableVertexAttribArray(gl.uint(pos_loc))
}
