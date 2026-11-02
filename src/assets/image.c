#include "p3d_image.h"
#include "../core/p3d_core.h"

#include <stdlib.h>
#include <string.h>

static int load_tga(const uint8_t *d, size_t n, p3d_image *out)
{
    int type, bpp, w, h, top_first, x, y, bytes;
    size_t pos, count = 0, total;
    uint8_t *px;
    if (n < 18) return 0;
    type = d[2];
    w = d[12] | (d[13] << 8);
    h = d[14] | (d[15] << 8);
    bpp = d[16];
    top_first = (d[17] & 0x20) != 0;
    if ((type != 2 && type != 10) || (bpp != 24 && bpp != 32) || w <= 0 || h <= 0 || d[1] != 0) return 0;
    bytes = bpp / 8;
    total = (size_t)w * (size_t)h;
    px = (uint8_t *)malloc(total * 4u);
    if (px == NULL) return 0;
    pos = 18u + d[0];
    while (count < total) {
        int run = 1, raw = 1, i;
        if (type == 10) {
            if (pos >= n) break;
            run = (d[pos] & 0x7F) + 1;
            raw = !(d[pos] & 0x80);
            ++pos;
        }
        for (i = 0; i < run && count < total; ++i, ++count) {
            const uint8_t *s;
            if (pos + (size_t)bytes > n) { free(px); return 0; }
            s = d + pos;
            px[count * 4] = s[2]; px[count * 4 + 1] = s[1]; px[count * 4 + 2] = s[0];
            px[count * 4 + 3] = bytes == 4 ? s[3] : 255;
            if (raw || i == run - 1) pos += (size_t)bytes;
        }
    }
    if (count < total) { free(px); return 0; }
    if (!top_first)
        for (y = 0; y < h / 2; ++y)
            for (x = 0; x < w * 4; ++x) {
                uint8_t *a = px + (size_t)y * (size_t)w * 4u + (size_t)x;
                uint8_t *b = px + (size_t)(h - 1 - y) * (size_t)w * 4u + (size_t)x;
                uint8_t t = *a; *a = *b; *b = t;
            }
    out->width = w; out->height = h; out->rgba = px;
    return 1;
}

static int ppm_int(const uint8_t *d, size_t n, size_t *pos, int *value)
{
    int v = 0, digits = 0;
    while (*pos < n && (d[*pos] == ' ' || d[*pos] == '\n' || d[*pos] == '\r' || d[*pos] == '\t' || d[*pos] == '#')) {
        if (d[*pos] == '#') while (*pos < n && d[*pos] != '\n') ++*pos;
        else ++*pos;
    }
    while (*pos < n && d[*pos] >= '0' && d[*pos] <= '9') { v = v * 10 + (d[*pos] - '0'); ++*pos; ++digits; }
    *value = v;
    return digits > 0;
}

static int load_ppm(const uint8_t *d, size_t n, p3d_image *out)
{
    size_t pos = 2, i, total;
    int w, h, maxv;
    uint8_t *px;
    if (!ppm_int(d, n, &pos, &w) || !ppm_int(d, n, &pos, &h) || !ppm_int(d, n, &pos, &maxv) || maxv != 255) return 0;
    ++pos;
    total = (size_t)w * (size_t)h;
    if (w <= 0 || h <= 0 || pos + total * 3u > n) return 0;
    px = (uint8_t *)malloc(total * 4u);
    if (px == NULL) return 0;
    for (i = 0; i < total; ++i) {
        memcpy(px + i * 4, d + pos + i * 3, 3);
        px[i * 4 + 3] = 255;
    }
    out->width = w; out->height = h; out->rgba = px;
    return 1;
}

int p3d_image_load(const char *path, p3d_image *out)
{
    size_t n;
    uint8_t *d = (uint8_t *)p3d_file_read(path, &n, NULL);
    int ok;
    memset(out, 0, sizeof(*out));
    if (d == NULL) { P3D_WARN("can't read %s", path); return 0; }
    ok = (n > 2 && d[0] == 'P' && d[1] == '6') ? load_ppm(d, n, out) : load_tga(d, n, out);
    if (!ok) P3D_WARN("%s: not a supported image (TGA 24/32-bit or binary PPM)", path);
    free(d);
    return ok;
}

void p3d_image_free(p3d_image *image)
{
    free(image->rgba);
    image->rgba = NULL;
}
