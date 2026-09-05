/* Независимые пальцы: стик, обзор, прыжок и переключатель полёта. */
#include "rbx_internal.h"
#include <math.h>

static int joy_id=-1, look_id=-1, jump_id=-1, flight_id=-1, flight_tap;
static float jx,jy,look_x,look_y,flight_start_x,flight_start_y;
static float joy_x,joy_y,joy_r,jump_x,jump_y,jump_r,flight_x,flight_y,flight_w,flight_h,ui_scale;
static int layout_w,layout_h;

void rbx_input_reset(void) {
    joy_id=look_id=jump_id=flight_id=-1;
    jx=jy=0; flight_tap=0;
    rbx_player_jump(0);
}
void rbx_input_layout(void) {
    if (rbx_player_flying() && jump_id >= 0) { jump_id=-1; rbx_player_jump(0); }
    if (screen_w == layout_w && screen_h == layout_h) return;
    layout_w=screen_w; layout_h=screen_h;
    rbx_input_reset();
    ui_scale=fminf(screen_w/960.0f,screen_h/540.0f);
    if (ui_scale <= 0) { joy_r=0; return; }
    joy_r=60*ui_scale; joy_x=86*ui_scale; joy_y=screen_h-86*ui_scale;
    jump_r=40*ui_scale; jump_x=screen_w-78*ui_scale; jump_y=screen_h-82*ui_scale;
    flight_w=128*ui_scale; flight_h=44*ui_scale;
    flight_x=screen_w-flight_w-18*ui_scale; flight_y=18*ui_scale;
}
void rbx_input_joy(float *x,float *y) { if(x)*x=jx; if(y)*y=jy; }
void rbx_input_joy_geom(float *x,float *y,float *r) { if(x)*x=joy_x; if(y)*y=joy_y; if(r)*r=joy_r; }
void rbx_input_jump_geom(float *x,float *y,float *r) { if(x)*x=jump_x; if(y)*y=jump_y; if(r)*r=jump_r; }
void rbx_input_flight_geom(float *x,float *y,float *w,float *h) {
    if (x) *x=flight_x;
    if (y) *y=flight_y;
    if (w) *w=flight_w;
    if (h) *h=flight_h;
}
static int hit_circle(float x,float y,float cx,float cy,float r) {
    float dx=x-cx,dy=y-cy;
    return dx*dx+dy*dy <= r*r;
}
static int hit_flight(float x,float y) {
    return x>=flight_x && x<=flight_x+flight_w && y>=flight_y && y<=flight_y+flight_h;
}
static void set_joy(float x,float y) {
    float dx=(x-joy_x)/joy_r,dy=(y-joy_y)/joy_r;
    float length=sqrtf(dx*dx+dy*dy);
    if (length<=.10f) { jx=jy=0; return; }
    float strength=(fminf(length,1)-.10f)/.90f;
    jx=dx/length*strength; jy=dy/length*strength;
}
void rbx_input_touch(float x,float y,int action,int id) {
    if (action==3) { rbx_input_reset(); return; }
    if (id<0 || !isfinite(x+y)) return;
    rbx_input_layout();
    if (joy_r<=0) return;
    if (action==0) {
        if (id==joy_id || id==look_id || id==jump_id || id==flight_id) return;
        if (hit_flight(x,y)) {
            if (flight_id<0) { flight_id=id; flight_tap=1; flight_start_x=x; flight_start_y=y; }
        } else if (!rbx_player_flying() && hit_circle(x,y,jump_x,jump_y,jump_r*1.15f)) {
            if (jump_id<0) { jump_id=id; rbx_player_jump(1); }
        } else if (x<screen_w*.5f && hit_circle(x,y,joy_x,joy_y,joy_r*1.3f)) {
            if (joy_id<0) { joy_id=id; set_joy(x,y); }
        } else if (x>=screen_w*.5f && look_id<0) {
            look_id=id; look_x=x; look_y=y;
        }
    } else if (action==2) {
        if (id==joy_id) set_joy(x,y);
        else if (id==look_id) { rbx_camera_look(x-look_x,y-look_y); look_x=x; look_y=y; }
        else if (id==flight_id && !hit_circle(x,y,flight_start_x,flight_start_y,12*ui_scale)) flight_tap=0;
    } else if (action==1 || action==4) { /* up / отмена одного pointer */
        if (id==joy_id) { joy_id=-1; jx=jy=0; }
        if (id==look_id) look_id=-1;
        if (id==jump_id) { jump_id=-1; rbx_player_jump(0); }
        if (id==flight_id) {
            flight_id=-1;
            if (action==1 && flight_tap && hit_flight(x,y)) {
                rbx_player_toggle_flight();
                jump_id=-1; rbx_player_jump(0);
            }
            flight_tap=0;
        }
    }
}
