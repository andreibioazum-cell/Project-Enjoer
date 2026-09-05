/* Sparse parent-block patches: eight independently editable half-block cells.
 * Indexed both by parent position and chunk. Edits survive cache eviction. */
#define _POSIX_C_SOURCE 200809L
#include "rbx_world_internal.h"
#include <stdio.h>
#include <unistd.h>

enum { MAX_EDITS=65536 };
typedef struct { int cx,cz,head; } Bucket;
static RbxEdit *edits;
static int *table;
static Bucket *buckets;
static int count,capacity,table_cap,dirty;
static uint32_t seed;

int rbx_floor_div(int x,int d) { return x/d-(x<0 && x%d!=0); }
static uint32_t hash(int x,int y,int z) {
    uint32_t h=(uint32_t)x*0x8da6b343u ^ (uint32_t)y*0xd8163841u ^ (uint32_t)z*0xcb1ab31fu;
    h^=h>>16; h*=0x7feb352du; return h^(h>>15);
}
static Bucket *bucket(int cx,int cz) {
    if (!table_cap) return NULL;
    unsigned i=hash(cx,0,cz)&(table_cap-1);
    while (buckets[i].head && (buckets[i].cx!=cx || buckets[i].cz!=cz)) i=(i+1)&(table_cap-1);
    return &buckets[i];
}
static void index_edit(int index) {
    RbxEdit *e=&edits[index];
    unsigned i=hash(e->x,e->y,e->z)&(table_cap-1);
    while (table[i]) i=(i+1)&(table_cap-1);
    table[i]=index+1;
    int cx=rbx_floor_div(e->x,CHUNK_SIZE),cz=rbx_floor_div(e->z,CHUNK_SIZE);
    Bucket *b=bucket(cx,cz);
    e->next=b->head; b->cx=cx; b->cz=cz; b->head=index+1;
}
static int reserve(int needed) {
    if (needed>MAX_EDITS) return 0;
    if (needed>capacity) {
        int n=capacity ? capacity*2 : 128;
        RbxEdit *p=realloc(edits,(size_t)n*sizeof(*p));
        if (!p) return 0;
        edits=p; capacity=n;
    }
    if (needed*2>=table_cap) {
        int n=table_cap ? table_cap*2 : 256;
        int *nt=calloc((size_t)n,sizeof(*nt));
        Bucket *nb=calloc((size_t)n,sizeof(*nb));
        if (!nt || !nb) { free(nt);free(nb);return 0; }
        free(table);free(buckets);table=nt;buckets=nb;table_cap=n;
        for (int i=0;i<count;i++) index_edit(i);
    }
    return 1;
}
void rbx_edits_reset(uint32_t value) {
    count=dirty=0; seed=value;
    if (table) memset(table,0,(size_t)table_cap*sizeof(*table));
    if (buckets) memset(buckets,0,(size_t)table_cap*sizeof(*buckets));
}
const RbxEdit *rbx_edit_find(int x,int y,int z) {
    if (!count) return NULL;
    unsigned i=hash(x,y,z)&(table_cap-1);
    while (table[i]) {
        RbxEdit *e=&edits[table[i]-1];
        if (e->x==x && e->y==y && e->z==z) return e;
        i=(i+1)&(table_cap-1);
    }
    return NULL;
}
const RbxEdit *rbx_edit_first(int cx,int cz) {
    Bucket *b=bucket(cx,cz);
    return b && b->head ? &edits[b->head-1] : NULL;
}
const RbxEdit *rbx_edit_next(const RbxEdit *e) { return e && e->next ? &edits[e->next-1] : NULL; }
int rbx_edit_set(int sx,int sy,int sz,int block,int original) {
    int x=rbx_floor_div(sx,2),y=rbx_floor_div(sy,2),z=rbx_floor_div(sz,2);
    int part=sx-x*2+(sy-y*2)*2+(sz-z*2)*4;
    RbxEdit *e=(RbxEdit *)rbx_edit_find(x,y,z);
    if (!e) {
        if (block==original) return 0;
        if (!reserve(count+1)) return 0;
        e=&edits[count]; *e=(RbxEdit){.x=x,.y=y,.z=z};
        memset(e->cells,original,8); index_edit(count++);
    }
    if (e->cells[part]==block) return 0;
    e->cells[part]=(unsigned char)block; dirty=1;
    return 1;
}
int rbx_edits_count(void) { return count; }
int rbx_edits_dirty(void) { return dirty; }
static void put32(unsigned char *p,uint32_t n) { for (int i=0;i<4;i++) p[i]=(unsigned char)(n>>(i*8)); }
static uint32_t get32(const unsigned char *p) { return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint32_t checksum(const unsigned char *p,size_t n) {
    uint32_t h=2166136261u;
    for (size_t i=0;i<n;i++) h=(h^p[i])*16777619u;
    return h;
}
int rbx_edits_save(void) {
    if (!dirty) return 1;
    char path[600],temp[600];
    if (!app_save_path(path,sizeof(path),"world.edits") || !app_save_path(temp,sizeof(temp),"world.edits.tmp")) return 0;
    size_t length=20+(size_t)count*20;
    unsigned char *bytes=malloc(length);
    if (!bytes) return 0;
    memcpy(bytes,"EJVOX01\0",8); put32(bytes+8,seed); put32(bytes+12,(uint32_t)count);
    for (int i=0;i<count;i++) {
        unsigned char *p=bytes+20+i*20;
        put32(p,(uint32_t)edits[i].x);put32(p+4,(uint32_t)edits[i].y);put32(p+8,(uint32_t)edits[i].z);
        memcpy(p+12,edits[i].cells,8);
    }
    put32(bytes+16,checksum(bytes+20,length-20));
    FILE *f=fopen(temp,"wb");
    if (!f) { free(bytes);return 0; }
    int ok=fwrite(bytes,1,length,f)==length && fflush(f)==0;
    if (ok) ok=fsync(fileno(f))==0;
    if (fclose(f)!=0) ok=0;
    free(bytes);
    if (ok) ok=rename(temp,path)==0;
    if (!ok) { remove(temp);app_log_error("Could not save world edits");return 0; }
    dirty=0; return 1;
}
int rbx_edits_load(void) {
    char path[600];
    if (!app_save_path(path,sizeof(path),"world.edits")) return 0;
    FILE *f=fopen(path,"rb"); if (!f) return 0;
    if (fseek(f,0,SEEK_END)!=0) { fclose(f);return 0; }
    long length=ftell(f); rewind(f);
    if (length<20 || length>20+MAX_EDITS*20) { fclose(f);return 0; }
    unsigned char *bytes=malloc((size_t)length);
    if (!bytes) { fclose(f);return 0; }
    int ok=fread(bytes,1,(size_t)length,f)==(size_t)length;
    fclose(f);
    uint32_t n=ok ? get32(bytes+12) : 0;
    ok=ok && !memcmp(bytes,"EJVOX01\0",8) && get32(bytes+8)==seed && n<=MAX_EDITS &&
       20+n*20==(uint32_t)length && get32(bytes+16)==checksum(bytes+20,(size_t)length-20);
    for (uint32_t i=0;ok && i<n;i++) {
        const unsigned char *p=bytes+20+i*20;
        int32_t x=(int32_t)get32(p),y=(int32_t)get32(p+4),z=(int32_t)get32(p+8);
        if (x < -10000000 || x>10000000 || z < -10000000 || z>10000000 || y<1 || y>=WORLD_HEIGHT) ok=0;
        for (int c=0;c<8;c++) if (p[12+c]>=BLOCK_COUNT) ok=0;
    }
    if (ok) {
        rbx_edits_reset(seed);
        for (uint32_t i=0;i<n;i++) {
            const unsigned char *p=bytes+20+i*20;
            int x=(int32_t)get32(p),y=(int32_t)get32(p+4),z=(int32_t)get32(p+8);
            if (rbx_edit_find(x,y,z) || !reserve(count+1)) { ok=0; break; }
            RbxEdit *e=&edits[count];*e=(RbxEdit){.x=x,.y=y,.z=z};memcpy(e->cells,p+12,8);
            index_edit(count++);
        }
    }
    free(bytes);
    if (!ok) { rbx_edits_reset(seed);app_log_error("Ignored invalid world edit file"); }
    dirty=0; return ok;
}
