/* Shared Android / host lifecycle. No script runtime. */
#include "rbx_world_internal.h"
static float save_timer;
void rbx_key(const char *name,int down) {rbx_key_state(name,down);}
void rbx_cancel_input(void) {rbx_input_reset();rbx_key_reset();rbx_actions_reset();}
void game_save(void) {rbx_edits_save();save_timer=0;}
void game_reset(void) {
    if (rbx_edits_dirty() && !rbx_edits_save()) {app_fail("Не удалось сохранить изменения мира");return;}
    rbx_cancel_input();rbx_world_build(RBX_WORLD_SEED);rbx_player_spawn();
    rbx_perf_reset();rbx_select(1);save_timer=0;rbx_input_layout();
}
void game_init(AAssetManager *assets) {
    if (!rbx_materials_load(assets)) return;
    snd_load("jump.wav");snd_load("break.wav");snd_load("place.wav");
    game_reset();app_log("Enjoer: PNG voxel world with half-block building");
}
void game_update(void) {
    rbx_perf_frame(dt);
    float d=(float)dt;
    if (!isfinite(d) || d<0) d=0;
    if (d>.05f) d=.05f;
    rbx_input_layout();rbx_player_update(d);rbx_actions_update(d);
    float x,z;rbx_player_pos(&x,NULL,&z);rbx_world_update(x,z);
    /* Debounced atomic autosave; focus loss and shutdown save immediately. */
    if (rbx_edits_dirty()) {
        save_timer+=d;
        if (save_timer>=3) game_save();
    } else save_timer=0;
}
void game_draw(Buffer *buffer) {rbx_scene_draw(buffer);rbx_hud_draw();}
void game_touch(float x,float y,int action,int id) {rbx_input_touch(x,y,action,id);}
