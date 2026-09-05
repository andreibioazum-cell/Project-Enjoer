/* Общие хуки Android и браузерного превью. */
#include "rbx_internal.h"
#include <math.h>

double rbx_t_abs;
void rbx_key(const char *name,int down) { rbx_key_state(name,down); }
void rbx_cancel_input(void) { rbx_input_reset(); rbx_key_reset(); }
void reset(void) {
    rbx_cancel_input();
    rbx_world_build(RBX_WORLD_SEED);
    rbx_player_spawn();
    rbx_t_abs=0;
    rbx_input_layout();
}
void init(AAssetManager *assets) {
    (void)assets;
    snd_load("send.wav");
    reset();
    ds_log("Enjoer: горизонтальный блочный мир, ходьба и переключаемый полёт");
}
void update(void) {
    float d=(float)dt;
    if (!isfinite(d) || d<0) d=0;
    if (d>.05f) d=.05f;
    rbx_t_abs+=d;
    rbx_input_layout();
    rbx_player_update(d);
    float x,z;
    rbx_player_pos(&x,NULL,&z,NULL,NULL);
    rbx_world_update(x,z);
}
void draw(Buffer *buffer) { rbx_scene_draw(buffer); rbx_hud_draw(); }
void touch(float x,float y,int action,int id) { rbx_input_touch(x,y,action,id); }
