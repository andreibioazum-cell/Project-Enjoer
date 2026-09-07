/* Видимая рука от первого лица: предплечье, кисть и выбранный блок.
 * Рисуется тем же софтверным растеризатором после мира — всегда поверх,
 * но с корректным перекрытием собственных частей и тенью от солнца. */
#include "rbx_render_internal.h"
#include <math.h>

#define SWING_TIME .26f
#define EQUIP_TIME .30f
#define HAND_PI 3.14159265358979323846f

static float swing_left, equip_left, bob_phase, last_x, last_z;
static int tracked;

void rbx_hand_reset(void) { swing_left=equip_left=bob_phase=last_x=last_z=0; tracked=0; }
void rbx_hand_swing(void) { swing_left=SWING_TIME; }
void rbx_hand_equip(void) { equip_left=EQUIP_TIME; }

void rbx_hand_update(float d) {
    if (!isfinite(d) || d<0) d=0;
    if (d>.05f) d=.05f;
    swing_left=fmaxf(0,swing_left-d);
    equip_left=fmaxf(0,equip_left-d);
    float x,y,z;
    rbx_player_pos(&x,&y,&z);
    if (tracked) {
        float dx=x-last_x,dz=z-last_z;
        bob_phase+=sqrtf(dx*dx+dz*dz)*3.1f; /* шаги, а не время: в полёте рука спокойна */
    }
    last_x=x;last_z=z;tracked=1;
}

typedef struct { float x,y,z; } P;
static P padd(P a,P b) { return (P){a.x+b.x,a.y+b.y,a.z+b.z}; }
static P psub(P a,P b) { return (P){a.x-b.x,a.y-b.y,a.z-b.z}; }
static P pscale(P a,float s) { return (P){a.x*s,a.y*s,a.z*s}; }
static P pnorm(P a) {
    float length=sqrtf(a.x*a.x+a.y*a.y+a.z*a.z);
    return length>1e-8f ? pscale(a,1/length) : (P){0,1,0};
}
static P pcross(P a,P b) {
    return (P){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
static float plength(P a) { return sqrtf(a.x*a.x+a.y*a.y+a.z*a.z); }

/* Ориентированный бокс: центр, три ортонормированные оси, полуразмеры.
 * block!=0 — текстура материала целиком на грань, иначе плоский цвет. */
static void draw_box(P center,P ax,P ay,P az,float hx,float hy,float hz,
                     int block,uint32_t color,int shadow) {
    static const float quad_corners[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
    for (int face=0;face<6;face++) {
        P n,tu,tv;
        float hu,hv;
        if (face==0) { n=ay; tu=ax; tv=az; hu=hx; hv=hz; }
        else if (face==1) { n=pscale(ay,-1); tu=ax; tv=az; hu=hx; hv=hz; }
        else if (face==2) { n=az; tu=ax; tv=ay; hu=hx; hv=hy; }
        else if (face==3) { n=pscale(az,-1); tu=ax; tv=ay; hu=hx; hv=hy; }
        else if (face==4) { n=ax; tu=az; tv=ay; hu=hz; hv=hy; }
        else { n=pscale(ax,-1); tu=az; tv=ay; hu=hz; hv=hy; }
        RbxMaterial *material=NULL;
        int textured=block!=BLOCK_AIR;
        if (textured) {
            float fx=fabsf(n.x),fy=fabsf(n.y),fz=fabsf(n.z);
            int id=fy>=fx && fy>=fz ? (n.y>0?0:1) : fz>=fx ? (n.z>0?2:3) : (n.x>0?4:5);
            material=rbx_material(block,id);
            if (!material) textured=0;
        }
        RbxVertex w[4];
        for (int i=0;i<4;i++) {
            float su=quad_corners[i][0]*hu,sv=quad_corners[i][1]*hv;
            P p=padd(padd(center,pscale(tu,su)),pscale(tv,sv));
            float u=su/hu*.5f+.5f,v=sv/hv*.5f+.5f;
            if (!textured) u=v=0;
            /* Боковые текстуры — как у граней мира: v убывает к верху блока. */
            else if (fabsf(n.y)<.5f) v=-v;
            w[i]=(RbxVertex){p.x,p.y,p.z,u,v};
        }
        rbx3d_polygon(w,4,n.x,n.y,n.z,color,textured?material:NULL,shadow);
    }
}

void rbx_hand_draw(void) {
    float x,y,z,yaw,pitch;
    rbx_player_pos(&x,&y,&z);
    rbx_camera_angles(&yaw,&pitch);
    if (!isfinite(x+y+z+yaw+pitch)) return;
    float sy=sinf(yaw),cy=cosf(yaw),sp=sinf(pitch),cp=cosf(pitch);
    /* Базис камеры в мире: right/up/forward из yaw и pitch. */
    P right={cy,0,-sy};
    P forward={cp*sy,sp,cp*cy};
    P up={-sp*sy,cp,-sp*cy};
    P eye={x,y+RBX_PLAYER_EYE_HEIGHT,z};

    float swing=swing_left>0 ? sinf(HAND_PI*(1-swing_left/SWING_TIME)) : 0;
    float equip=equip_left>0 ? equip_left/EQUIP_TIME : 0;

    /* Позиция блока в системе камеры + походка, замах и смена материала. */
    float lx=.42f+.02f*sinf(bob_phase)-.06f*swing;
    float ly=-.25f+.024f*sinf(bob_phase*2)-.27f*swing-.5f*equip*equip;
    float lz=.64f-.08f*swing;
    if (screen_w>0 && screen_h>0) { /* на узком экране прижимаем к центру */
        float half_w=.6494f*lz*(float)screen_w/screen_h;
        if (half_w>0) lx=fminf(lx,half_w*.58f);
    }
    float spin=-.55f-.25f*swing, tilt=.14f+.85f*swing;
    float cg=cosf(spin),sg=sinf(spin),ct=cosf(tilt),st=sinf(tilt);
    /* Оси блока: поворот вокруг локальной вертикали, затем наклон к камере. */
    P bax={cg,0,sg};
    P bay={0,ct,st};
    P baz={-sg,-st*cg,ct*cg};
    float half=.19f;

    P block_c=padd(padd(padd(eye,pscale(right,lx)),pscale(up,ly)),pscale(forward,lz));
    P wrist=padd(block_c,pscale(bay,-.16f));
    P arm_base=padd(padd(padd(eye,pscale(right,.95f)),pscale(up,-.95f)),pscale(forward,.30f));
    P dir=pnorm(psub(wrist,arm_base));
    P arm_u=pnorm(pcross(forward,dir));
    P arm_v=pcross(dir,arm_u);
    float arm_length=plength(psub(wrist,arm_base));
    P arm_c=pscale(padd(wrist,arm_base),.5f);

    /* Оверлей всегда поверх мира: чистим z-буфер под будущими пикселями руки. */
    P origins[3]={block_c,wrist,arm_c};
    P oaxes[3][3]={{bax,bay,baz},{bax,bay,baz},{dir,arm_u,arm_v}};
    float ohalves[3][3]={{half,half,half},{.17f,.055f,.17f},
        {arm_length*.5f,.072f,.086f}};
    float x0=1e9f,y0=1e9f,x1=-1e9f,y1=-1e9f;
    for (int b=0;b<3;b++) for (int i=0;i<8;i++) {
        P p=origins[b];
        for (int axis=0;axis<3;axis++)
            p=padd(p,pscale(oaxes[b][axis],((i>>axis)&1 ? 1 : -1)*ohalves[b][axis]));
        float px,py;
        if (!rbx3d_project(p.x,p.y,p.z,&px,&py)) return;
        if (px<x0) x0=px;
        if (px>x1) x1=px;
        if (py<y0) y0=py;
        if (py>y1) y1=py;
    }
    rbx3d_depth_clear(x0-2,y0-2,x1+2,y1+2);

    /* Тень солнца падает и на руку — как на любой объект мира. */
    P probe=padd(block_c,pscale(bay,half+.05f));
    int shadow=!rbx_sunlit(probe.x,probe.y,probe.z);

    draw_box(arm_c,dir,arm_u,arm_v,arm_length*.5f,.072f,.086f,0,0xffe2b48cu,shadow);
    draw_box(wrist,bax,bay,baz,.17f,.055f,.17f,0,0xffeec39a,shadow);
    draw_box(block_c,bax,bay,baz,half,half,half,rbx_slot_block(rbx_selected()),0,shadow);
}
