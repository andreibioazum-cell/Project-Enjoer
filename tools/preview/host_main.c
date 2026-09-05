/* Превью-сервер 3D-плейса для ПК: тот же C-код, окно — браузер.
 * Кадры — сырой RGBA (быстрее PNG), касания и WASD приходят со страницы. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include "rbx/rbx.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

void host_key_enter(void);

static Buffer frame;
static uint64_t last_ns = 0;

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void tick(void) {
    uint64_t now = mono_ns();
    double d = last_ns ? (double)(now - last_ns) / 1e9 : 0.016;
    last_ns = now;
    if (d < 0) d = 0;
    if (d > 0.05) d = 0.05;
    dt = d;
    update();
}

static void render_frame(void) {
    tick();
    if (!ds_graphics_begin_frame(&frame)) return;
    draw(&frame);
    ds_graphics_end_frame();
}

static int read_request(int fd, char *buf, int cap, int *body_off, int *body_len) {
    int total = 0;
    int header_end = -1;
    while (total < cap - 1) {
        int n = (int)recv(fd, buf + total, (size_t)(cap - 1 - total), 0);
        if (n <= 0) return -1;
        total += n;
        buf[total] = '\0';
        char *e = strstr(buf, "\r\n\r\n");
        if (e) {
            header_end = (int)(e - buf) + 4;
            break;
        }
    }
    if (header_end < 0) return -1;
    int clen = 0;
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl && cl < buf + header_end) clen = atoi(cl + 15);
    while (total < header_end + clen && total < cap - 1) {
        int n = (int)recv(fd, buf + total, (size_t)(header_end + clen - total), 0);
        if (n <= 0) break;
        total += n;
    }
    *body_off = header_end;
    *body_len = total - header_end;
    return total;
}

static void send_all(int fd, const void *data, size_t len) {
    const char *p = (const char *)data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return;
        p += n;
        len -= (size_t)n;
    }
}

static void http_head(int fd, int code, const char *ctype, int len, int cache) {
    char h[256];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                     "Cache-Control: %s\r\nConnection: close\r\n\r\n",
                     code, ctype, len, cache ? "max-age=3600" : "no-store");
    send_all(fd, h, (size_t)n);
}

static void url_decode(char *s) {
    char *o = s;
    while (*s) {
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else *o++ = *s++;
    }
    *o = '\0';
}

static void form_get(const char *body, int blen, const char *key, char *out, int outsz) {
    out[0] = '\0';
    if (!body || blen <= 0) return;
    char tmp[2048];
    int n = blen < (int)sizeof(tmp) - 1 ? blen : (int)sizeof(tmp) - 1;
    memcpy(tmp, body, (size_t)n);
    tmp[n] = '\0';
    size_t kl = strlen(key);
    char *p = tmp;
    while (p && *p) {
        char *amp = strchr(p, '&');
        if (amp) *amp = '\0';
        char *eq = strchr(p, '=');
        if (eq && (size_t)(eq - p) == kl && strncmp(p, key, kl) == 0) {
            url_decode(eq + 1);
            snprintf(out, (size_t)outsz, "%s", eq + 1);
            return;
        }
        p = amp ? amp + 1 : NULL;
    }
}

static void handle_event(const char *body, int blen) {
    char t[16], k[16], s[1024], xs[32], ys[32];
    form_get(body, blen, "t", t, sizeof(t));
    if (strcmp(t, "down") == 0 || strcmp(t, "move") == 0 || strcmp(t, "up") == 0) {
        form_get(body, blen, "x", xs, sizeof(xs));
        form_get(body, blen, "y", ys, sizeof(ys));
        int action = strcmp(t, "down") == 0 ? 0 : strcmp(t, "up") == 0 ? 1 : 2;
        touch((float)atof(xs), (float)atof(ys), action, 0);
    } else if (strcmp(t, "key") == 0) {
        form_get(body, blen, "k", k, sizeof(k));
        char ds[8];
        form_get(body, blen, "d", ds, sizeof(ds));
        int down = ds[0] ? atoi(ds) : 1;
        if (strcmp(k, "enter") == 0) host_key_enter();
        else if (strcmp(k, "backspace") == 0) keyboard_backspace();
        else rbx_key(k, down);
    } else if (strcmp(t, "text") == 0) {
        form_get(body, blen, "s", s, sizeof(s));
        if (s[0]) keyboard_type(s);
    }
}

static const char *INDEX_HTML =
"<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Enjoer — 3D плейс</title>"
"<style>"
"html,body{margin:0;height:100%;background:#111216;color:#eceff1;"
"font-family:system-ui,'Segoe UI',sans-serif;}"
".wrap{height:100%;display:flex;flex-direction:column;align-items:center;"
"justify-content:center;gap:14px;padding:16px;box-sizing:border-box;}"
"h1{font-size:15px;font-weight:600;margin:0;letter-spacing:.3px;}"
"h1 b{color:#e2231a;}"
".phone{border-radius:28px;overflow:hidden;box-shadow:0 24px 80px #0009,"
"0 0 0 1px #ffffff14;height:min(92vh,900px);aspect-ratio:var(--ar);background:#000;}"
"canvas{width:100%;height:100%;display:block;touch-action:none;cursor:grab;}"
".hint{font-size:12px;color:#90a4ae;text-align:center;max-width:560px;line-height:1.5;}"
"kbd{background:#1c2733;border:1px solid #2c3947;border-radius:5px;padding:1px 6px;font-size:11px;}"
"</style></head><body><div class=\"wrap\">"
"<h1><b>Enjoer</b> — заходишь в игру, сразу 3D</h1>"
"<div class=\"phone\" style=\"--ar:0.5\"><canvas id=\"c\" tabindex=\"0\"></canvas></div>"
"<div class=\"hint\"><kbd>W</kbd><kbd>A</kbd><kbd>S</kbd><kbd>D</kbd> — ходить, "
"<kbd>пробел</kbd> — прыжок, стрелки или перетаскивание — камера. "
"Слева джойстик, справа прыжок. Собери монеты на радужном обби.</div>"
"</div><script>"
"const cv=document.getElementById('c'),ctx=cv.getContext('2d');"
"let W=400,H=800,img=null;"
"fetch('/info').then(r=>r.json()).then(j=>{W=j.w;H=j.h;cv.width=W;cv.height=H;"
"img=ctx.createImageData(W,H);loop();});"
"async function loop(){try{const r=await fetch('/frame.rgba',{cache:'no-store'});"
"const buf=new Uint8ClampedArray(await r.arrayBuffer());"
"if(img&&buf.length>=W*H*4){img.data.set(buf.subarray(0,W*H*4));ctx.putImageData(img,0,0);}"
"}catch(e){}requestAnimationFrame(loop);}"
"function ev(body){fetch('/event',{method:'POST',body:new URLSearchParams(body)});}"
"function xy(e){const r=cv.getBoundingClientRect();"
"return [Math.round((e.clientX-r.left)*W/r.width),Math.round((e.clientY-r.top)*H/r.height)];}"
"let down=false;"
"cv.addEventListener('pointerdown',e=>{cv.focus();down=true;cv.setPointerCapture(e.pointerId);"
"const[x,y]=xy(e);ev({t:'down',x,y});e.preventDefault();});"
"cv.addEventListener('pointermove',e=>{if(!down)return;const[x,y]=xy(e);ev({t:'move',x,y});});"
"cv.addEventListener('pointerup',e=>{down=false;const[x,y]=xy(e);ev({t:'up',x,y});});"
"cv.addEventListener('pointercancel',e=>{down=false;const[x,y]=xy(e);ev({t:'up',x,y});});"
"const keys=new Set(['w','a','s','d','W','A','S','D',' ','ArrowLeft','ArrowRight','ArrowUp','ArrowDown']);"
"window.addEventListener('keydown',e=>{"
"if(e.repeat||e.ctrlKey||e.metaKey||e.altKey)return;"
"if(keys.has(e.key)){const k=e.key===' '?'space':e.key;ev({t:'key',k,d:'1'});e.preventDefault();return;}"
"if(e.key==='Enter'){ev({t:'key',k:'enter'});e.preventDefault();}});"
"window.addEventListener('keyup',e=>{"
"if(keys.has(e.key)){const k=e.key===' '?'space':e.key;ev({t:'key',k,d:'0'});e.preventDefault();}});"
"</script></body></html>";

int main(int argc, char **argv) {
    int port = 8090;
    int w = 400, h = 800;
    const char *assets = "game/assets";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--w") && i + 1 < argc) w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--h") && i + 1 < argc) h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--assets") && i + 1 < argc) assets = argv[++i];
    }
    signal(SIGPIPE, SIG_IGN);

    screen_w = w;
    screen_h = h;
    frame.pixels = (uint32_t *)malloc((size_t)w * h * 4);
    frame.width = w;
    frame.height = h;
    frame.stride = w;
    if (!frame.pixels) { fprintf(stderr, "OOM\n"); return 1; }

    AAssetManager *am = host_asset_manager(assets);
    if (!ds_graphics_init(am)) { fprintf(stderr, "graphics init failed\n"); return 1; }
    init(am);
    render_frame();

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    listen(srv, 16);
    fprintf(stderr, "Enjoer 3D preview: http://0.0.0.0:%d (%dx%d, assets=%s)\n",
            port, w, h, assets);

    static char reqbuf[16384];
    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int body_off = 0, body_len = 0;
        if (read_request(fd, reqbuf, sizeof(reqbuf), &body_off, &body_len) <= 0) {
            close(fd);
            continue;
        }
        char method[8] = {0}, path[512] = {0};
        sscanf(reqbuf, "%7s %511s", method, path);

        if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
            http_head(fd, 200, "text/html; charset=utf-8", (int)strlen(INDEX_HTML), 0);
            send_all(fd, INDEX_HTML, strlen(INDEX_HTML));
        } else if (strcmp(method, "GET") == 0 && strcmp(path, "/info") == 0) {
            char info[64];
            int n = snprintf(info, sizeof(info), "{\"w\":%d,\"h\":%d}", w, h);
            http_head(fd, 200, "application/json", n, 0);
            send_all(fd, info, (size_t)n);
        } else if (strcmp(method, "GET") == 0 && strcmp(path, "/frame.rgba") == 0) {
            render_frame();
            int bytes = frame.width * frame.height * 4;
            http_head(fd, 200, "application/octet-stream", bytes, 0);
            send_all(fd, frame.pixels, (size_t)bytes);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/event") == 0) {
            handle_event(reqbuf + body_off, body_len);
            http_head(fd, 200, "text/plain", 2, 0);
            send_all(fd, "ok", 2);
        } else {
            http_head(fd, 404, "text/plain", 3, 0);
            send_all(fd, "404", 3);
        }
        close(fd);
    }
    return 0;
}
