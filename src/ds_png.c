/*
 * PNG decoding for DimScript's image.* builtins.
 *
 * `image.load("sprites.png")` has to work on the device and in the tests, so
 * the decoder lives in the engine instead of a dependency: an inflate
 * implementation (RFC 1951), a CRC-32, and the PNG side of it (IHDR/PLTE/tRNS/
 * IDAT, all five row filters, bit depths 8 and 16, colour types 0/2/3/4/6).
 * Interlaced files, odd bit depths and unknown chunks that matter are rejected
 * with a readable message instead of producing garbage pixels.
 */
#include "dimscript_runtime.h"

#include <stdlib.h>
#include <string.h>

#define PNG_MAX_BITS 15
#define PNG_MAX_LCODES 286
#define PNG_MAX_DCODES 30
#define PNG_MAX_CODES (PNG_MAX_LCODES + PNG_MAX_DCODES)

typedef struct BitReader {
    const uint8_t *data;
    size_t size;
    size_t position;
    uint32_t buffer;
    int count;
} BitReader;

typedef struct Huffman {
    int16_t count[PNG_MAX_BITS + 1];
    int16_t symbol[PNG_MAX_CODES];
} Huffman;

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void) {
    for (uint32_t index = 0; index < 256; ++index) {
        uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1) ? 0xEDB88320u ^ (value >> 1) : value >> 1;
        crc_table[index] = value;
    }
    crc_ready = 1;
}

static uint32_t crc32_bytes(const uint8_t *data, size_t length, uint32_t crc) {
    if (!crc_ready) crc_init();
    crc ^= 0xFFFFFFFFu;
    for (size_t index = 0; index < length; ++index)
        crc = crc_table[(crc ^ data[index]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static int read_bits(BitReader *reader, int need) {
    int value = 0;
    int got = 0;
    while (got < need) {
        if (reader->count == 0) {
            if (reader->position >= reader->size) return -1;
            reader->buffer = reader->data[reader->position++];
            reader->count = 8;
        }
        value |= (int)((reader->buffer & 1u) << got);
        reader->buffer >>= 1;
        --reader->count;
        ++got;
    }
    return value;
}

static int huffman_build(Huffman *huffman, const int16_t *lengths, int count) {
    memset(huffman->count, 0, sizeof(huffman->count));
    for (int index = 0; index < count; ++index) huffman->count[lengths[index]]++;
    if (huffman->count[0] == count) return 0; /* nothing coded: valid, empty */
    int left = 1;
    for (int length = 1; length <= PNG_MAX_BITS; ++length) {
        left <<= 1;
        left -= huffman->count[length];
        if (left < 0) return -1;
    }
    int16_t offsets[PNG_MAX_BITS + 2];
    offsets[1] = 0;
    for (int length = 1; length <= PNG_MAX_BITS; ++length)
        offsets[length + 1] = (int16_t)(offsets[length] + huffman->count[length]);
    for (int index = 0; index < count; ++index)
        if (lengths[index]) huffman->symbol[offsets[lengths[index]]++] = (int16_t)index;
    return left;
}

static int huffman_decode(BitReader *reader, const Huffman *huffman) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int length = 1; length <= PNG_MAX_BITS; ++length) {
        const int bit = read_bits(reader, 1);
        if (bit < 0) return -1;
        code |= bit;
        const int count = huffman->count[length];
        if (code - count < first) return huffman->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_codes(BitReader *reader, const Huffman *literal, const Huffman *distance,
                         uint8_t *out, size_t out_size, size_t *out_used) {
    static const int length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                         3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                          193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                          6145, 8193, 12289, 16385, 24577};
    static const int distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                           7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        const int symbol = huffman_decode(reader, literal);
        if (symbol < 0) return -1;
        if (symbol < 256) {
            if (*out_used >= out_size) return -1;
            out[(*out_used)++] = (uint8_t)symbol;
            continue;
        }
        if (symbol == 256) return 0;
        const int index = symbol - 257;
        if (index >= 29) return -1;
        int extra = read_bits(reader, length_extra[index]);
        if (extra < 0) return -1;
        const int length = length_base[index] + extra;
        const int distance_symbol = huffman_decode(reader, distance);
        if (distance_symbol < 0 || distance_symbol >= 30) return -1;
        extra = read_bits(reader, distance_extra[distance_symbol]);
        if (extra < 0) return -1;
        const size_t distance_back = (size_t)(distance_base[distance_symbol] + extra);
        if (distance_back > *out_used) return -1;
        for (int copied = 0; copied < length; ++copied) {
            if (*out_used >= out_size) return -1;
            out[*out_used] = out[*out_used - distance_back];
            ++*out_used;
        }
    }
}

static int inflate_fixed(BitReader *reader, uint8_t *out, size_t out_size, size_t *out_used) {
    int16_t literal_lengths[288];
    int16_t distance_lengths[30];
    for (int index = 0; index < 144; ++index) literal_lengths[index] = 8;
    for (int index = 144; index < 256; ++index) literal_lengths[index] = 9;
    for (int index = 256; index < 280; ++index) literal_lengths[index] = 7;
    for (int index = 280; index < 288; ++index) literal_lengths[index] = 8;
    for (int index = 0; index < 30; ++index) distance_lengths[index] = 5;
    Huffman literal, distance;
    if (huffman_build(&literal, literal_lengths, 288) < 0) return -1;
    if (huffman_build(&distance, distance_lengths, 30) < 0) return -1;
    return inflate_codes(reader, &literal, &distance, out, out_size, out_used);
}

static int inflate_dynamic(BitReader *reader, uint8_t *out, size_t out_size, size_t *out_used) {
    static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int16_t code_lengths[19];
    const int literal_count = read_bits(reader, 5);
    const int distance_count = read_bits(reader, 5);
    const int code_count = read_bits(reader, 4);
    if (literal_count < 0 || distance_count < 0 || code_count < 0) return -1;
    const int literal_total = literal_count + 257;
    const int distance_total = distance_count + 1;
    const int code_total = code_count + 4;
    if (literal_total > PNG_MAX_LCODES || distance_total > PNG_MAX_DCODES) return -1;
    memset(code_lengths, 0, sizeof(code_lengths));
    for (int index = 0; index < code_total; ++index) {
        const int value = read_bits(reader, 3);
        if (value < 0) return -1;
        code_lengths[order[index]] = (int16_t)value;
    }
    Huffman code_huffman;
    if (huffman_build(&code_huffman, code_lengths, 19) < 0) return -1;

    int16_t lengths[PNG_MAX_LCODES + PNG_MAX_DCODES];
    const int total = literal_total + distance_total;
    for (int index = 0; index < total;) {
        const int symbol = huffman_decode(reader, &code_huffman);
        if (symbol < 0) return -1;
        if (symbol < 16) {
            lengths[index++] = (int16_t)symbol;
            continue;
        }
        int repeat = 0;
        int16_t value = 0;
        if (symbol == 16) {
            if (index == 0) return -1;
            value = lengths[index - 1];
            const int extra = read_bits(reader, 2);
            if (extra < 0) return -1;
            repeat = 3 + extra;
        } else if (symbol == 17) {
            const int extra = read_bits(reader, 3);
            if (extra < 0) return -1;
            repeat = 3 + extra;
        } else {
            const int extra = read_bits(reader, 7);
            if (extra < 0) return -1;
            repeat = 11 + extra;
        }
        if (index + repeat > total) return -1;
        while (repeat-- > 0) lengths[index++] = value;
    }

    Huffman literal, distance;
    if (huffman_build(&literal, lengths, literal_total) < 0) return -1;
    if (huffman_build(&distance, lengths + literal_total, distance_total) < 0) return -1;
    return inflate_codes(reader, &literal, &distance, out, out_size, out_used);
}

/* Raw DEFLATE stream (RFC 1951). */
static int inflate_raw(const uint8_t *data, size_t length, uint8_t *out, size_t out_size,
                       size_t *out_used) {
    BitReader reader = {data, length, 0, 0, 0};
    *out_used = 0;
    for (;;) {
        const int final = read_bits(&reader, 1);
        if (final < 0) return -1;
        const int type = read_bits(&reader, 2);
        if (type < 0) return -1;
        if (type == 0) {
            reader.buffer = 0;
            reader.count = 0;
            if (reader.position + 4 > reader.size) return -1;
            const int stored = reader.data[reader.position] | (reader.data[reader.position + 1] << 8);
            reader.position += 4;
            if (reader.position + (size_t)stored > reader.size) return -1;
            if (*out_used + (size_t)stored > out_size) return -1;
            memcpy(out + *out_used, reader.data + reader.position, (size_t)stored);
            reader.position += (size_t)stored;
            *out_used += (size_t)stored;
        } else if (type == 1) {
            if (inflate_fixed(&reader, out, out_size, out_used) < 0) return -1;
        } else if (type == 2) {
            if (inflate_dynamic(&reader, out, out_size, out_used) < 0) return -1;
        } else {
            return -1;
        }
        if (final) break;
    }
    return 0;
}

/* zlib stream (RFC 1950) around a raw DEFLATE stream. */
static int inflate_zlib(const uint8_t *data, size_t length, uint8_t *out, size_t out_size,
                        size_t *out_used) {
    if (length < 6) return -1;
    if ((data[0] & 0x0F) != 8) return -1; /* only the deflate method */
    if (((data[0] << 8) | data[1]) % 31 != 0) return -1;
    return inflate_raw(data + 2, length - 6, out, out_size, out_used);
}

/* --- PNG ------------------------------------------------------------------ */

static uint32_t be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static int paeth(int left, int above, int upper_left) {
    const int estimate = left + above - upper_left;
    const int da = estimate > left ? estimate - left : left - estimate;
    const int db = estimate > above ? estimate - above : above - estimate;
    const int dc = estimate > upper_left ? estimate - upper_left : upper_left - estimate;
    if (da <= db && da <= dc) return left;
    return db <= dc ? above : upper_left;
}

static int channels_for(int color_type) {
    switch (color_type) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    default: return 0;
    }
}

uint8_t *ds_png_decode(const uint8_t *data, size_t length, int32_t *width, int32_t *height,
                       const char **error) {
    static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    const char *fail = NULL;
    if (length < 8 || memcmp(data, signature, 8) != 0) {
        *error = "это не PNG";
        return NULL;
    }
    uint32_t image_width = 0;
    uint32_t image_height = 0;
    int bit_depth = 0;
    int color_type = -1;
    int interlace = 0;
    uint8_t palette[256 * 3];
    uint8_t palette_alpha[256];
    int palette_size = 0;
    for (int index = 0; index < 256; ++index) palette_alpha[index] = 255;

    uint8_t *compressed = NULL;
    size_t compressed_size = 0;
    size_t position = 8;
    while (position + 12 <= length) {
        const uint8_t *chunk = data + position;
        const uint32_t chunk_length = be32(chunk);
        if (position + 12 + chunk_length > length) {
            fail = "обрезанный чанк PNG";
            break;
        }
        const uint8_t *payload = chunk + 8;
        /* The chunk CRC is part of the format, so a truncated or corrupted
         * sprite is reported instead of being decoded into garbage pixels. */
        if (crc32_bytes(chunk + 4, (size_t)chunk_length + 4, 0) != be32(payload + chunk_length)) {
            fail = "повреждённый чанк PNG (CRC)";
            break;
        }
        if (!memcmp(chunk + 4, "IHDR", 4)) {
            if (chunk_length < 13) {
                fail = "короткий IHDR";
                break;
            }
            image_width = be32(payload);
            image_height = be32(payload + 4);
            bit_depth = payload[8];
            color_type = payload[9];
            interlace = payload[12];
        } else if (!memcmp(chunk + 4, "PLTE", 4)) {
            palette_size = (int)(chunk_length / 3);
            if (palette_size > 256) palette_size = 256;
            memcpy(palette, payload, (size_t)palette_size * 3);
        } else if (!memcmp(chunk + 4, "tRNS", 4)) {
            if (color_type == 3) {
                const size_t entries = chunk_length < 256 ? chunk_length : 256;
                for (size_t entry = 0; entry < entries; ++entry) palette_alpha[entry] = payload[entry];
            }
        } else if (!memcmp(chunk + 4, "IDAT", 4)) {
            uint8_t *grown = (uint8_t *)realloc(compressed, compressed_size + chunk_length);
            if (!grown) {
                fail = "нет памяти под PNG";
                break;
            }
            compressed = grown;
            memcpy(compressed + compressed_size, payload, chunk_length);
            compressed_size += chunk_length;
        } else if (!memcmp(chunk + 4, "IEND", 4)) {
            break;
        }
        position += 12 + chunk_length;
    }
    if (fail) free(compressed);
    if (fail) {
        *error = fail;
        return NULL;
    }
    if (!image_width || !image_height || image_width > 4096 || image_height > 4096) {
        free(compressed);
        *error = "неверный размер PNG";
        return NULL;
    }
    const int channels = channels_for(color_type);
    if (!channels || (bit_depth != 8 && bit_depth != 16) || interlace != 0) {
        free(compressed);
        *error = bit_depth == 8 || bit_depth == 16 ? "неподдерживаемый PNG" : "неподдерживаемая битность";
        return NULL;
    }
    if (!compressed) {
        *error = "в PNG нет данных IDAT";
        return NULL;
    }

    const size_t bits_per_pixel = (size_t)channels * (size_t)bit_depth;
    const size_t stride = (image_width * bits_per_pixel + 7) / 8;
    const size_t raw_size = (stride + 1) * image_height;
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    uint8_t *rgba = (uint8_t *)malloc((size_t)image_width * image_height * 4);
    if (!raw || !rgba) {
        free(raw);
        free(rgba);
        free(compressed);
        *error = "нет памяти под PNG";
        return NULL;
    }
    size_t produced = 0;
    if (inflate_zlib(compressed, compressed_size, raw, raw_size, &produced) < 0 ||
        produced < raw_size) {
        free(raw);
        free(rgba);
        free(compressed);
        *error = "повреждённые данные PNG";
        return NULL;
    }
    free(compressed);

    const size_t filter_bytes = (bits_per_pixel + 7) / 8;
    for (uint32_t row = 0; row < image_height; ++row) {
        const uint8_t filter = raw[(stride + 1) * row];
        uint8_t *line = raw + (stride + 1) * row + 1;
        const uint8_t *previous = row ? line - (stride + 1) : NULL;
        for (size_t column = 0; column < stride; ++column) {
            const int left = column >= filter_bytes ? line[column - filter_bytes] : 0;
            const int above = previous ? previous[column] : 0;
            const int upper_left = (previous && column >= filter_bytes) ? previous[column - filter_bytes] : 0;
            switch (filter) {
            case 0: break;
            case 1: line[column] = (uint8_t)(line[column] + left); break;
            case 2: line[column] = (uint8_t)(line[column] + above); break;
            case 3: line[column] = (uint8_t)(line[column] + ((left + above) >> 1)); break;
            case 4: line[column] = (uint8_t)(line[column] + paeth(left, above, upper_left)); break;
            default:
                free(raw);
                free(rgba);
                *error = "неизвестный фильтр PNG";
                return NULL;
            }
        }
        uint8_t *out = rgba + (size_t)row * image_width * 4;
        for (uint32_t column = 0; column < image_width; ++column) {
            /* 16 bit samples keep their high byte: sprites do not need more. */
            const size_t step = (bit_depth == 16) ? 2 : 1;
            const uint8_t *pixel = line + (size_t)column * channels * step;
            uint8_t red = 0, green = 0, blue = 0, alpha = 255;
            switch (color_type) {
            case 0:
                red = green = blue = pixel[0];
                break;
            case 2:
                red = pixel[0];
                green = pixel[step];
                blue = pixel[2 * step];
                break;
            case 3:
                if (pixel[0] >= palette_size) {
                    free(raw);
                    free(rgba);
                    *error = "палитра PNG короче данных";
                    return NULL;
                }
                red = palette[pixel[0] * 3];
                green = palette[pixel[0] * 3 + 1];
                blue = palette[pixel[0] * 3 + 2];
                alpha = palette_alpha[pixel[0]];
                break;
            case 4:
                red = green = blue = pixel[0];
                alpha = pixel[step];
                break;
            default:
                red = pixel[0];
                green = pixel[step];
                blue = pixel[2 * step];
                alpha = pixel[3 * step];
                break;
            }
            out[column * 4] = red;
            out[column * 4 + 1] = green;
            out[column * 4 + 2] = blue;
            out[column * 4 + 3] = alpha;
        }
    }
    free(raw);
    *width = (int32_t)image_width;
    *height = (int32_t)image_height;
    *error = NULL;
    return rgba;
}
