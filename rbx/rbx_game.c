/* rbx/rbx_game.c — сборка модуля: хуки рантайма init/update/draw/touch
 * и публичный ввод/сброс удержаний для Android и превью. */
#include "rbx_internal.h"
#include <math.h>

double rbx_t_abs = 0.0;
double rbx_join_t = 0.0;

void rbx_key(const char *name, int down) {
    rbx_key_state(name, down);
}

void rbx_cancel_input(void) {
    rbx_input_reset();
    rbx_key_reset();
}

void init(AAssetManager *assets) {
    (void)assets;
    snd_load("send.wav");
    snd_load("notify.wav");
    reset();
    ds_log("Enjoer: первое лицо, свободный полёт");
}

void reset(void) {
    rbx_world_build();
    rbx_player_spawn();
    rbx_world_collect_reset();
    rbx_join_t = rbx_t_abs = 0;
    rbx_cancel_input();
    rbx_input_layout();
}

void update(void) {
    float d = (float)dt;
    if (!isfinite(d) || d < 0) d = 0;
    if (d > 0.05f) d = 0.05f;
    rbx_t_abs += d;
    rbx_join_t += d;
    rbx_input_layout();
    rbx_player_update(d);
    rbx_bots_update(d);
}

void draw(Buffer *buffer) {
    rbx_scene_draw(buffer);
    rbx_hud_draw();
}

void touch(float x, float y, int action, int pointer_id) {
    rbx_input_touch(x, y, action, pointer_id);
}
