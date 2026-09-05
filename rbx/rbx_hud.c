/* Только управление: чистый белый прицел, чёрный стик без подложки,
 * белый переключатель полёта и прыжок только в режиме ходьбы. */
#include "rbx_internal.h"
#include <math.h>

static void centered(const char *s,float x,float y,float scale) {
    text_scaled(s,x-text_width(s)*scale*.5f,y,0xFF000000u,scale);
}
void rbx_hud_draw(void) {
    float u=fminf(screen_w/960.0f,screen_h/540.0f);
    float cx,cy,r,jx,jy;
    rbx_input_joy_geom(&cx,&cy,&r); rbx_input_joy(&jx,&jy);
    ring(cx,cy,r,7*u,0xFF000000u);
    circle(cx+jx*r*.56f,cy+jy*r*.56f,r*.38f,0xFF000000u);

    float x,y,w,h;
    rbx_input_flight_geom(&x,&y,&w,&h);
    roundrect(x,y,w,h,7*u,0xFFFFFFFFu);
    centered("Полёт",x+w*.44f,y+9*u,.48f*u);
    if (rbx_player_flying()) {
        line(x+w-28*u,y+23*u,x+w-23*u,y+28*u,3*u,0xFF000000u);
        line(x+w-23*u,y+28*u,x+w-14*u,y+15*u,3*u,0xFF000000u);
    } else ring(x+w-21*u,y+h*.5f,6*u,2*u,0xFF000000u);

    if (!rbx_player_flying()) {
        rbx_input_jump_geom(&cx,&cy,&r);
        circle(cx,cy,r,0xFFFFFFFFu);
        line(cx,cy-22*u,cx,cy+1*u,4*u,0xFF000000u);
        line(cx,cy-22*u,cx-10*u,cy-12*u,4*u,0xFF000000u);
        line(cx,cy-22*u,cx+10*u,cy-12*u,4*u,0xFF000000u);
        centered("Прыжок",cx,cy+9*u,.28f*u);
    }
    /* Без чёрной обводки, фона, счётчика монет и подсказок. */
    float mx=screen_w*.5f,my=screen_h*.5f;
    rect(mx-6*u,my-u,12*u,2*u,0xFFFFFFFFu);
    rect(mx-u,my-6*u,2*u,12*u,0xFFFFFFFFu);
}
