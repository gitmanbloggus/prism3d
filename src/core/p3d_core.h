/* p3d_core.h: base types, logging and memory arenas. C99, no dependencies beyond libc. */
#ifndef P3D_CORE_H
#define P3D_CORE_H

#include <stddef.h>
#include <stdint.h>

#define P3D_VERSION_MAJOR 0
#define P3D_VERSION_MINOR 1

#define P3D_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define P3D_UNUSED(x) ((void)(x))

typedef enum { P3D_LOG_DEBUG, P3D_LOG_INFO, P3D_LOG_WARN, P3D_LOG_ERROR } p3d_log_level;

void p3d_log(p3d_log_level level, const char *fmt, ...);
#define P3D_INFO(...)  p3d_log(P3D_LOG_INFO, __VA_ARGS__)
#define P3D_WARN(...)  p3d_log(P3D_LOG_WARN, __VA_ARGS__)
#define P3D_ERROR(...) p3d_log(P3D_LOG_ERROR, __VA_ARGS__)

/* Linear arena: one block, bump allocation, freed all at once. Every engine allocation should go
 * through an arena or a pool so retro targets can run out of a fixed, preallocated heap. */
typedef struct {
    uint8_t *base;
    size_t size;
    size_t used;
    int owns;
} p3d_arena;

int   p3d_arena_init(p3d_arena *arena, size_t size);                    /* mallocs the block */
void  p3d_arena_init_buffer(p3d_arena *arena, void *buffer, size_t size); /* caller-owned block */
void *p3d_arena_alloc(p3d_arena *arena, size_t size);                   /* 16-byte aligned, zeroed */
void  p3d_arena_reset(p3d_arena *arena);
void  p3d_arena_free(p3d_arena *arena);

/* Whole-file read into an arena (or malloc when arena is NULL). Returns NULL on failure. */
void *p3d_file_read(const char *path, size_t *size, p3d_arena *arena);

#endif
