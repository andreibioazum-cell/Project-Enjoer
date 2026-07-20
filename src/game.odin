package game

import gl "vendor:gles2"

State :: enum { LOBBY, PLAYING }

Game :: struct {
	state:  State,
	player: Player,
	ui:     UI,
}

game_init :: proc(g: ^Game) {
	g.state = .LOBBY
	player_init(&g.player)
}

game_update :: proc(g: ^Game, dt: f32, w, h: i32) {
	ui_set_screen(&g.ui, w, h)
	if g.state == .PLAYING {
		dir := f32(i32(g.ui.move_r) - i32(g.ui.move_l))
		player_update(&g.player, dir, g.ui.jump, dt, w, h)
	} else if g.ui.start_p {
		g.state = .PLAYING
		g.ui.start_p = false
	}
}

game_draw :: proc(g: ^Game, w, h: i32) {
	if g.state == .LOBBY {
		ui_draw_lobby(&g.ui)
	} else {
		draw_rect(0, f32(h)-150, f32(w), 150, {0.1, 0.1, 0.12})
		player_draw(&g.player, w, h)
		ui_draw_game(&g.ui)
	}
}

ui_set_screen :: proc(u: ^UI, w, h: i32) {
	u.screen_w, u.screen_h = w, h
	u.btn_s = f32(h) / 7.5
}

ui_draw_lobby :: proc(u: ^UI) {
	sw, sh := f32(u.screen_w), f32(u.screen_h)
	draw_c(sw/2, sh/2, 100, {0, 0, 0, 1}, u.screen_w, u.screen_h)
}

ui_draw_game :: proc(u: ^UI) {
	s, sw, sh := u.btn_s, f32(u.screen_w), f32(u.screen_h)
	r := s * 0.8
	draw_c(s * 1.5, sh - s * 1.5, r, {0, 0, 0, 0.7}, u.screen_w, u.screen_h)
	draw_c(s * 3.5, sh - s * 1.5, r, {0, 0, 0, 0.7}, u.screen_w, u.screen_h)
	draw_c(sw - s * 1.5, sh - s * 1.5, r, {0, 0, 0, 0.7}, u.screen_w, u.screen_h)
}
