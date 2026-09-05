/* Растекание воды: уровни потока, очередь пересчёта и бюджет на кадр.
 *
 * Правила те же, что в играх с вёдрами: источник (природная вода и поставленный
 * игрокой блок воды) имеет уровень 0 и не иссякает. Соседняя по горизонтали
 * ячейка получает уровень «минимум у питающих соседей + 1», ячейка под водой —
 * уровень соседа сверху (падающая струя не создаёт новый источник). Когда
 * питания не остаётся, ячейка высыхает, и соседи пересчитываются следом.
 * Пересчёт — релаксация к одному и тому же неподвижному состоянию, поэтому
 * порядок обхода очереди не влияет на итог, а работа размазана по кадрам. */
#include "rbx_world_internal.h"

enum { QUEUE_CAPACITY=16384, QUEUE_MASK=QUEUE_CAPACITY-1,
       MAX_CELLS_PER_FRAME=1024, MAX_WORK_PER_RUN=200000, MAX_RESCANS=16 };
typedef struct { int x,y,z; } Cell;
static Cell queue[QUEUE_CAPACITY];
static int head,tail,dropped,changed,run_work,rescans;
/* Габарит всего, что попадало в очередь: по нему делается повторный проход,
 * если очередь однажды переполнилась и часть ячеек не была пересчитана. */
static int box_min[3],box_max[3],box_valid;

static int queued_count(void) { return (tail-head)&QUEUE_MASK; }
int rbx_water_pending(void) { return queued_count(); }
int rbx_water_stats(int *queued,int *total_changed) {
    if (queued) *queued=queued_count();
    if (total_changed) *total_changed=changed;
    return dropped;
}
void rbx_water_reset(void) {
    head=tail=dropped=changed=run_work=rescans=0;
    box_valid=0;
}

/* Уровень воды в ячейке: -1 для всего, что не является водой. */
int rbx_water_level(int sx,int sy,int sz) {
    if (rbx_world_cell(sx,sy,sz)!=BLOCK_WATER) return -1;
    int value=rbx_edit_cell_value(sx,sy,sz);
    /* Природная вода живёт в чанке и всегда остаётся источником. */
    return value<0 ? 0 : RBX_CELL_LEVEL(value);
}
static int is_source(int sx,int sy,int sz) {
    int value=rbx_edit_cell_value(sx,sy,sz);
    return value<0 || RBX_CELL_LEVEL(value)==0;
}
static void push(int sx,int sy,int sz) {
    if (sy<2 || sy>=WORLD_HEIGHT*2) return;
    /* Твёрдые ячейки вода никогда не меняет — не занимаем ими очередь. */
    int block=rbx_world_cell(sx,sy,sz);
    if (block!=BLOCK_AIR && block!=BLOCK_WATER) return;
    const int coords[3]={sx,sy,sz};
    if (!box_valid) {
        box_valid=1;
        for (int i=0;i<3;i++) { box_min[i]=coords[i];box_max[i]=coords[i]; }
    } else {
        for (int i=0;i<3;i++) {
            if (coords[i]<box_min[i]) box_min[i]=coords[i];
            if (coords[i]>box_max[i]) box_max[i]=coords[i];
        }
    }
    if (queued_count()==QUEUE_CAPACITY-1) { dropped++;return; }
    queue[tail]=(Cell){sx,sy,sz};
    tail=(tail+1)&QUEUE_MASK;
}
/* Разреженный повторный проход по габариту: гарантирует, что после
 * переполнения очереди вода всё равно приходит к неподвижному состоянию. */
static void rescan_box(void) {
    if (!box_valid) { dropped=0;return; }
    for (int z=box_min[2];z<=box_max[2];z+=2)
        for (int y=box_min[1];y<=box_max[1];y+=2)
            for (int x=box_min[0];x<=box_max[0];x+=2) push(x,y,z);
    dropped=0;
}
void rbx_water_touch(int sx,int sy,int sz) {
    push(sx,sy,sz);
    push(sx+1,sy,sz);push(sx-1,sy,sz);
    push(sx,sy,sz+1);push(sx,sy,sz-1);
    push(sx,sy+1,sz);push(sx,sy-1,sz);
}
/* Желаемое содержимое ячейки по текущему состоянию шести соседей. */
static int desired(int sx,int sy,int sz,int *level) {
    int self=rbx_world_cell(sx,sy,sz);
    if (self!=BLOCK_AIR && self!=BLOCK_WATER) return self;
    if (rbx_world_cell(sx,sy+1,sz)==BLOCK_WATER) {
        int above=rbx_water_level(sx,sy+1,sz);
        *level=above<1 ? 1 : above;
        return BLOCK_WATER;
    }
    static const int dx[4]={1,-1,0,0},dz[4]={0,0,1,-1};
    int best=WATER_MAX_FLOW+1;
    for (int i=0;i<4;i++) {
        int x=sx+dx[i],z=sz+dz[i];
        if (rbx_world_cell(x,sy,z)!=BLOCK_WATER) continue;
        int candidate=rbx_water_level(x,sy,z)+1;
        if (candidate<best) best=candidate;
    }
    if (best<=WATER_MAX_FLOW) { *level=best;return BLOCK_WATER; }
    return BLOCK_AIR;
}
static void evaluate(int sx,int sy,int sz) {
    int level=0;
    int want=desired(sx,sy,sz,&level);
    int have=rbx_world_cell(sx,sy,sz);
    if (have==BLOCK_WATER && is_source(sx,sy,sz)) return; /* источник не иссякает */
    if (want==have) {
        /* Блок тот же: выравниваем только уровень потока. */
        if (have==BLOCK_WATER && rbx_water_level(sx,sy,sz)!=level &&
            rbx_world_set_value(sx,sy,sz,RBX_CELL_VALUE(BLOCK_WATER,level))) {
            changed++;
            rbx_water_touch(sx,sy,sz);
        }
        return;
    }
    if (!rbx_world_set_value(sx,sy,sz,want==BLOCK_WATER ? RBX_CELL_VALUE(BLOCK_WATER,level) : BLOCK_AIR)) return;
    changed++;
    rbx_water_touch(sx,sy,sz);
}
int rbx_water_update(double budget) {
    if (head==tail) {
        run_work=0;
        if (dropped&&rescans<MAX_RESCANS) { rescans++;rescan_box(); }
        else if (dropped) { dropped=0;box_valid=0; }
        if (head==tail) return 0;
    }
    double start=app_now();
    int work=0;
    while (head!=tail) {
        Cell cell=queue[head];
        head=(head+1)&QUEUE_MASK;
        evaluate(cell.x,cell.y,cell.z);
        if (++work>=MAX_CELLS_PER_FRAME) break;
        if (budget>0 && (work&31)==0 && app_now()-start>budget) break;
    }
    /* Предохранитель: если релаксация не сходится, очередь сбрасывается. */
    run_work+=work;
    if (run_work>MAX_WORK_PER_RUN) rbx_water_reset();
    else if (!queued_count()&&!dropped) rescans=0;   /* чистая сходимость */
    return work;
}
