#define _POSIX_C_SOURCE 200809L
#include "rbx/rbx_world_internal.h"
#include "rbx/rbx_render_internal.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
extern AAssetManager *host_asset_manager(const char *root);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static void capture(Buffer *b,const char *name) {
    const char *directory=getenv("ENJOER_TEST_IMAGES");if(!directory)return;
    char path[600];snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",b->width,b->height);
    for(int y=0;y<b->height;y++)for(int x=0;x<b->width;x++) {
        uint32_t c=b->pixels[y*b->stride+x];fputc(c&255,f);fputc((c>>8)&255,f);fputc((c>>16)&255,f);
    }
    assert(!fclose(f));
}
static void aim(float yaw,float pitch) {
    float y,p;rbx_camera_angles(&y,&p);float size=fminf(screen_w,screen_h);
    rbx_camera_look((yaw-y)*size/2.7f,(p-pitch)*size/2.4f);
}
static void hand_frame(Buffer *b,int occluder) {
    float x,y,z,yaw,pitch;rbx_player_pos(&x,&y,&z);rbx_camera_angles(&yaw,&pitch);
    assert(rbx3d_begin(b,1,x,y+RBX_PLAYER_EYE_HEIGHT,z,yaw,pitch,66));rbx3d_sky(0xff101820u,0xff101820u);
    if(occluder) {
        RbxVertex v[4]={{-2,-2,.081f,0,0},{2,-2,.081f,0,0},{2,2,.081f,0,0},{-2,2,.081f,0,0}};
        rbx3d_viewmodel(1);rbx3d_polygon(v,4,0,0,-1,0xff1020f0u,NULL,NULL);rbx3d_viewmodel(0);
    }
    rbx_hand_draw();rbx3d_end();
}
static void test_viewmodel(Buffer *b) {
    rbx_select(0);rbx_hand_reset();hand_frame(b,0);capture(b,"hand");
    size_t bytes=(size_t)b->stride*b->height*4;uint32_t *reference=malloc(bytes);assert(reference);memcpy(reference,b->pixels,bytes);
    int green=0,occupied=0;
    for(int y=0;y<b->height;y++)for(int x=0;x<b->width;x++) {
        uint32_t c=reference[y*b->stride+x];int r=c&255,g=(c>>8)&255;
        green+=g>r*1.2f && g>40;occupied+=c!=0xff201810u;
    }
    assert(green>200 && occupied>3000); /* visible grass top, not just two flat sides */
    for(int i=0;i<48;i++) {
        aim((i/3)*.39269908f-3.14159265f,(i%3-1)*1.25f);
        hand_frame(b,0);assert(!memcmp(reference,b->pixels,bytes));
    }
    hand_frame(b,1);
    for(int y=0;y<b->height;y++)for(int x=0;x<b->width;x++) {
        int at=y*b->stride+x;if(reference[at]!=0xff201810u)assert(b->pixels[at]==reference[at]);
    }
    free(reference);puts("PASS rendered 3D hand: identical silhouette/UV at 48 yaw/pitch combinations, visible grass top, overlay depth against a near wall");
}
static void set(int x,int y,int z,int block) {if(rbx_world_cell(x,y,z)!=block)assert(rbx_world_set(x,y,z,block));}
static void warm(void) {
    for(int i=0;i<600;i++) {rbx_world_update(8.5f,8.5f);if(!rbx_world_pending())return;}assert(0);
}
static void test_cave_frame(Buffer *b) {
    rbx_world_build(RBX_WORLD_SEED);rbx_player_spawn();rbx_actions_reset();rbx_hand_reset();aim(0,-.1f);
    for(int x=8;x<=27;x++)for(int z=8;z<=27;z++)for(int y=25;y<=35;y++) {
        int wall=x==8 || x==27 || z==8 || z==27 || y==25 || y==35;
        set(x,y,z,wall ? BLOCK_STONE : BLOCK_AIR);
    }
    warm();rbx_hand_reset();rbx_scene_draw(b);capture(b,"mine-closed");
    unsigned long long dark=0,open=0;
    for(int y=0;y<b->height;y++)for(int x=0;x<b->width;x++) {
        uint32_t c=b->pixels[y*b->stride+x];
        for(int ch=0;ch<3;ch++) {int value=(c>>(ch*8))&255;assert(value<24);dark+=value;}
    }
    for(int x=16;x<=19;x++)for(int y=27;y<=32;y++)set(x,y,27,BLOCK_AIR);
    warm();for(int i=0;i<40;i++)rbx_hand_update(.05f);rbx_scene_draw(b);capture(b,"mine-entrance");
    for(int y=0;y<b->height;y++)for(int x=0;x<b->width;x++) {
        uint32_t c=b->pixels[y*b->stride+x];for(int ch=0;ch<3;ch++)open+=(c>>(ch*8))&255;
    }
    assert(open>dark*3);rbx_edits_reset(RBX_WORLD_SEED);
    puts("PASS full cave frame: sealed room and hand stay dark; a portal lights the room without sky-fog glow");
}
static void test_water_frames(Buffer *b) {
    rbx_world_build(RBX_WORLD_SEED);rbx_player_spawn();rbx_actions_reset();rbx_hand_reset();aim(0,-.2f);
    for(int x=10;x<=26;x++)for(int z=15;z<=32;z++) {
        set(x,25,z,BLOCK_SAND);
        for(int y=26;y<=34;y++)set(x,y,z,BLOCK_AIR);
    }
    set(18,34,23,BLOCK_WATER);warm();rbx_scene_draw(b);capture(b,"water-start");
    size_t bytes=(size_t)b->stride*b->height*4;uint32_t *reference=malloc(bytes);assert(reference);memcpy(reference,b->pixels,bytes);
    for(int i=0;i<50;i++) {rbx_water_update(.05f);rbx_world_update(8.5f,8.5f);}
    warm();rbx_scene_draw(b);capture(b,"water-flow");
    int different=0;for(int y=0;y<b->height;y++)for(int x=0;x<b->width;x++) {
        int at=y*b->stride+x;different+=reference[at]!=b->pixels[at];
    }
    assert(different>1000 && rbx_world_cell(18,26,23)==BLOCK_WATER && rbx_world_cell(19,26,23)==BLOCK_WATER);
    free(reference);rbx_edits_reset(RBX_WORLD_SEED);
    puts("PASS fluid render integration: a source becomes a meshed waterfall and a block-by-block pool");
}
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
    if(w==960) {test_viewmodel(&b);test_cave_frame(&b);test_water_frames(&b);}
    gfx_shutdown();free(b.pixels);
}
int main(void) {run(960,540);run(2400,1080);return 0;}
