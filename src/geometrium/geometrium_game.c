/* Shared Android / host lifecycle. No script runtime. */
#include "geometrium_world_internal.h"
static float save_timer;
void geometrium_key(const char *name,int down) {geometrium_key_state(name,down);}
void geometrium_cancel_input(void) {geometrium_input_reset();geometrium_key_reset();geometrium_actions_reset();}
void game_save(void) {geometrium_edits_save();save_timer=0;}
void game_reset(void) {
    if (geometrium_edits_dirty() && !geometrium_edits_save()) {app_fail("Could not save the world edits");return;}
    geometrium_cancel_input();geometrium_world_build(GEOMETRIUM_WORLD_SEED);geometrium_player_spawn();
    geometrium_perf_reset();geometrium_select(1);save_timer=0;geometrium_input_layout();geometrium_hand_reset();
}
void game_init(AAssetManager *assets) {
    if (!geometrium_materials_load(assets)) return;
    snd_load("jump.wav");snd_load("break.wav");snd_load("place.wav");
    game_reset();app_log("Enjoer: PNG voxel world with half-block building");
}
void game_update(void) {
    geometrium_perf_frame(dt);
    float d=(float)dt;
    if (!isfinite(d) || d<0) d=0;
    if (d>.05f) d=.05f;
    geometrium_input_layout();geometrium_player_update(d);geometrium_actions_update(d);geometrium_hand_update(d);
    float x,z;geometrium_player_pos(&x,NULL,&z);geometrium_water_update(d);geometrium_world_update(x,z);
    /* Debounced atomic autosave; focus loss and shutdown save immediately. */
    if (geometrium_edits_dirty()) {
        save_timer+=d;
        if (save_timer>=3) game_save();
    } else save_timer=0;
}
void game_draw(Buffer *buffer) {geometrium_scene_draw(buffer);geometrium_hud_draw();}
void game_touch(float x,float y,int action,int id) {geometrium_input_touch(x,y,action,id);}
