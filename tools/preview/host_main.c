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
#include <limits.h>
#include <sys/time.h>
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
    if (clen < 0 || clen > cap - 1 - header_end) return -1;
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
    char t[16], k[16], s[1024], xs[32], ys[32], ids[32];
    form_get(body, blen, "t", t, sizeof(t));
    if (strcmp(t, "down") == 0 || strcmp(t, "move") == 0 || strcmp(t, "up") == 0 || strcmp(t, "cancel-pointer") == 0) {
        form_get(body, blen, "x", xs, sizeof(xs));
        form_get(body, blen, "y", ys, sizeof(ys));
        int action = strcmp(t, "down") == 0 ? 0 : strcmp(t, "up") == 0 ? 1 : strcmp(t, "cancel-pointer") == 0 ? 4 : 2;
        form_get(body, blen, "id", ids, sizeof(ids));
        char *end;
        long id = ids[0] ? strtol(ids, &end, 10) : 0;
        if (id < 0 || id > INT_MAX || (ids[0] && *end)) return;
        touch((float)atof(xs), (float)atof(ys), action, (int)id);
    } else if (strcmp(t, "cancel") == 0) {
        rbx_cancel_input();
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

/* Пакет событий одного запроса применяется строго по порядку. */
static void handle_events(const char *body, int len) {
    while (len > 0) {
        const char *nl = (const char *)memchr(body, '\n', (size_t)len);
        int n = nl ? (int)(nl - body) : len;
        if (n > 0) handle_event(body, n);
        body += n; len -= n;
        if (len > 0) { body++; len--; }
    }
}

static const char *INDEX_HTML =
"<!doctype html>\n"
"<html lang=\"ru\"><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">\n"
"<title>Enjoer — блочный мир</title>\n"
"<style>\n"
"html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#111;overscroll-behavior:none;}\n"
".game{position:fixed;inset:0;display:grid;place-items:center;}\n"
"canvas{display:block;touch-action:none;user-select:none;outline:none;cursor:grab;}\n"
"canvas:active{cursor:grabbing;}\n"
"</style></head><body>\n"
"<main class=\"game\"><canvas id=\"c\" tabindex=\"0\" aria-label=\"Блочный мир. WASD — движение, пробел — прыжок, F — переключить полёт. Слева джойстик, справа камера.\"></canvas></main>\n"
"<script>\n"
"const cv=document.getElementById('c'),ctx=cv.getContext('2d');\n"
"let W=960,H=540,img=null;\n"
"function fit(){const s=Math.min(innerWidth/W,innerHeight/H);cv.style.width=W*s+'px';cv.style.height=H*s+'px';}\n"
"fetch('/info').then(r=>r.json()).then(async j=>{\n"
"  W=j.w;H=j.h;cv.width=W;cv.height=H;fit();\n"
"  await fetch('/event',{method:'POST',body:'t=cancel'});\n"
"  img=ctx.createImageData(W,H);loop();\n"
"});\n"
"async function loop(){\n"
"  try{\n"
"    if(!document.hidden){\n"
"      const r=await fetch('/frame.rgba',{cache:'no-store'});\n"
"      const buf=new Uint8ClampedArray(await r.arrayBuffer());\n"
"      if(img&&buf.length>=W*H*4){img.data.set(buf.subarray(0,W*H*4));ctx.putImageData(img,0,0);}\n"
"    }\n"
"  }catch(e){}\n"
"  requestAnimationFrame(loop);\n"
"}\n"
"// Порядок down/move/up сохраняется; лишние move не забивают сеть.\n"
"const queue=[];\n"
"let sending=false;\n"
"function ev(body){\n"
"  if(body.t==='move'){\n"
"    for(let i=queue.length-1;i>=0&&queue[i].t==='move';i--){\n"
"      if(queue[i].id===body.id){queue[i]=body;return;}\n"
"    }\n"
"  }\n"
"  queue.push(body);flushEvents();\n"
"}\n"
"async function flushEvents(){\n"
"  if(sending||!queue.length)return;\n"
"  sending=true;\n"
"  const batch=queue.splice(0,64);let retryDelay=16;\n"
"  try{\n"
"    await fetch('/event',{method:'POST',body:batch.map(b=>new URLSearchParams(b).toString()).join('\\n'),keepalive:true});\n"
"  }catch(e){queue.length=0;queue.push({t:'cancel'});retryDelay=250;}\n"
"  finally{sending=false;if(queue.length)setTimeout(flushEvents,retryDelay);}\n"
"}\n"
"function xy(e){\n"
"  const r=cv.getBoundingClientRect();\n"
"  return {x:Math.round((e.clientX-r.left)*W/r.width),y:Math.round((e.clientY-r.top)*H/r.height)};\n"
"}\n"
"const pointers=new Set(),pressed=new Map();\n"
"cv.addEventListener('pointerdown',e=>{\n"
"  if(e.pointerType==='mouse'&&e.button!==0)return;\n"
"  cv.focus({preventScroll:true});pointers.add(e.pointerId);cv.setPointerCapture(e.pointerId);\n"
"  ev({t:'down',id:e.pointerId,...xy(e)});e.preventDefault();\n"
"});\n"
"cv.addEventListener('pointermove',e=>{\n"
"  if(pointers.has(e.pointerId))ev({t:'move',id:e.pointerId,...xy(e)});\n"
"});\n"
"function releasePointer(e){\n"
"  if(!pointers.delete(e.pointerId))return;\n"
"  ev({t:'up',id:e.pointerId,...xy(e)});\n"
"}\n"
"cv.addEventListener('pointerup',releasePointer);\n"
"function cancelPointer(e){if(pointers.delete(e.pointerId))ev({t:'cancel-pointer',id:e.pointerId});}\n"
"cv.addEventListener('pointercancel',cancelPointer);\n"
"cv.addEventListener('lostpointercapture',cancelPointer);\n"
"cv.addEventListener('contextmenu',e=>e.preventDefault());\n"
"const keyMap={KeyW:'w',KeyA:'a',KeyS:'s',KeyD:'d',KeyF:'f',Space:'space',ShiftLeft:'Shift',ShiftRight:'Shift',ArrowLeft:'ArrowLeft',ArrowRight:'ArrowRight',ArrowUp:'ArrowUp',ArrowDown:'ArrowDown'};\n"
"window.addEventListener('keydown',e=>{\n"
"  const k=keyMap[e.code];\n"
"  if(!k||e.ctrlKey||e.metaKey||e.altKey)return;\n"
"  e.preventDefault();if(e.repeat||pressed.has(e.code))return;\n"
"  const held=[...pressed.values()].includes(k);pressed.set(e.code,k);\n"
"  if(!held)ev({t:'key',k,d:1});\n"
"});\n"
"window.addEventListener('keyup',e=>{\n"
"  const k=pressed.get(e.code);if(!k)return;\n"
"  pressed.delete(e.code);e.preventDefault();\n"
"  if(![...pressed.values()].includes(k))ev({t:'key',k,d:0});\n"
"});\n"
"function releaseAll(){\n"
"  const ids=[...pointers];pointers.clear();pressed.clear();queue.length=0;ev({t:'cancel'});\n"
"  for(const id of ids){if(cv.hasPointerCapture(id))cv.releasePointerCapture(id);}\n"
"}\n"
"window.addEventListener('blur',releaseAll);\n"
"window.addEventListener('resize',()=>{releaseAll();fit();});\n"
"document.addEventListener('visibilitychange',()=>{if(document.hidden)releaseAll();});\n"
"</script>\n"
"</body></html>\n";

int main(int argc, char **argv) {
    int port = 8090;
    int w = 960, h = 540;
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
        struct timeval timeout = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
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
            handle_events(reqbuf + body_off, body_len);
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
