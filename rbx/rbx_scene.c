#include "rbx_internal.h"
void rbx_scene_draw(Buffer *buffer) {
    float x,y,z,yaw,pitch;
    rbx_player_pos(&x,&y,&z);rbx_camera_angles(&yaw,&pitch);
    int scale=rbx_render_scale(screen_w,screen_h);
    double start=app_now();
    if (!rbx3d_begin(buffer,scale,x,y+RBX_PLAYER_EYE_HEIGHT,z,yaw,pitch,66)) return;
    rbx3d_sky(0xff78b8e8u,0xffc7e5f5u);
    float distance=rbx_world_distance();rbx3d_fog(distance*.5f,distance);
    rbx_world_draw();rbx_target_draw();rbx3d_end();
    rbx_render_time(app_now()-start);
}
