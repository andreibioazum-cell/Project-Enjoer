/* Растекание воды: источник, предел потока, падение в шахту и осушение.
 * Проверки идут на синтетических площадках, чтобы результат не зависел от
 * рельефа, плюс случай на природном озере — тот самый баг, когда после
 * сломанного под водой блока оставался воздух. Все координаты в полуклетках:
 * уровень потока WATER_MAX_FLOW считается тоже в полуклетках. */
#include "rbx/rbx_world_internal.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__func__,__LINE__,#x);exit(1);}}while(0)

int rbx3d_visible(float x,float y,float z,float hx,float hy,float hz) {(void)x;(void)y;(void)z;(void)hx;(void)hy;(void)hz;return 0;}
int rbx3d_box_visible(float x,float y,float z,float hx,float hy,float hz) {(void)x;(void)y;(void)z;(void)hx;(void)hy;(void)hz;return 0;}
int rbx3d_quad_visible(int x,int y,int z,int u,int v,int face) {(void)x;(void)y;(void)z;(void)u;(void)v;(void)face;return 0;}
void rbx3d_surface(int x,int y,int z,int u,int v,int face,int block) {(void)x;(void)y;(void)z;(void)u;(void)v;(void)face;(void)block;}

static int cell(int sx,int sy,int sz) { return rbx_world_cell(sx,sy,sz); }
static void load(float x,float z) {
    rbx_world_update(x,z);
    for (int i=0;rbx_world_pending()&&i<500;i++) rbx_world_update(x,z);
    CHECK(!rbx_world_pending());
}
/* Релаксация обязана сходиться: крутим очередь до полного опустошения.
 * После переполнения очереди вода делает повторный проход по габариту,
 * поэтому опустошение проверяется дважды. */
static void settle(void) {
    int changed=0;
    for (int pass=0;pass<3;pass++) {
        int guard=0;
        while (rbx_water_pending()&&guard++<200000) rbx_water_update(0);
        CHECK(!rbx_water_pending());
        rbx_water_update(0);                          /* поднять повторный проход */
    }
    CHECK(!rbx_water_pending());
    CHECK(!rbx_water_stats(NULL,&changed));           /* всё пересчитано, потерь нет */
}
/* Каменная площадка: каждая полуклетка, иначе вода утечёт сквозь шов блока. */
static void platform(int cx,int cy,int cz,int radius) {
    int r=radius*2;
    for (int dz=-r;dz<=r;dz++) for (int dx=-r;dx<=r;dx++)
        for (int half=0;half<2;half++)
            CHECK(rbx_world_set(cx*2+dx,cy*2+half,cz*2+dz,BLOCK_STONE));
}
static void source(int cx,int cy,int cz) {
    CHECK(rbx_world_set(cx*2,cy*2,cz*2,BLOCK_WATER));
    CHECK(rbx_world_set(cx*2,cy*2+1,cz*2,BLOCK_WATER));
}
static void drain_source(int cx,int cy,int cz) {
    CHECK(rbx_world_set(cx*2,cy*2,cz*2,BLOCK_AIR));
    CHECK(rbx_world_set(cx*2,cy*2+1,cz*2,BLOCK_AIR));
}

/* Поставленный блок воды растекается ромбом на предел потока и уходит без следа. */
static void test_spread(void) {
    const int cx=0,cy=40,cz=0,water=41,reach=WATER_MAX_FLOW+2;
    rbx_world_build(RBX_WORLD_SEED);
    load(.5f,.5f);
    platform(cx,cy,cz,10);
    settle();
    source(cx,water,cz);
    settle();
    int blocks=0,max_level=0;
    for (int dz=-reach;dz<=reach;dz++) for (int dx=-reach;dx<=reach;dx++) {
        int distance=abs(dx)+abs(dz),sx=cx*2+dx,sz=cz*2+dz;
        for (int half=0;half<2;half++) {
            int sy=water*2+half;
            if (distance<=WATER_MAX_FLOW) {
                CHECK(cell(sx,sy,sz)==BLOCK_WATER);
                /* уровень потока — расстояние до источника в полуклетках */
                int level=rbx_water_level(sx,sy,sz);
                CHECK(level==distance);
                if (level>max_level) max_level=level;
            } else {
                CHECK(cell(sx,sy,sz)==BLOCK_AIR);
                CHECK(rbx_water_level(sx,sy,sz)==-1);
            }
        }
        if (distance<=WATER_MAX_FLOW) blocks++;
    }
    CHECK(blocks==1+2*WATER_MAX_FLOW*(WATER_MAX_FLOW+1));   /* ромб: 421 блок */
    CHECK(max_level==WATER_MAX_FLOW);
    CHECK(rbx_water_level(cx*2,water*2,cz*2)==0);           /* центр — источник */
    CHECK(cell(cx*2,cy*2,cz*2)==BLOCK_STONE);               /* вода не размывает пол */
    CHECK(cell(cx*2+2,cy*2+1,cz*2)==BLOCK_STONE);

    drain_source(cx,water,cz);
    settle();
    int left=0;
    for (int dz=-reach;dz<=reach;dz++) for (int dx=-reach;dx<=reach;dx++)
        for (int half=0;half<2;half++)
            if (cell(cx*2+dx,water*2+half,cz*2+dz)==BLOCK_WATER) left++;
    CHECK(left==0);                                          /* без питания высохло */
    CHECK(cell(cx*2,cy*2,cz*2)==BLOCK_STONE);
    puts("PASS источник воды растекается ромбом на предел потока, уровни растут с расстоянием и всё высыхает без питания");
}

/* Шахта сквозь пол: вода падает вниз, заполняет пролёт и растекается по полу. */
static void test_fall(void) {
    const int cx=0,cz=0,upper=40,lower=30;
    rbx_world_build(RBX_WORLD_SEED);
    load(.5f,.5f);
    platform(cx,upper,cz,10);
    platform(cx,lower,cz,10);
    settle();
    source(cx,upper+1,cz);
    settle();
    CHECK(cell(cx*2,(upper+1)*2,cz*2)==BLOCK_WATER);
    CHECK(cell(cx*2,upper*2,cz*2)==BLOCK_STONE);
    /* пробиваем пол под источником */
    CHECK(rbx_world_set(cx*2,upper*2,cz*2,BLOCK_AIR));
    CHECK(rbx_world_set(cx*2,upper*2+1,cz*2,BLOCK_AIR));
    settle();
    CHECK(cell(cx*2,upper*2,cz*2)==BLOCK_WATER);            /* струя в шахте */
    CHECK(cell(cx*2,upper*2+1,cz*2)==BLOCK_WATER);
    CHECK(cell(cx*2,(upper-1)*2,cz*2)==BLOCK_WATER);        /* падает дальше */
    CHECK(cell(cx*2,(lower+5)*2,cz*2)==BLOCK_WATER);        /* весь пролёт заполнен */
    CHECK(cell(cx*2,(lower+1)*2,cz*2)==BLOCK_WATER);        /* долетела до пола */
    CHECK(cell(cx*2,(lower+1)*2+1,cz*2)==BLOCK_WATER);
    CHECK(cell(cx*2+8,(lower+1)*2,cz*2)==BLOCK_WATER);      /* растеклась по полу */
    CHECK(cell(cx*2+8,(lower+1)*2+1,cz*2)==BLOCK_WATER);
    CHECK(cell(cx*2+16,(lower+1)*2,cz*2)==BLOCK_AIR);       /* но не дальше предела */
    CHECK(cell(cx*2,(upper+1)*2,cz*2)==BLOCK_WATER);        /* источник не иссяк */
    CHECK(rbx_water_level(cx*2,(upper+1)*2,cz*2)==0);
    CHECK(rbx_water_level(cx*2,(lower+1)*2,cz*2)>=1);       /* снизу поток, не источник */
    CHECK(cell(cx*2+8,upper*2,cz*2)==BLOCK_STONE);          /* пол цел кроме шахты */
    CHECK(cell(cx*2,lower*2,cz*2)==BLOCK_STONE);
    puts("PASS вода падает в шахту сквозь пробитый пол, заполняет пролёт и растекается внизу");
}

/* Баг из отчёта: блок, поставленный и сломанный под водой, не должен оставлять воздух. */
static void test_refill(void) {
    rbx_world_build(RBX_WORLD_SEED);
    load(.5f,.5f);
    int sx=0,sy=-1,sz=0;
    /* верхняя ячейка озера, у которой есть вода хотя бы с одной стороны */
    for (int x=-20;x<=20&&sy<0;x++) for (int z=-20;z<=20&&sy<0;z++) {
        for (int y=WATER_LEVEL*2+1;y>=2;y--) {
            if (cell(x*2,y,z*2)!=BLOCK_WATER) continue;
            int fed=cell(x*2+2,y,z*2)==BLOCK_WATER||cell(x*2-2,y,z*2)==BLOCK_WATER||
                    cell(x*2,y,z*2+2)==BLOCK_WATER||cell(x*2,y,z*2-2)==BLOCK_WATER;
            if (fed) { sx=x*2;sy=y;sz=z*2; }
            break;
        }
    }
    CHECK(sy>=0);
    CHECK(rbx_world_set(sx,sy,sz,BLOCK_STONE));
    settle();
    CHECK(cell(sx,sy,sz)==BLOCK_STONE);
    CHECK(rbx_water_level(sx,sy,sz)==-1);
    CHECK(rbx_world_set(sx,sy,sz,BLOCK_AIR));
    settle();
    CHECK(cell(sx,sy,sz)==BLOCK_WATER);                     /* вода затекла обратно */
    CHECK(rbx_water_level(sx,sy,sz)>=0);
    int base=sy&~1;                                         /* обе половины блока */
    CHECK(cell(sx,base,sz)==BLOCK_WATER);
    CHECK(cell(sx,base+1,sz)==BLOCK_WATER);
    /* затекшая вода — не источник: у неё есть уровень потока */
    CHECK(rbx_water_level(sx,base,sz)<=WATER_MAX_FLOW);
    puts("PASS сломанный под водой блок снова заполняется водой, а не остаётся воздухом");
}

/* Границы: коренная порода, переполнение уровня, бюджет кадра. */
static void test_limits(void) {
    rbx_world_build(RBX_WORLD_SEED);
    load(.5f,.5f);
    CHECK(!rbx_world_set(0,1,0,BLOCK_WATER));
    CHECK(!rbx_world_set(0,0,0,BLOCK_AIR));
    CHECK(!rbx_world_set(0,WORLD_HEIGHT*2,0,BLOCK_WATER));
    CHECK(!rbx_world_set_value(0,80,0,RBX_CELL_VALUE(BLOCK_WATER,WATER_MAX_FLOW+1)));
    CHECK(!rbx_world_set_value(0,80,0,256));
    CHECK(!rbx_world_set_value(0,80,0,-1));
    /* Коренная порода под ногами не редактируется и не размывается водой. */
    int bx=0,bz=0,bedrock=rbx_terrain_block(bx,1,bz);
    for (int x=-20;x<=20&&bedrock!=BLOCK_STONE;x++)
        for (int z=-20;z<=20;z++)
            if (rbx_terrain_block(x,1,z)==BLOCK_STONE) { bx=x;bz=z;bedrock=BLOCK_STONE;break; }
    CHECK(bedrock==BLOCK_STONE);
    CHECK(cell(bx*2,2,bz*2)==bedrock&&cell(bx*2,3,bz*2)==bedrock);
    /* Одинокая вода в толще камня без питания высыхает, камень остаётся. */
    CHECK(rbx_world_set_value(bx*2,4,bz*2,RBX_CELL_VALUE(BLOCK_WATER,WATER_MAX_FLOW)));
    CHECK(rbx_water_level(bx*2,4,bz*2)==WATER_MAX_FLOW);
    rbx_water_touch(bx*2,4,bz*2);
    CHECK(rbx_water_update(0)<=1024);                       /* бюджет на один кадр */
    settle();
    CHECK(cell(bx*2,4,bz*2)==BLOCK_AIR);                    /* высохла до воздуха */
    CHECK(cell(bx*2,5,bz*2)==bedrock);                      /* вторая половина цела */
    CHECK(cell(bx*2,2,bz*2)==bedrock&&cell(bx*2,3,bz*2)==bedrock);
    CHECK(!rbx_water_pending());
    puts("PASS пределы мира и уровня воды, бюджет кадра и высыхание воды без питания");
}

int main(void) {
    dt=1.0/60;
    test_limits();
    test_refill();
    test_spread();
    test_fall();
    return 0;
}
