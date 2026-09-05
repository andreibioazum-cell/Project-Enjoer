/* Breaking/building, ray picking, independent controls, half collisions and saves. */
#define _POSIX_C_SOURCE 200809L
#include "rbx/rbx_world_internal.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__func__,__LINE__,#x);exit(1);}}while(0)
#define CLOSE(a,b) CHECK(fabsf((a)-(b))<.001f)
void rbx_cancel_input(void) {rbx_input_reset();rbx_key_reset();rbx_actions_reset();}
int rbx3d_visible(float x,float y,float z,float a,float b,float c) {(void)x;(void)y;(void)z;(void)a;(void)b;(void)c;return 0;}
void rbx3d_surface(int x,int y,int z,int u,int v,int f,int b) {(void)x;(void)y;(void)z;(void)u;(void)v;(void)f;(void)b;}
void rbx3d_segment(float x,float y,float z,float a,float b,float c,uint32_t color) {(void)x;(void)y;(void)z;(void)a;(void)b;(void)c;(void)color;}
static void fresh(void) {app_set_storage(NULL);rbx_cancel_input();rbx_world_build(RBX_WORLD_SEED);rbx_player_spawn();rbx_input_layout();}
static void aim(float yaw,float pitch) {
    float y,p;rbx_camera_angles(&y,&p);float size=fminf(screen_w,screen_h);
    rbx_camera_look((yaw-y)*size/2.7f,(p-pitch)*size/2.4f);
}
static void test_eight_parts(void) {
    fresh();
    for(int i=0;i<8;i++)CHECK(rbx_world_cell(16+(i&1),24+((i>>1)&1),16+(i>>2))==BLOCK_GRASS);
    CHECK(rbx_world_set(17,25,16,BLOCK_AIR));int empty=0;
    for(int i=0;i<8;i++)empty+=rbx_world_cell(16+(i&1),24+((i>>1)&1),16+(i>>2))==BLOCK_AIR;
    CHECK(empty==1 && rbx_world_uniform(8,12,8)==BLOCK_PARTIAL && rbx_edits_count()==1);
    CHECK(rbx_world_set(17,25,16,BLOCK_GRASS));CHECK(rbx_world_uniform(8,12,8)==BLOCK_GRASS);
    CHECK(!rbx_world_set(17,25,16,BLOCK_GRASS));
    CHECK(!rbx_world_set(17,1,16,BLOCK_AIR));CHECK(!rbx_world_set(17,128,16,BLOCK_STONE));
    CHECK(!rbx_world_set(17,25,16,BLOCK_COUNT));
    puts("PASS a parent contains eight independent 0.5-cubes; unchanged cells, bedrock and height bounds are preserved");
}
static void test_ray_and_build(void) {
    fresh();rbx_select(0);RbxHit h;
    for(int layer=0;layer<2;layer++)for(int z=0;z<2;z++)for(int x=0;x<2;x++) {
        CHECK(rbx_raycast(8.25f+x*.5f,16.75f,8.25f+z*.5f,0,-1,0,6,&h));
        CHECK(h.x==16+x && h.y==25-layer && h.z==16+z && h.ny==1);
        CLOSE(h.distance,3.75f+layer*.5f);CHECK(rbx_action_apply(ACTION_BREAK,&h));
    }
    for(int i=0;i<8;i++)CHECK(rbx_world_cell(16+(i&1),24+((i>>1)&1),16+(i>>2))==BLOCK_AIR);
    for(int layer=0;layer<2;layer++)for(int z=0;z<2;z++)for(int x=0;x<2;x++) {
        CHECK(rbx_raycast(8.25f+x*.5f,16.75f,8.25f+z*.5f,0,-1,0,6,&h));
        CHECK(h.y==23+layer);CHECK(rbx_action_apply(ACTION_PLACE,&h));
    }
    CHECK(rbx_world_uniform(8,12,8)==BLOCK_GRASS);
    CHECK(!rbx_raycast(8.25f,30,8.25f,0,-1,0,6,&h));
    CHECK(!rbx_raycast(NAN,0,0,0,-1,0,6,&h));CHECK(!rbx_raycast(0,0,0,0,0,0,6,&h));
    CHECK(rbx_world_set(-1,90,-1,BLOCK_STONE));
    CHECK(rbx_raycast(-.25f,46.75f,-.25f,0,-1,0,6,&h));CHECK(h.x==-1 && h.y==90 && h.z==-1 && h.ny==1);CLOSE(h.distance,1.25f);
    CHECK(rbx_raycast(-2,45.25f,-.25f,1,0,0,6,&h));CHECK(h.nx==-1);CLOSE(h.distance,1.5f);
    CHECK(rbx_raycast(0,45.25f,-.25f,-1,0,0,6,&h));CHECK(h.nx==1 && h.x==-1);CLOSE(h.distance,0);
    CHECK(rbx_world_set(17,27,18,BLOCK_STONE));h=(RbxHit){17,27,18,0,0,-1,BLOCK_STONE,1};
    CHECK(!rbx_action_apply(ACTION_PLACE,&h)); /* would overlap the player */
    rbx_select(2);h.nz=1;CHECK(rbx_action_apply(ACTION_PLACE,&h));CHECK(rbx_world_cell(17,27,19)==BLOCK_STONE);
    CHECK(!rbx_action_apply(ACTION_PLACE,&h)); /* occupied */
    h.distance=6.01f;CHECK(!rbx_action_apply(ACTION_BREAK,&h));h.distance=NAN;CHECK(!rbx_action_apply(ACTION_BREAK,&h));
    h=(RbxHit){4,1,4,0,1,0,BLOCK_STONE,1};CHECK(!rbx_action_apply(ACTION_BREAK,&h));
    CHECK(rbx_world_set(4,2,4,BLOCK_AIR));CHECK(rbx_action_apply(ACTION_PLACE,&h)); /* build on bedrock */
    puts("PASS half-cell DDA, exact negative/boundary hits, eight break/place operations, reach and self/occupied-cell rejection");
}
static uint32_t nearby(void) {
    uint32_t h=2166136261u;
    for(int y=10;y<32;y++)for(int z=12;z<24;z++)for(int x=12;x<24;x++)h=(h^(uint32_t)rbx_world_cell(x,y,z))*16777619u;
    return h;
}
static void test_edit_controls(void) {
    fresh();aim(0,-1.3f);rbx_actions_update(0);RbxHit h;CHECK(rbx_target(&h));
    float x,y,r;rbx_input_action_geom(ACTION_BREAK,&x,&y,&r);
    uint32_t original=nearby();
    rbx_input_touch(x,y,0,51);rbx_input_touch(x,y,4,51);rbx_actions_update(.3f);CHECK(nearby()==original);
    rbx_input_touch(x,y,0,52);rbx_input_touch(x,y,1,52);rbx_actions_update(.016f);CHECK(rbx_world_cell(h.x,h.y,h.z)==BLOCK_AIR);
    rbx_input_touch(x,y,0,53);rbx_key_state("break",1);rbx_actions_update(.016f);
    rbx_input_touch(x,y,4,53);CHECK(rbx_target(&h));rbx_actions_update(.25f);CHECK(rbx_world_cell(h.x,h.y,h.z)==BLOCK_AIR);
    rbx_key_state("break",0);original=nearby();rbx_actions_update(.3f);CHECK(nearby()==original);
    /* A tap of the camera must not release a different finger holding break. */
    rbx_input_touch(x,y,0,54);rbx_actions_update(.016f);
    rbx_input_touch(600,200,0,55);rbx_input_touch(600,200,1,55);rbx_actions_update(.016f);
    CHECK(rbx_target(&h));rbx_actions_update(.25f);CHECK(rbx_world_cell(h.x,h.y,h.z)==BLOCK_AIR);
    rbx_input_touch(x,y,1,54);original=nearby();rbx_actions_update(.3f);CHECK(nearby()==original);
    rbx_key_state("3",1);CHECK(rbx_selected()==2);
    rbx_input_slot_geom(5,&x,&y,&r);rbx_input_touch(x+r/2,y+r/2,0,81);rbx_input_touch(x+r/2,y+r/2,1,81);CHECK(rbx_selected()==5);
    rbx_key_state("break",1);rbx_cancel_input();original=nearby();rbx_actions_update(.3f);CHECK(nearby()==original);
    float jx,jy,jr;rbx_input_joy_geom(&jx,&jy,&jr);rbx_input_touch(jx,jy-jr,0,200);
    rbx_input_touch(600,200,0,201);rbx_input_touch(650,215,2,201);
    rbx_input_action_geom(ACTION_PLACE,&x,&y,&r);rbx_input_touch(x,y,0,202);rbx_input_touch(x,y,4,202);
    rbx_input_joy(NULL,&jy);CLOSE(jy,-1);rbx_cancel_input();
    puts("PASS tap/hold editing, cancelled actions, key/touch ownership, camera-tap independence, hotbar and simultaneous movement/look/place");
}
static void no_overlap(void) {
    float px,py,pz;rbx_player_pos(&px,&py,&pz);
    for(int y=(int)floorf((py+.001f)*2);y<=(int)floorf((py+RBX_PLAYER_HEIGHT-.001f)*2);y++)
        for(int z=(int)floorf((pz-RBX_PLAYER_RADIUS+.001f)*2);z<=(int)floorf((pz+RBX_PLAYER_RADIUS-.001f)*2);z++)
            for(int x=(int)floorf((px-RBX_PLAYER_RADIUS+.001f)*2);x<=(int)floorf((px+RBX_PLAYER_RADIUS-.001f)*2);x++)CHECK(!rbx_cell_solid(x,y,z));
}
static void test_half_collisions(void) {
    fresh();for(int z=16;z<18;z++)for(int x=16;x<18;x++)CHECK(rbx_world_set(x,25,z,BLOCK_AIR));
    for(int i=0;i<100;i++){rbx_player_update(.016f);no_overlap();}
    float x,y,z;rbx_player_pos(&x,&y,&z);CLOSE(y,12.5f);CHECK(rbx_player_grounded());
    fresh();aim(0,0);
    for(int sy=26;sy<31;sy++)for(int sx=15;sx<20;sx++)if(sx!=17)CHECK(rbx_world_set(sx,sy,18,BLOCK_STONE));
    rbx_key_state("w",1);for(int i=0;i<30;i++){rbx_player_update(.016f);no_overlap();}
    rbx_player_pos(&x,&y,&z);CLOSE(z,8.7f); /* 0.5 opening < 0.6 player diameter */
    for(int sy=26;sy<30;sy++)CHECK(rbx_world_set(16,sy,18,BLOCK_AIR));
    for(int i=0;i<30;i++){rbx_player_update(.016f);no_overlap();}
    rbx_player_pos(&x,&y,&z);CHECK(z>10);rbx_cancel_input();
    puts("PASS standing on a remaining half, accurate half-cell walls and correctly sized editable openings");
}
static void warm(float x,float z) {
    rbx_world_update(x,z);for(int i=0;i<500 && rbx_world_pending();i++)rbx_world_update(x,z);CHECK(!rbx_world_pending());
}
static void test_persistence(const char *directory) {
    fresh();mkdir(directory,0700);app_set_storage(directory);char path[600];CHECK(app_save_path(path,sizeof(path),"world.edits"));remove(path);
    CHECK(!app_save_path(path,sizeof(path),"../escape"));CHECK(app_save_path(path,sizeof(path),"world.edits"));
    for(int i=0;i<600;i++)CHECK(rbx_world_set(i*2-600,100,i%23-11,(i%3)+BLOCK_DIRT));
    CHECK(rbx_edits_count()==600 && rbx_edits_dirty());CHECK(rbx_edits_save());CHECK(!rbx_edits_dirty());
    warm(2048,-2048);warm(-4096,4096);warm(8.5f,8.5f);
    for(int i=0;i<600;i++)CHECK(rbx_world_cell(i*2-600,100,i%23-11)==(i%3)+BLOCK_DIRT);
    rbx_world_build(RBX_WORLD_SEED);CHECK(rbx_edits_count()==600);
    for(int i=0;i<600;i++)CHECK(rbx_world_cell(i*2-600,100,i%23-11)==(i%3)+BLOCK_DIRT);
    rbx_world_build(RBX_WORLD_SEED+1);CHECK(rbx_edits_count()==0);
    rbx_world_build(RBX_WORLD_SEED);CHECK(rbx_edits_count()==600);
    FILE *f=fopen(path,"r+b");CHECK(f);CHECK(!fseek(f,24,SEEK_SET));fputc(0x7f,f);fclose(f);
    rbx_world_build(RBX_WORLD_SEED);CHECK(rbx_edits_count()==0); /* checksum rejects corruption */
    f=fopen(path,"wb");CHECK(f);fwrite("EJVOX01",1,7,f);fclose(f);
    rbx_world_build(RBX_WORLD_SEED);CHECK(rbx_edits_count()==0);
    remove(path);app_set_storage(NULL);
    puts("PASS edit rehashing, distant eviction, atomic disk reload, seed/version/CRC/truncation validation and private save paths");
}
int main(int argc,char **argv) {
    CHECK(argc==2);screen_w=960;screen_h=540;dt=1.0/60;
    test_eight_parts();test_ray_and_build();test_edit_controls();test_half_collisions();test_persistence(argv[1]);return 0;
}
