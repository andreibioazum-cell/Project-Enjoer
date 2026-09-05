/* Read real PNG assets once, then build palette mipmaps and cached lighting. */
#include "rbx_render_internal.h"
#include <limits.h>
#include <stdio.h>
enum { GRASS_TOP,GRASS_SIDE,DIRT,STONE,SAND,WATER,BARK,RINGS,LEAVES,MATERIALS };
static RbxMaterial materials[MATERIALS];
static int initialized;
static int color_index(RbxMaterial *m,uint32_t color) {
    for (int i=0;i<m->colors;i++) if (m->palette[i]==color) return i;
    if (m->colors<PALETTE_SIZE) {m->palette[m->colors]=color;return m->colors++;}
    int best=0,distance=INT_MAX;
    for (int i=0;i<m->colors;i++) {
        int dr=(int)((color>>16)&255)-(int)((m->palette[i]>>16)&255);
        int dg=(int)((color>>8)&255)-(int)((m->palette[i]>>8)&255);
        int db=(int)(color&255)-(int)(m->palette[i]&255);
        int d=dr*dr+dg*dg+db*db;
        if (d<distance) {distance=d;best=i;}
    }
    return best;
}
static void build_mips(RbxMaterial *m) {
    int source=0,dest=TEXTURE_SIZE*TEXTURE_SIZE;
    for (int size=TEXTURE_SIZE;size>1;size/=2) {
        int next=size/2;
        for (int y=0;y<next;y++) for (int x=0;x<next;x++) {
            int r=0,g=0,b=0;
            for (int dy=0;dy<2;dy++) for (int dx=0;dx<2;dx++) {
                uint32_t c=m->palette[m->mip[source+(y*2+dy)*size+x*2+dx]];
                r+=(c>>16)&255;g+=(c>>8)&255;b+=c&255;
            }
            uint32_t color=0xff000000u|((uint32_t)(r/4)<<16)|((uint32_t)(g/4)<<8)|(uint32_t)(b/4);
            m->mip[dest+y*next+x]=(unsigned char)color_index(m,color);
        }
        source=dest;dest+=next*next;
    }
}
int rbx_materials_load(AAssetManager *assets) {
    if (initialized) return 1;
    static const char *names[MATERIALS]={"grass_top","grass_side","dirt","stone","sand","water","log_side","log_top","leaves"};
    for (int t=0;t<MATERIALS;t++) {
        char path[80];snprintf(path,sizeof(path),"textures/%s.png",names[t]);
        RbxMaterial *m=&materials[t];
        image_free(&m->image);memset(m,0,sizeof(*m));
        if (!image_load(assets,path,&m->image)) {app_fail("Не удалось загрузить %s",path);return 0;}
        if (m->image.width!=TEXTURE_SIZE || m->image.height!=TEXTURE_SIZE) {
            app_fail("%s: требуется PNG %d×%d",path,TEXTURE_SIZE,TEXTURE_SIZE);return 0;
        }
        for (int i=0;i<TEXTURE_SIZE*TEXTURE_SIZE;i++) {
            uint32_t c=m->image.pixels[i]; /* framebuffer RGBA to ARGB */
            uint32_t color=0xff000000u|((c&255)<<16)|(c&0xff00)|((c>>16)&255);
            m->mip[i]=(unsigned char)color_index(m,color);
        }
        build_mips(m);
    }
    initialized=1;return 1;
}
static int texture(int block,int face) {
    switch (block) {
        case BLOCK_GRASS:return face==0 ? GRASS_TOP : face==1 ? DIRT : GRASS_SIDE;
        case BLOCK_DIRT:return DIRT;
        case BLOCK_SAND:return SAND;
        case BLOCK_WATER:return WATER;
        case BLOCK_LOG:return face<2 ? RINGS : BARK;
        case BLOCK_LEAVES:return LEAVES;
        default:return STONE;
    }
}
RbxMaterial *rbx_material(int block,int face) { return initialized ? &materials[texture(block,face)] : NULL; }
const Image *rbx_material_icon(int block) { return initialized ? &materials[texture(block,block==BLOCK_LOG ? 2 : 0)].image : NULL; }
const uint32_t *rbx_material_shades(RbxMaterial *m,int face,uint32_t fog) {
    static const float light[6]={.96f,.52f,.695f,.52f,.68f,.52f};
    if (m->fog_color!=fog) { m->fog_color=fog;m->ready=0; }
    if (!(m->ready&(1u<<face))) {
        int fr=fog&255,fg=(fog>>8)&255,fb=(fog>>16)&255;
        for (int level=0;level<FOG_LEVELS;level++) for (int i=0;i<PALETTE_SIZE;i++) {
            uint32_t c=m->palette[i];
            int r=(int)(((c>>16)&255)*light[face]),g=(int)(((c>>8)&255)*light[face]),b=(int)((c&255)*light[face]);
            /* Signed differences preserve channel bounds even toward darker fog. */
            r+=(fr-r)*level/(FOG_LEVELS-1);g+=(fg-g)*level/(FOG_LEVELS-1);b+=(fb-b)*level/(FOG_LEVELS-1);
            m->shades[face][level*PALETTE_SIZE+i]=0xff000000u|(uint32_t)r|((uint32_t)g<<8)|((uint32_t)b<<16);
        }
        m->ready|=1u<<face;
    }
    return m->shades[face];
}
