#include "p3d_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void p3d_log(p3d_log_level level, const char *fmt, ...)
{
    static const char *const names[] = { "debug", "info", "warn", "error" };
    va_list args;
    fprintf(stderr, "[prism %s] ", names[level]);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

int p3d_arena_init(p3d_arena *arena, size_t size)
{
    arena->base = (uint8_t *)malloc(size);
    arena->size = arena->base ? size : 0;
    arena->used = 0;
    arena->owns = 1;
    return arena->base != NULL;
}

void p3d_arena_init_buffer(p3d_arena *arena, void *buffer, size_t size)
{
    arena->base = (uint8_t *)buffer;
    arena->size = size;
    arena->used = 0;
    arena->owns = 0;
}

void *p3d_arena_alloc(p3d_arena *arena, size_t size)
{
    size_t start = (arena->used + 15u) & ~(size_t)15u;
    void *p;
    if (start > arena->size || size > arena->size - start) {
        P3D_ERROR("arena out of memory (%lu of %lu bytes used, %lu requested)",
                  (unsigned long)arena->used, (unsigned long)arena->size, (unsigned long)size);
        return NULL;
    }
    p = arena->base + start;
    arena->used = start + size;
    memset(p, 0, size);
    return p;
}

void p3d_arena_reset(p3d_arena *arena) { arena->used = 0; }

void p3d_arena_free(p3d_arena *arena)
{
    if (arena->owns) free(arena->base);
    arena->base = NULL;
    arena->size = arena->used = 0;
}

void *p3d_file_read(const char *path, size_t *size, p3d_arena *arena)
{
    FILE *f = fopen(path, "rb");
    long len;
    void *data;
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    data = arena ? p3d_arena_alloc(arena, (size_t)len + 1) : calloc(1, (size_t)len + 1);
    if (data != NULL && fread(data, 1, (size_t)len, f) != (size_t)len) {
        if (!arena) free(data);
        data = NULL;
    }
    fclose(f);
    if (data != NULL && size != NULL) *size = (size_t)len;
    return data;
}
