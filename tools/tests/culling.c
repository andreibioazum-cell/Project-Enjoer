/* Отсечение скрытой геометрии.
 *
 * Два требования: кадр с отсечением обязан побитово совпадать с кадром без него
 * (отсечение ничего видимого не теряет) и объём отправленной в рендер геометрии
 * обязан падать — толща породы под ногами и стены глубокой шахты не рисуются.
 * Эталон без отсечения даёт rbx_world_draw_all(): тот же обход чанков и квадов,
 * но без тестов видимости. */
#include "rbx/rbx_world_internal.h"
#include "rbx/rbx_render_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__func__,__LINE__,#x);exit(1);}}while(0)
#define WIDTH 320
#define HEIGHT 240
#define SKY_TOP 0xFF6EC5F7u
#define SKY_BOT 0xFFBFD9E8u

typedef struct { float x,y,z,yaw,pitch; } Pose;

static Buffer make_buffer(void) {
    Buffer b={NULL,WIDTH,HEIGHT,WIDTH};
    b.pixels=malloc((size_t)b.stride*b.height*sizeof(*b.pixels));
    CHECK(b.pixels);
    memset(b.pixels,0,(size_t)b.stride*b.height*sizeof(*b.pixels));
    return b;
}
static void warm(float x,float z) {
    rbx_world_update(x,z);
    for (int i=0;rbx_world_pending()&&i<500;i++) rbx_world_update(x,z);
    CHECK(!rbx_world_pending());
    for (int i=0;i<180;i++) rbx_world_update(x,z);
}
/* Возвращает число треугольников, присланных миру рендером за кадр. */
static int render(Buffer *out,int scale,const Pose *p,int uncullled) {
    CHECK(rbx3d_begin(out,scale,p->x,p->y,p->z,p->yaw,p->pitch,72));
    rbx3d_sky(SKY_TOP,SKY_BOT);
    if (uncullled) rbx_world_draw_all();
    else rbx_world_draw();
    rbx3d_end();
    return rbx3d_frame_triangles();
}
static void same_image(const Buffer *a,const Buffer *b) {
    for (int i=0;i<a->stride*a->height;i++)
        if (a->pixels[i]!=b->pixels[i]) {
            fprintf(stderr,"кадр с отсечением отличается в пикселе %d\n",i);
            CHECK(a->pixels[i]==b->pixels[i]);
        }
}
/* Сколько пикселей занято миром, а не небом: отсечение не должно прятать мир. */
static int coverage(int scale,const Pose *p) {
    Buffer sky=make_buffer(),world=make_buffer();
    CHECK(rbx3d_begin(&sky,scale,p->x,p->y,p->z,p->yaw,p->pitch,72));
    rbx3d_sky(SKY_TOP,SKY_BOT);
    rbx3d_end();
    render(&world,scale,p,0);
    int pixels=0;
    for (int i=0;i<sky.stride*sky.height;i++) if (sky.pixels[i]!=world.pixels[i]) pixels++;
    free(sky.pixels);free(world.pixels);
    return pixels;
}
/* Сухая колонка 2x2 без деревьев и без воды рядом: камера не должна оказаться
 * внутри блока, а выкопанная шахта — затопиться. */
static void find_spot(int *bx,int *bz) {
    for (int x=-24;x<=24;x++) for (int z=-24;z<=24;z++) {
        if (rbx_terrain_height(x,z)<WATER_LEVEL+3) continue;   /* нужна высота под шахту */
        int dry=1;
        for (int dz=-3;dz<=3&&dry;dz++) for (int dx=-3;dx<=3&&dry;dx++)
            if (rbx_terrain_height(x+dx,z+dz)<=WATER_LEVEL) dry=0;
        if (!dry) continue;
        int clear=1;
        for (int y=rbx_terrain_height(x,z)+1;y<=rbx_terrain_height(x,z)+4&&clear;y++)
            for (int dz=0;dz<2&&clear;dz++) for (int dx=0;dx<2&&clear;dx++)
                if (rbx_world_cell((x+dx)*2,y*2,(z+dz)*2)!=BLOCK_AIR) clear=0;
        if (clear) { *bx=x;*bz=z;return; }
    }
    CHECK(0);
}

/* Стоя на поверхности: под ногами и за спиной геометрия не отправляется. */
static void test_surface(Buffer *a,Buffer *b) {
    rbx_world_build(RBX_WORLD_SEED);
    warm(.5f,.5f);
    int bx=0,bz=0;
    find_spot(&bx,&bz);
    int h=rbx_terrain_height(bx,bz);
    Pose stand={(float)bx+.5f,(float)h+1.6f,(float)bz+.5f,0,0};
    warm(stand.x,stand.z);
    int culled=render(a,2,&stand,0),all=render(b,2,&stand,1);
    same_image(a,b);
    CHECK(all>2000);
    CHECK(culled*3<all);              /* больше двух третей геометрии отсекается */
    CHECK(coverage(2,&stand)>WIDTH*HEIGHT/4);
    /* взгляд вниз: земля под ногами видна, а кадр всё ещё совпадает */
    Pose down={stand.x,stand.y,stand.z,0,-1.0f};
    int down_culled=render(a,2,&down,0),down_all=render(b,2,&down,1);
    same_image(a,b);
    CHECK(down_culled<down_all);
    CHECK(coverage(2,&down)>WIDTH*HEIGHT/4);
    printf("surface: culled=%d of %d triangles, looking down %d of %d\n",culled,all,down_culled,down_all);
    puts("PASS отсечение на поверхности: кадр совпадает с полным обходом, геометрии уходит в три раза меньше");
}

/* Шахта под ногами: её стены есть в мешах, но в кадр сверху не отправляются. */
static void test_shaft(Buffer *a,Buffer *b) {
    rbx_world_build(RBX_WORLD_SEED);
    warm(.5f,.5f);
    int bx=0,bz=0;
    find_spot(&bx,&bz);
    int h=rbx_terrain_height(bx,bz);
    Pose mouth={(float)bx+1.0f,(float)h+1.6f,(float)bz+1.0f,0,0};
    warm(mouth.x,mouth.z);
    int before_culled=render(a,2,&mouth,0),before_all=render(b,2,&mouth,1);
    same_image(a,b);
    /* кадр «вниз» до шахты: после раскопки изображение обязано измениться */
    Pose down={mouth.x,mouth.y,mouth.z,0,-1.2f};
    Buffer before_down=make_buffer();
    render(&before_down,2,&down,0);
    /* шахта 2x2 блока вниз прямо под камерой, не заходя в коренную защиту */
    int bottom=h-10;
    if (bottom<4) bottom=4;
    for (int y=h;y>=bottom;y--)
        for (int z=0;z<2;z++) for (int x=0;x<2;x++) for (int half=0;half<2;half++)
            CHECK(rbx_world_set((bx+x)*2,y*2+half,(bz+z)*2,BLOCK_AIR));
    warm(mouth.x,mouth.z);
    CHECK(h-bottom>=6);
    CHECK(rbx_world_cell(bx*2,(bottom+4)*2,bz*2)==BLOCK_AIR);
    int after_culled=render(a,2,&mouth,0),after_all=render(b,2,&mouth,1);
    same_image(a,b);
    CHECK(after_all>before_all+10);        /* стены шахты попали в меши */
    CHECK(after_culled<=before_culled+8);  /* но сверху они не рисуются */
    /* взгляд в шахту: провал виден, стены рисуются, кадр совпадает с полным */
    int down_culled=render(a,2,&down,0),down_all=render(b,2,&down,1);
    same_image(a,b);
    CHECK(down_culled<down_all);
    CHECK(memcmp(a->pixels,before_down.pixels,(size_t)a->stride*a->height*sizeof(*a->pixels))!=0);
    CHECK(coverage(2,&down)>100);
    free(before_down.pixels);
    /* камера внутри шахты: видно стены рядом, а не всю толщу породы */
    Pose inside={mouth.x,(float)bottom+3.5f,mouth.z,.7853981634f,0};
    int in_culled=render(a,2,&inside,0),in_all=render(b,2,&inside,1);
    same_image(a,b);
    CHECK(in_culled<in_all);
    CHECK(coverage(2,&inside)>100);
    printf("shaft: before %d/%d, after %d/%d, down %d/%d, inside %d/%d\n",
           before_culled,before_all,after_culled,after_all,down_culled,down_all,in_culled,in_all);
    puts("PASS шахта под ногами: стены есть в мешах, но из положения сверху в кадр не отправляются");
}

int main(void) {
    dt=1.0/60;
    CHECK(rbx_materials_load(host_asset_manager("game/assets")));
    Buffer a=make_buffer(),b=make_buffer();
    test_surface(&a,&b);
    test_shaft(&a,&b);
    free(a.pixels);free(b.pixels);
    rbx3d_shutdown();
    return 0;
}
