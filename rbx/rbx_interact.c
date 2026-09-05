/* Half-cell DDA picking, building and independent held-action repeat. */
#include "rbx_internal.h"
static const int slots[HOTBAR_SLOTS]={BLOCK_GRASS,BLOCK_DIRT,BLOCK_STONE,BLOCK_SAND,BLOCK_LOG,BLOCK_LEAVES,BLOCK_WATER};
static int selected=1,held[ACTION_COUNT],pending[ACTION_COUNT],has_target;
static float cooldown[ACTION_COUNT];
static RbxHit target;
int rbx_raycast(float x,float y,float z,float dx,float dy,float dz,float reach,RbxHit *hit) {
    if (!hit || !isfinite(x+y+z+dx+dy+dz+reach) || reach<=0 || reach>64 ||
        fabsf(x)>10000000 || fabsf(z)>10000000 || fabsf(y)>10000000) return 0;
    float length=sqrtf(dx*dx+dy*dy+dz*dz);
    if (!isfinite(length) || length<.00001f) return 0;
    float p[3]={x,y,z},d[3]={dx/length,dy/length,dz/length},next[3],delta[3];
    int cell[3],step[3],normal[3]={0,0,0};
    for (int i=0;i<3;i++) {
        cell[i]=(int)floorf(p[i]*2);step[i]=d[i]>=0 ? 1 : -1;
        delta[i]=fabsf(d[i])>.000001f ? .5f/fabsf(d[i]) : INFINITY;
        next[i]=isfinite(delta[i]) ? ((cell[i]+(step[i]>0))*.5f-p[i])/d[i] : INFINITY;
    }
    float distance=0;
    for (int n=0;n<256 && distance<=reach;n++) {
        int b=rbx_world_cell(cell[0],cell[1],cell[2]);
        if (b!=BLOCK_AIR && b!=BLOCK_WATER) {
            *hit=(RbxHit){cell[0],cell[1],cell[2],normal[0],normal[1],normal[2],b,distance};
            return 1;
        }
        int a=next[0]<=next[1] && next[0]<=next[2] ? 0 : next[1]<=next[2] ? 1 : 2;
        distance=next[a];next[a]+=delta[a];cell[a]+=step[a];
        normal[0]=normal[1]=normal[2]=0;normal[a]=-step[a];
    }
    return 0;
}
void rbx_select(int index) { if(index>=0 && index<HOTBAR_SLOTS) selected=index; }
int rbx_selected(void) { return selected; }
int rbx_slot_block(int index) { return index>=0 && index<HOTBAR_SLOTS ? slots[index] : BLOCK_DIRT; }
void rbx_actions_reset(void) {
    memset(held,0,sizeof(held));memset(pending,0,sizeof(pending));memset(cooldown,0,sizeof(cooldown));has_target=0;
}
void rbx_action_hold(int action,int down,int source) {
    if (action<0 || action>=ACTION_COUNT || source<0 || source>1) return;
    int bit=1<<source;
    if (down && !(held[action]&bit)) pending[action]|=bit;
    if (down) held[action]|=bit;else held[action]&=~bit;
}
void rbx_action_cancel(int action,int source) {
    if (action<0 || action>=ACTION_COUNT || source<0 || source>1) return;
    held[action]&=~(1<<source);pending[action]&=~(1<<source);
    if (!held[action] && !pending[action]) cooldown[action]=0;
}
void rbx_action_pulse(int action) {if(action>=0 && action<ACTION_COUNT)pending[action]|=4;}
int rbx_action_apply(int action,const RbxHit *h) {
    if (!h || !isfinite(h->distance) || h->distance<0 || h->distance>6 || h->y<0 || h->y>=WORLD_HEIGHT*2 ||
        h->x < -20000000 || h->x>20000000 || h->z < -20000000 || h->z>20000000) return 0;
    if (!rbx_cell_solid(h->x,h->y,h->z)) return 0;
    if (action==ACTION_BREAK) {
        if (h->y<2) return 0;
        if (!rbx_world_set(h->x,h->y,h->z,BLOCK_AIR)) return 0;
        snd_play("break.wav");return 1;
    }
    if (action!=ACTION_PLACE || abs(h->nx)+abs(h->ny)+abs(h->nz)!=1) return 0;
    int x=h->x+h->nx,y=h->y+h->ny,z=h->z+h->nz;
    if (rbx_cell_solid(x,y,z) || rbx_player_overlaps(x,y,z)) return 0;
    if (!rbx_world_set(x,y,z,slots[selected])) return 0;
    snd_play("place.wav");return 1;
}
static void pick(void) {
    float x,y,z,yaw,pitch;rbx_player_pos(&x,&y,&z);rbx_camera_angles(&yaw,&pitch);
    has_target=rbx_raycast(x,y+RBX_PLAYER_EYE_HEIGHT,z,sinf(yaw)*cosf(pitch),sinf(pitch),cosf(yaw)*cosf(pitch),6,&target);
}
void rbx_actions_update(float d) {
    if (!isfinite(d) || d<0) d=0;
    pick();
    for (int a=0;a<ACTION_COUNT;a++) {
        cooldown[a]=fmaxf(0,cooldown[a]-d);
        if (pending[a] || (held[a] && cooldown[a]<=0)) {
            if (has_target && rbx_action_apply(a,&target)) pick();
            cooldown[a]=a==ACTION_BREAK ? .18f : .22f;
        }
        pending[a]=0;
    }
}
int rbx_target(RbxHit *hit) { if(hit && has_target)*hit=target;return has_target; }
void rbx_target_draw(void) {
    if (!has_target || target.y<2) return;
    /* Немного расширяем обводку outward чтобы не z-fighting с гранью блока. */
    const float eps=.004f;
    float p[3]={target.x*.5f - eps, target.y*.5f - eps, target.z*.5f - eps};
    float size=.5f + 2*eps;
    for (int axis=0;axis<3;axis++) for (int a=0;a<2;a++) for (int b=0;b<2;b++) {
        float v[3]={p[0],p[1],p[2]},end[3];
        v[(axis+1)%3]+=a*size;v[(axis+2)%3]+=b*size;
        memcpy(end,v,sizeof(end));end[axis]+=size;
        rbx3d_segment(v[0],v[1],v[2],end[0],end[1],end[2],0xffffffffu);
    }
}
