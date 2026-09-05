/* FPS counts actual frame intervals. Render quality uses measured CPU cost,
 * never network/vsync delays, with hysteresis to avoid resolution flicker. */
#include "rbx_internal.h"
static double frame_seconds,render_seconds;
static int frame_count,render_count,fast_windows,render_scale=1,base_scale=1,last_w,last_h;
static float fps=-1;
void rbx_perf_reset(void) {
    frame_seconds=render_seconds=0;frame_count=render_count=fast_windows=last_w=last_h=0;fps=-1;
}
void rbx_perf_frame(double interval) {
    if (!isfinite(interval) || interval<=0) return;
    frame_seconds+=interval;frame_count++;
    if (frame_seconds>=.5) {fps=(float)(frame_count/frame_seconds);frame_seconds=0;frame_count=0;}
}
float rbx_fps(void) { return fps; }
int rbx_render_scale(int width,int height) {
    if (width!=last_w || height!=last_h) {
        last_w=width;last_h=height;base_scale=1;
        while (base_scale<4 && (double)(width/base_scale)*(height/base_scale)>1000000) base_scale++;
        render_scale=base_scale;render_count=fast_windows=0;render_seconds=0;
    }
    return render_scale;
}
void rbx_render_time(double elapsed) {
    if (!isfinite(elapsed) || elapsed<=0) return;
    render_seconds+=elapsed;render_count++;
    if (render_count<30) return;
    double average=render_seconds/render_count;
    if (average>.019 && render_scale<4) {render_scale++;fast_windows=0;}
    else if (average<.007 && render_scale>base_scale) {
        if (++fast_windows>=4) {render_scale--;fast_windows=0;}
    } else fast_windows=0;
    render_count=0;render_seconds=0;
}
