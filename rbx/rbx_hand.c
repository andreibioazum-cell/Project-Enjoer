/* A real, closed 3D viewmodel in camera space. World rotation never rotates its
 * axes twice; lighting follows the player smoothly, not one sharp sun probe. */
#include "rbx_render_internal.h"
#include <math.h>

#define SWING_TIME .26f
#define EQUIP_TIME .30f
#define HAND_PI 3.14159265358979323846f
static float swing_left,equip_left,bob_phase,bob_weight,last_x,last_z,hand_light;
static int tracked,light_tracked;
void rbx_hand_reset(void) {
    swing_left=equip_left=bob_phase=bob_weight=last_x=last_z=0;
    hand_light=1;tracked=light_tracked=0;
}
void rbx_hand_swing(void) {swing_left=SWING_TIME;}
void rbx_hand_equip(void) {equip_left=EQUIP_TIME;}
static void update_light(float x,float y,float z,float d) {
    float target=rbx_world_light(x,y+RBX_PLAYER_EYE_HEIGHT,z);
    if (!light_tracked) {hand_light=target;light_tracked=1;}
    else hand_light+=(target-hand_light)*(1-expf(-d*9));
}
void rbx_hand_update(float d) {
    if (!isfinite(d) || d<0) d=0;
    if (d>.05f) d=.05f;
    swing_left=fmaxf(0,swing_left-d);equip_left=fmaxf(0,equip_left-d);
    float x,y,z,moving=0;rbx_player_pos(&x,&y,&z);
    if (tracked && rbx_player_grounded() && !rbx_player_flying()) {
        float dx=x-last_x,dz=z-last_z,distance=sqrtf(dx*dx+dz*dz);
        if (distance<1) {bob_phase=fmodf(bob_phase+distance*3.1f,HAND_PI*2);moving=distance>.0001f;}
    }
    bob_weight+=(moving-bob_weight)*fminf(1,d*12);
    last_x=x;last_z=z;tracked=1;
    update_light(x,y,z,d);
}

typedef struct {float x,y,z;} P;
static P add(P a,P b) {return (P){a.x+b.x,a.y+b.y,a.z+b.z};}
static P sub(P a,P b) {return (P){a.x-b.x,a.y-b.y,a.z-b.z};}
static P mul(P a,float s) {return (P){a.x*s,a.y*s,a.z*s};}
static float length(P a) {return sqrtf(a.x*a.x+a.y*a.y+a.z*a.z);}
static P norm(P a) {float l=length(a);return l>1e-8f ? mul(a,1/l) : (P){0,1,0};}
static P cross(P a,P b) {return (P){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}

static void draw_box(P center,P ax,P ay,P az,float hx,float hy,float hz,int block,uint32_t color) {
    static const float corners[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
    unsigned char value=(unsigned char)(fmaxf(0,fminf(1,hand_light))*255+.5f);
    unsigned char light[4]={value,value,value,value};
    for (int face=0;face<6;face++) {
        P n,tu,tv;float hu,hv,hn;
        if (face<2) {n=ay;tu=ax;tv=az;hu=hx;hv=hz;hn=hy;}
        else if (face<4) {n=az;tu=ax;tv=ay;hu=hx;hv=hy;hn=hz;}
        else {n=ax;tu=az;tv=ay;hu=hz;hv=hy;hn=hx;}
        if (face&1) n=mul(n,-1);
        /* The missing normal offset used to put all six faces through the
         * center, producing intersecting flat polygons instead of a cube. */
        P face_center=add(center,mul(n,hn));
        RbxMaterial *material=block ? rbx_material(block,face) : NULL;
        RbxVertex w[4];
        for (int i=0;i<4;i++) {
            P p=add(add(face_center,mul(tu,corners[i][0]*hu)),mul(tv,corners[i][1]*hv));
            float u=corners[i][0]*.5f+.5f,v=corners[i][1]*.5f+.5f;
            if (face>=2) v=-v; /* local top remains the texture's top at every yaw */
            w[i]=(RbxVertex){p.x,p.y,p.z,u,v};
        }
        rbx3d_polygon(w,4,n.x,n.y,n.z,color,material,light);
    }
}
void rbx_hand_draw(void) {
    float x,y,z;rbx_player_pos(&x,&y,&z);
    if (!isfinite(x+y+z)) return;
    if (!light_tracked) update_light(x,y,z,0);
    float swing=swing_left>0 ? sinf(HAND_PI*(1-swing_left/SWING_TIME)) : 0;
    float equip=equip_left>0 ? equip_left/EQUIP_TIME : 0;
    float lx=.60f+.02f*sinf(bob_phase)*bob_weight-.06f*swing;
    float ly=-.26f+.024f*sinf(bob_phase*2)*bob_weight-.20f*swing-.5f*equip*equip;
    float lz=1.05f-.10f*swing;
    if (screen_w>0 && screen_h>0) {
        float half_w=.6494f*lz*(float)screen_w/screen_h;
        lx=fminf(lx,half_w*.58f);
    }
    float spin=-.58f-.25f*swing,tilt=-.20f+.75f*swing;
    float cg=cosf(spin),sg=sinf(spin),ct=cosf(tilt),st=sinf(tilt);
    /* R_y(spin) * R_x(tilt): all three axes stay unit length and orthogonal. */
    P ax={cg,0,-sg},ay={sg*st,ct,cg*st},az={sg*ct,-st,cg*ct};
    P block={lx,ly,lz},wrist=add(block,mul(ay,-.19f));
    P base={.95f,-.95f,.35f},forward={0,0,1};
    P dir=norm(sub(wrist,base)),arm_u=norm(cross(forward,dir)),arm_v=cross(dir,arm_u);
    P arm=mul(add(wrist,base),.5f);
    float arm_length=length(sub(wrist,base)),half=.15f;

    rbx3d_viewmodel(1);
    P origins[3]={block,wrist,arm},axes[3][3]={{ax,ay,az},{ax,ay,az},{dir,arm_u,arm_v}};
    float halves[3][3]={{half,half,half},{.125f,.055f,.12f},{arm_length*.5f,.065f,.08f}};
    float x0=1e9f,y0=1e9f,x1=-1e9f,y1=-1e9f;int clipped=0;
    for (int b=0;b<3;b++) for (int i=0;i<8;i++) {
        P p=origins[b];
        for (int a=0;a<3;a++) p=add(p,mul(axes[b][a],((i>>a)&1 ? 1 : -1)*halves[b][a]));
        float px,py;
        if (!rbx3d_project(p.x,p.y,p.z,&px,&py)) {clipped=1;continue;}
        x0=fminf(x0,px);y0=fminf(y0,py);x1=fmaxf(x1,px);y1=fmaxf(y1,py);
    }
    if (clipped) rbx3d_depth_clear(0,0,(float)screen_w,(float)screen_h);
    else rbx3d_depth_clear(x0-2,y0-2,x1+2,y1+2);
    draw_box(arm,dir,arm_u,arm_v,arm_length*.5f,.065f,.08f,BLOCK_AIR,0xffe2b48cu);
    draw_box(wrist,ax,ay,az,.125f,.055f,.12f,BLOCK_AIR,0xffeec39au);
    draw_box(block,ax,ay,az,half,half,half,rbx_slot_block(rbx_selected()),0);
    rbx3d_viewmodel(0);
}
