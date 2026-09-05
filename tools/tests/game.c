#define _POSIX_C_SOURCE 200809L
#include "rbx/rbx_world_internal.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
extern AAssetManager *host_asset_manager(const char *root);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static void run(int w,int h) {
    screen_w=w;screen_h=h;
    Buffer b={malloc((size_t)(w+8)*h*4),w,h,w+8};assert(b.pixels);
    for(int y=0;y<h;y++)for(int x=w;x<b.stride;x++)b.pixels[y*b.stride+x]=0x12345678;
    AAssetManager *am=host_asset_manager("game/assets");assert(gfx_init(am));game_init(am);
    double start=now();int max_scale=1;
    for(int i=0;i<180;i++) {
        if(i==20)rbx_key("space",1);
        if(i==35)rbx_key("space",0);
        if(i==50)rbx_player_toggle_flight();
        if(i==55) {rbx_key("space",1);rbx_key("w",1);}
        if(i==95) {rbx_key("space",0);rbx_key("w",0);}
        if(i==110)rbx_camera_look(.12f*h,-.10f*h);
        if(i==145)rbx_player_toggle_flight();
        dt=1.0/60;game_update();
        assert(gfx_begin_frame(&b));game_draw(&b);gfx_end_frame();
        int scale=rbx_render_scale(w,h);if(scale>max_scale)max_scale=scale;assert(!app_failed());
        for(int y=0;y<h;y++)for(int x=w;x<b.stride;x++)assert(b.pixels[y*b.stride+x]==0x12345678);
    }
    int chunks,faces;rbx_world_stats(&chunks,&faces);
    printf("PASS %dx%d: 180 complete frames, jump/flight/fall/UV/HUD/stride, %d chunks, %d exposed quads, scale %d (max %d), %.2f ms/frame\n",w,h,chunks,faces,rbx_render_scale(w,h),max_scale,(now()-start)*1000/180);
    assert(rbx_world_set(17,25,16,BLOCK_AIR));game_update();
    assert(gfx_begin_frame(&b));game_draw(&b);gfx_end_frame();assert(!app_failed());
    assert(rbx_world_set(17,25,16,BLOCK_GRASS));game_update();
    rbx_edits_reset(RBX_WORLD_SEED); /* isolated test has no disk storage */
    gfx_shutdown();free(b.pixels);
}
int main(void) {run(960,540);run(2400,1080);return 0;}
