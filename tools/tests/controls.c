/* Регрессии ходьбы, переключаемого полёта, мультитача и чистого HUD. */
#include "rbx/rbx_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__func__,__LINE__,#x); exit(1); } } while(0)
#define CLOSE(a,b) CHECK(fabsf((a)-(b)) < .0004f)
static float scene_x,scene_y,scene_z,scene_yaw,scene_pitch;
static int scene_scale,drawn_faces,jump_labels,flight_labels,joy_rings,knobs,backgrounds,cross_rects;

int rbx_materials_load(AAssetManager *assets) {(void)assets;return 1;}
const Image *rbx_material_icon(int block) {static Image image;CHECK(block>0 && block<BLOCK_COUNT);return &image;}
static int images,fps_labels,action_labels;
void image_draw(const Image *image,float x,float y,float w,float h) {CHECK(image && x>0 && y>0 && w>0 && w==h);images++;}
void rbx3d_fog(float start,float end) {CHECK(start>=0 && end>start && end<=RBX_FOG_END);}
void rbx3d_segment(float x,float y,float z,float a,float b,float c,uint32_t color) {
    CHECK(isfinite(x+y+z+a+b+c) && color==0xffffffffu);
}
int rbx3d_begin(Buffer *b,int sc,float x,float y,float z,float yaw,float pitch,float fov) {
    (void)b; CHECK(fov==66); scene_scale=sc; scene_x=x; scene_y=y; scene_z=z; scene_yaw=yaw; scene_pitch=pitch; return 1;
}
void rbx3d_sky(uint32_t a,uint32_t b) { (void)a; (void)b; }
void rbx3d_end(void) {}
int rbx3d_visible(float x,float y,float z,float hx,float hy,float hz) { (void)x;(void)y;(void)z;(void)hx;(void)hy;(void)hz;return 1; }
void rbx3d_surface(int x,int y,int z,int u,int v,int face,int block) {
    (void)x;(void)y;(void)z; CHECK(u>0 && v>0); CHECK(face>=0&&face<6&&block>0&&block<BLOCK_COUNT); drawn_faces++;
}
void rect(float x,float y,float w,float h,uint32_t c) {
    CHECK(c==0xFFFFFFFFu); CHECK(w<=20 && h<=20); CHECK(fabsf(x-screen_w*.5f)<20 && fabsf(y-screen_h*.5f)<20); cross_rects++;
}
void roundrect(float x,float y,float w,float h,float r,uint32_t c) {
    float bx,by,bw,bh; rbx_input_flight_geom(&bx,&by,&bw,&bh);
    if(fabsf(x-bx)<.01f && fabsf(y-by)<.01f) {CLOSE(w,bw);CLOSE(h,bh);CHECK(r>0&&c==0xffffffffu);}
    else CHECK(x>screen_w*.2f && y>screen_h*.75f && w==h && r>0 && (c==0xffffffffu || c==0xb3222929u));
}
void ring(float x,float y,float r,float th,uint32_t c) {
    float jx,jy,jr; rbx_input_joy_geom(&jx,&jy,&jr);
    CHECK(c==0xFF000000u);
    if (fabsf(x-jx)<.01f && fabsf(y-jy)<.01f) { CLOSE(r,jr); CHECK(th>=2 && th<=6); joy_rings++; }
}
void circle(float x,float y,float r,uint32_t c) {
    float jr; rbx_input_joy_geom(NULL,NULL,&jr); (void)x;(void)y;
    if (fabsf(r-jr)<.001f) backgrounds++;
    if (fabsf(r-jr*.38f)<.001f) { CHECK(c==0xFF000000u); knobs++; }
    else {
        CHECK(c==0xffffffffu);
        float bx,by,br;rbx_input_jump_geom(&bx,&by,&br);
        if(fabsf(x-bx)<.01f && fabsf(y-by)<.01f) {CLOSE(r,br);CHECK(!rbx_player_flying());}
    }
}
void line(float x,float y,float x2,float y2,float th,uint32_t c) { (void)x;(void)y;(void)x2;(void)y2;(void)th; CHECK(c==0xFF000000u); }
int text_width(const char *s) { return (int)strlen(s)*12; }
void text_scaled(const char *s,float x,float y,uint32_t c,float scale) {
    CHECK(scale>0);
    if(!strncmp(s,"FPS ",4)) {
        CHECK(x>screen_w*.8f && y<screen_h*.15f && (c==0xffffffffu || c==0xaa000000u));
        if(c==0xffffffffu)fps_labels++;
    } else if(s[0]>='1' && s[0]<='6' && !s[1]) CHECK(c==0xffffffffu && y>screen_h*.9f);
    else {
        CHECK(c==0xff000000u);
        if(!strcmp(s,"Полёт"))flight_labels++;
        else if(!strcmp(s,"Прыжок"))jump_labels++;
        else if(!strcmp(s,"Ломать") || !strcmp(s,"Ставить"))action_labels++;
        else CHECK(!"Unexpected HUD text");
    }
}

typedef struct { float x,y,z; } Pos;
static Pos pos(void) { Pos p; rbx_player_pos(&p.x,&p.y,&p.z); return p; }
static float pos_dist(Pos a,Pos b) { return sqrtf((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z)); }
static void fresh(void) { rbx_cancel_input(); rbx_player_spawn(); rbx_input_layout(); rbx_player_update(.016f); }
static void aim(float yaw,float pitch) {
    float y,p; rbx_camera_angles(&y,&p); float size=fminf(screen_w,screen_h);
    rbx_camera_look((yaw-y)*size/2.7f,(p-pitch)*size/2.4f);
}
static void tap_flight(int id) {
    float x,y,w,h; rbx_input_flight_geom(&x,&y,&w,&h);
    rbx_input_touch(x+w*.5f,y+h*.5f,0,id); rbx_input_touch(x+w*.5f,y+h*.5f,1,id);
}
static void test_walk_jump(void) {
    fresh(); CHECK(!rbx_player_flying()&&rbx_player_grounded());
    Pos start=pos(); aim(0,.9f); rbx_key("w",1); rbx_player_update(.05f); rbx_key("w",0);
    CLOSE(pos().y,start.y); CLOSE(pos().z-start.z,4.6f*.05f);
    fresh(); start=pos(); float peak=start.y;
    rbx_player_jump(1);
    for (int i=0;i<150;i++) { rbx_player_update(.016f); if(pos().y>peak)peak=pos().y; }
    CHECK(peak>start.y+1.0f&&peak<start.y+1.5f);
    CLOSE(pos().y,start.y); CHECK(rbx_player_grounded());
    rbx_player_jump(0); rbx_player_jump(1); rbx_player_update(.05f); CHECK(pos().y>start.y);
    rbx_player_jump(0);
    puts("PASS walk mode ignores camera pitch, has gravity and a grounded single jump");
}
static void test_toggle_and_flight(void) {
    fresh(); tap_flight(1001); CHECK(rbx_player_flying());
    Pos start=pos(); for(int i=0;i<60;i++)rbx_player_update(.016f); CLOSE(pos_dist(start,pos()),0);
    float cx,cy,r,yaw,pitch; rbx_input_joy_geom(&cx,&cy,&r);
    rbx_input_touch(cx,cy,0,37); rbx_input_touch(cx,cy-r,2,37);
    rbx_input_touch(600,220,0,809); rbx_input_touch(630,130,2,809);
    rbx_camera_angles(&yaw,&pitch); CHECK(pitch>0);
    rbx_player_update(.05f); Pos end=pos();
    CLOSE(end.x-start.x,sinf(yaw)*cosf(pitch)*8*.05f);
    CLOSE(end.y-start.y,sinf(pitch)*8*.05f);
    CLOSE(end.z-start.z,cosf(yaw)*cosf(pitch)*8*.05f);
    float jy; rbx_input_touch(630,130,1,809); rbx_input_joy(NULL,&jy); CLOSE(jy,-1);
    rbx_input_touch(cx,cy-r,1,37);
    start=pos(); for(int i=0;i<10;i++)rbx_player_update(.05f); CLOSE(pos_dist(start,pos()),0);
    /* Отключение в воздухе возвращает падение, а не скрытый полёт. */
    tap_flight(2001); CHECK(!rbx_player_flying());
    rbx_player_update(.05f); CHECK(pos().y<start.y);
    rbx_key("f",1); CHECK(rbx_player_flying());
    rbx_key("f",1); CHECK(rbx_player_flying());
    rbx_key("f",0); rbx_key("f",1); CHECK(!rbx_player_flying()); rbx_key("f",0);
    puts("PASS white-button toggle, keyboard debounce, two-finger flight, hover and gravity restoration");
}
static void test_touch_lifetimes(void) {
    fresh();
    float jx,jy,jr,bx,by,bw,bh,cx,cy,cr;
    rbx_input_joy_geom(&jx,&jy,&jr); rbx_input_jump_geom(&cx,&cy,&cr); rbx_input_flight_geom(&bx,&by,&bw,&bh);
    rbx_input_touch(cx,cy,0,81); rbx_player_update(.016f); CHECK(pos().y>13);
    rbx_input_touch(jx,jy-jr,0,71); rbx_input_touch(600,200,0,91);
    tap_flight(111); CHECK(rbx_player_flying());
    rbx_input_touch(cx,cy,1,81); /* палец исчезнувшего прыжка не отпускает стик */
    rbx_input_joy(NULL,&jy); CLOSE(jy,-1);
    rbx_input_touch(0,0,3,-1); rbx_input_joy(&jx,&jy); CLOSE(jx,0); CLOSE(jy,0);
    CHECK(rbx_player_flying());
    rbx_input_touch(bx+10,by+10,0,45); rbx_input_touch(bx-50,by+100,2,45); rbx_input_touch(bx+10,by+10,1,45);
    CHECK(rbx_player_flying()); /* drag на кнопке не считается tap */
    rbx_input_touch(bx+10,by+10,0,45); rbx_input_touch(bx+10,by+10,4,45);
    CHECK(rbx_player_flying()); /* pointercancel не должен нажимать кнопку */
    rbx_key("w",1); rbx_key("space",1); rbx_cancel_input();
    Pos start=pos(); rbx_player_update(.05f); CLOSE(pos_dist(start,pos()),0);
    rbx_input_joy_geom(&jx,&jy,&jr); rbx_input_touch(jx,jy-jr,0,301);
    screen_w=1280; screen_h=720; rbx_input_layout(); rbx_input_joy(&jx,&jy); CLOSE(jx,0); CLOSE(jy,0);
    screen_w=960; screen_h=540; rbx_input_layout();
    puts("PASS simultaneous jump/look/move, toggle while held, cancelled taps, blur and resize reset");
}
static void no_overlap(void) {
    Pos p=pos(); float r=RBX_PLAYER_RADIUS,h=RBX_PLAYER_HEIGHT;
    for(int y=(int)floorf((p.y+.001f)*2);y<=(int)floorf((p.y+h-.001f)*2);y++)
        for(int z=(int)floorf((p.z-r+.001f)*2);z<=(int)floorf((p.z+r-.001f)*2);z++)
            for(int x=(int)floorf((p.x-r+.001f)*2);x<=(int)floorf((p.x+r-.001f)*2);x++) CHECK(!rbx_cell_solid(x,y,z));
}
static void test_voxel_collisions(void) {
    for(int direction=0;direction<8;direction++) {
        fresh(); aim(direction*.785398f,0); rbx_key("w",1);
        for(int i=0;i<150;i++) { rbx_player_update(.016f); no_overlap(); }
    }
    fresh(); tap_flight(5); rbx_key("space",1);
    for(int i=0;i<120;i++) { rbx_player_update(.016f); no_overlap(); }
    rbx_key("space",0); rbx_key("Shift",1);
    for(int i=0;i<200;i++) { rbx_player_update(.016f); no_overlap(); }
    CHECK(pos().y>=13-.001f);
    rbx_cancel_input();
    puts("PASS voxel walls, terrain steps and vertical flight never penetrate solid blocks");
}

static void test_clean_hud_and_camera(void) {
    fresh();
    joy_rings=knobs=backgrounds=cross_rects=flight_labels=jump_labels=0;
    rbx_hud_draw();
    CHECK(images==6 && fps_labels==1 && action_labels==2);
    CHECK(joy_rings==1&&knobs==1&&backgrounds==0&&cross_rects==2&&flight_labels==1&&jump_labels==1);
    tap_flight(100); rbx_hud_draw(); CHECK(jump_labels==1&&flight_labels==2);
    tap_flight(100); rbx_hud_draw(); CHECK(jump_labels==2&&flight_labels==3);
    Pos p=pos(); float yaw,pitch; rbx_camera_angles(&yaw,&pitch);
    drawn_faces=0; rbx_scene_draw(NULL); CHECK(drawn_faces>0&&scene_scale==1);
    CLOSE(scene_x,p.x); CLOSE(scene_y,p.y+RBX_PLAYER_EYE_HEIGHT); CLOSE(scene_z,p.z);
    CLOSE(scene_yaw,yaw); CLOSE(scene_pitch,pitch);
    screen_w=2400; screen_h=1080; rbx_input_layout(); rbx_scene_draw(NULL); CHECK(scene_scale==2);
    const int sizes[][2]={{960,540},{2400,1080},{640,360},{800,600}};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++) {
        screen_w=sizes[i][0]; screen_h=sizes[i][1]; rbx_input_layout();
        float x,y,r; rbx_input_joy_geom(&x,&y,&r); CHECK(x-r>0&&x+r<screen_w*.5f&&y+r<screen_h);
        float w,h; rbx_input_flight_geom(&x,&y,&w,&h); CHECK(x>screen_w*.5f&&y>0&&x+w<screen_w&&y+h<screen_h*.5f);
    }
    rbx_key("w",1); game_reset(); CHECK(!rbx_player_flying());
    dt=-1; p=pos(); game_update(); CLOSE(pos_dist(p,pos()),0);
    puts("PASS clean HUD: transparent black joystick, thick outer ring, white crosshair, conditional jump and eye camera");
}
static void test_fps_and_quality(void) {
    rbx_perf_reset();for(int i=0;i<60;i++)rbx_perf_frame(1.0/60);CLOSE(rbx_fps(),60);
    rbx_perf_reset();dt=.1;for(int i=0;i<10;i++)game_update();CLOSE(rbx_fps(),10); /* NOT clamped physics 20 FPS */
    rbx_perf_frame(NAN);rbx_perf_frame(-1);CLOSE(rbx_fps(),10);
    rbx_perf_reset();CHECK(rbx_render_scale(960,540)==1);
    for(int i=0;i<30;i++)rbx_render_time(.03);
    CHECK(rbx_render_scale(960,540)==2);
    for(int i=0;i<120;i++)rbx_render_time(.003);
    CHECK(rbx_render_scale(960,540)==1);
    CHECK(rbx_render_scale(2400,1080)==2);
    puts("PASS genuine wall-clock FPS, invalid interval guard, adaptive resolution with hysteresis");
}
int main(void) {
    screen_w=960;screen_h=540;dt=1.0/60;game_init(NULL);
    test_walk_jump(); test_toggle_and_flight(); test_touch_lifetimes(); test_voxel_collisions(); test_clean_hud_and_camera();test_fps_and_quality();
    return 0;
}
