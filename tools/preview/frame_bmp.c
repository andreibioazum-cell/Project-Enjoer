/* Preview transport only. A dependency-free 24-bit BMP is understood by all
 * modern browsers and keeps the native preview free of image libraries. */
#include "engine.h"
#include <stdlib.h>
#include <string.h>

static unsigned char *encoded;
static size_t encoded_capacity;

static void put_u16(unsigned char *p, unsigned value) { p[0] = value; p[1] = value >> 8; }
static void put_u32(unsigned char *p, unsigned value) {
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}

/* The old symbol name is kept private to the preview server's tiny protocol. */
const unsigned char *preview_bmp(const Buffer *frame, size_t *length) {
    if (!frame || !frame->pixels || frame->width < 1 || frame->height < 1 ||
        frame->stride < frame->width || !length) return NULL;
    const size_t row = (size_t)frame->width * 3;
    const size_t padded_row = (row + 3u) & ~3u;
    const size_t bytes = 54 + padded_row * (size_t)frame->height;
    if (bytes > encoded_capacity) {
        unsigned char *replacement = (unsigned char *)realloc(encoded, bytes);
        if (!replacement) return NULL;
        encoded = replacement;
        encoded_capacity = bytes;
    }
    memset(encoded, 0, 54);
    encoded[0] = 'B'; encoded[1] = 'M';
    put_u32(encoded + 2, (unsigned)bytes);
    put_u32(encoded + 10, 54);
    put_u32(encoded + 14, 40);
    put_u32(encoded + 18, (unsigned)frame->width);
    put_u32(encoded + 22, (unsigned)(-frame->height)); /* top-down rows */
    put_u16(encoded + 26, 1);
    put_u16(encoded + 28, 24);
    put_u32(encoded + 34, (unsigned)(padded_row * (size_t)frame->height));
    for (int y = 0; y < frame->height; ++y) {
        unsigned char *out = encoded + 54 + padded_row * (size_t)y;
        const unsigned char *in = (const unsigned char *)(frame->pixels + y * frame->stride);
        memset(out, 0, padded_row);
        for (int x = 0; x < frame->width; ++x) {
            /* uint32 pixels are stored as RGBA bytes on the target machines. */
            out[x * 3 + 0] = in[x * 4 + 2];
            out[x * 3 + 1] = in[x * 4 + 1];
            out[x * 3 + 2] = in[x * 4 + 0];
        }
    }
    *length = bytes;
    return encoded;
}
