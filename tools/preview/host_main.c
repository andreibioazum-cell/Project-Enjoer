/* Tiny HTTP transport for the C/C++ cube preview. The browser is only an
 * input surface; all pixels are produced by the native cube renderer. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "engine.h"
#include "dawn_cube.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static Buffer frame;
static uint64_t previous_ns;
static volatile sig_atomic_t running = 1;
const unsigned char *preview_bmp(const Buffer *buffer, size_t *length);

static void stop_server(int signal_number) { (void)signal_number; running = 0; }

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void render_frame(void) {
    const uint64_t now = monotonic_ns();
    dt = previous_ns ? (double)(now - previous_ns) / 1000000000.0 : 0.0;
    previous_ns = now;
    game_update();
    game_draw(&frame);
}

static int read_request(int fd, char *buffer, int capacity, int *body_offset, int *body_length) {
    int total = 0;
    int header_end = -1;
    while (total < capacity - 1) {
        const int n = (int)recv(fd, buffer + total, (size_t)(capacity - total - 1), 0);
        if (n <= 0) return -1;
        total += n;
        buffer[total] = '\0';
        char *end = strstr(buffer, "\r\n\r\n");
        if (end) { header_end = (int)(end - buffer) + 4; break; }
    }
    if (header_end < 0) return -1;
    int content_length = 0;
    char *header = strcasestr(buffer, "Content-Length:");
    if (header && header < buffer + header_end) content_length = atoi(header + 15);
    if (content_length < 0 || content_length > capacity - header_end - 1) return -1;
    while (total < header_end + content_length) {
        const int n = (int)recv(fd, buffer + total,
                                (size_t)(header_end + content_length - total), 0);
        if (n <= 0) return -1;
        total += n;
    }
    *body_offset = header_end;
    *body_length = content_length;
    return total;
}

static void send_all(int fd, const void *data, size_t length) {
    const char *bytes = (const char *)data;
    while (length) {
        const ssize_t sent = send(fd, bytes, length, 0);
        if (sent <= 0) return;
        bytes += sent;
        length -= (size_t)sent;
    }
}

static void http_head(int fd, int status, const char *type, size_t length) {
    char header[256];
    const int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, type, length);
    send_all(fd, header, (size_t)n);
}

static void url_decode(char *text) {
    char *out = text;
    for (; *text; ++text) {
        if (*text == '+') *out++ = ' ';
        else if (*text == '%' && text[1] && text[2]) {
            char hex[3] = {text[1], text[2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            text += 2;
        } else *out++ = *text;
    }
    *out = '\0';
}

static void form_value(const char *body, int length, const char *key,
                       char *value, size_t value_size) {
    value[0] = '\0';
    if (!body || length <= 0) return;
    char copy[2048];
    const int n = length < (int)sizeof(copy) - 1 ? length : (int)sizeof(copy) - 1;
    memcpy(copy, body, (size_t)n);
    copy[n] = '\0';
    for (char *part = copy; part && *part;) {
        char *next = strchr(part, '&');
        if (next) *next = '\0';
        char *equal = strchr(part, '=');
        if (equal && (size_t)(equal - part) == strlen(key) &&
            strncmp(part, key, (size_t)(equal - part)) == 0) {
            url_decode(equal + 1);
            snprintf(value, value_size, "%s", equal + 1);
            return;
        }
        part = next ? next + 1 : NULL;
    }
}

static void handle_event(const char *body, int length) {
    char type[24], key[24], xs[32], ys[32], ids[32], downs[8];
    form_value(body, length, "t", type, sizeof(type));
    if (!strcmp(type, "down") || !strcmp(type, "move") || !strcmp(type, "up") ||
        !strcmp(type, "cancel-pointer")) {
        form_value(body, length, "x", xs, sizeof(xs));
        form_value(body, length, "y", ys, sizeof(ys));
        form_value(body, length, "id", ids, sizeof(ids));
        const int action = !strcmp(type, "down") ? 0 : !strcmp(type, "up") ? 1 :
                           !strcmp(type, "cancel-pointer") ? 4 : 2;
        game_touch((float)atof(xs), (float)atof(ys), action, atoi(ids));
    } else if (!strcmp(type, "cancel")) {
        game_cancel_input();
        previous_ns = 0;
    } else if (!strcmp(type, "key")) {
        form_value(body, length, "k", key, sizeof(key));
        form_value(body, length, "d", downs, sizeof(downs));
        game_key(key, downs[0] ? atoi(downs) : 1);
    }
}

static void handle_events(const char *body, int length) {
    while (length > 0) {
        const char *newline = (const char *)memchr(body, '\n', (size_t)length);
        const int line_length = newline ? (int)(newline - body) : length;
        if (line_length) handle_event(body, line_length);
        body += line_length;
        length -= line_length;
        if (length) { ++body; --length; }
    }
}

static char *read_html(void) {
    FILE *file = fopen("tools/preview/index.html", "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    const long length = ftell(file);
    rewind(file);
    if (length <= 0 || length > 65536) { fclose(file); return NULL; }
    char *html = (char *)malloc((size_t)length + 1);
    if (html && fread(html, 1, (size_t)length, file) == (size_t)length)
        html[length] = '\0';
    else { free(html); html = NULL; }
    fclose(file);
    return html;
}

int main(int argc, char **argv) {
    int port = 8090, width = 960, height = 540;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--port")) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--w")) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--h")) height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--storage") || !strcmp(argv[i], "--assets")) ++i;
    }
    if (width < 64 || height < 64 || width > 4096 || height > 4096 ||
        port < 1 || port > 65535) return 1;
    char *html = read_html();
    if (!html) { fprintf(stderr, "Run preview from the repository root.\n"); return 1; }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction shutdown_action = {0};
    shutdown_action.sa_handler = stop_server;
    sigemptyset(&shutdown_action.sa_mask);
    sigaction(SIGINT, &shutdown_action, NULL);
    sigaction(SIGTERM, &shutdown_action, NULL);

    screen_w = width; screen_h = height;
    frame.width = width; frame.height = height; frame.stride = width;
    frame.pixels = (uint32_t *)malloc((size_t)width * height * sizeof(uint32_t));
    if (!frame.pixels) return 1;
    game_init(NULL);
    render_frame();
    if (app_failed()) { fprintf(stderr, "%s\n", app_error()); return 1; }

    const int server = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server, 16) != 0) return 1;
    fprintf(stderr, "Enjoer cube preview: http://0.0.0.0:%d (%dx%d, backend=%s)\n",
            port, width, height, cube_renderer_backend());

    char request[16384];
    while (running) {
        const int client = accept(server, NULL, NULL);
        if (client < 0) continue;
        struct timeval timeout = {5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int body_offset = 0, body_length = 0;
        if (read_request(client, request, sizeof(request), &body_offset, &body_length) <= 0) {
            close(client); continue;
        }
        char method[8] = {0}, path[128] = {0};
        sscanf(request, "%7s %127s", method, path);
        if (!strcmp(method, "GET") && !strcmp(path, "/")) {
            http_head(client, 200, "text/html; charset=utf-8", strlen(html));
            send_all(client, html, strlen(html));
        } else if (!strcmp(method, "GET") && !strcmp(path, "/info")) {
            char info[256];
            const int n = snprintf(info, sizeof(info),
                "{\"w\":%d,\"h\":%d,\"backend\":\"%s\"}", width, height,
                cube_renderer_backend());
            http_head(client, 200, "application/json", (size_t)n);
            send_all(client, info, (size_t)n);
        } else if (!strcmp(method, "GET") && !strcmp(path, "/frame.jpg")) {
            render_frame();
            size_t length = 0;
            const unsigned char *bmp = preview_bmp(&frame, &length);
            if (bmp) { http_head(client, 200, "image/bmp", length); send_all(client, bmp, length); }
            else { http_head(client, 500, "text/plain", 5); send_all(client, "error", 5); }
        } else if (!strcmp(method, "POST") && !strcmp(path, "/event")) {
            handle_events(request + body_offset, body_length);
            http_head(client, 200, "text/plain", 2); send_all(client, "ok", 2);
        } else {
            http_head(client, 404, "text/plain", 3); send_all(client, "404", 3);
        }
        close(client);
    }
    game_shutdown();
    free(frame.pixels);
    free(html);
    close(server);
    return 0;
}
