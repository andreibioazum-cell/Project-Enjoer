/* От первого лица, только сгенерированный блочный ландшафт. */
#include "rbx_internal.h"

void rbx_scene_draw(Buffer *buffer) {
    float x,y,z,yaw,pitch;
    rbx_player_pos(&x,&y,&z,NULL,NULL);
    rbx_camera_angles(&yaw,&pitch);
    int scale=(long)screen_w*screen_h>1500000L ? 2 : 1;
    if (!rbx3d_begin(buffer,scale,x,y+RBX_PLAYER_EYE_HEIGHT,z,yaw,pitch,66)) return;
    rbx3d_sky(0xFF78B8E8u,0xFFC7E5F5u);
    rbx_world_draw();
    rbx3d_end();
}
