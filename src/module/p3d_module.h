/* p3d_module.h: the one interface every optional subsystem implements.
 *
 * A module is linked statically (listed in the build's MODULES, registered by the generated
 * p3d_static_modules table) or, on platforms with a dynamic loader, built as a shared library that
 * exports `const p3d_module *p3d_module_entry(void)`. Same struct either way. */
#ifndef P3D_MODULE_H
#define P3D_MODULE_H

struct p3d_engine;

#define P3D_MODULE_ABI 1

typedef struct p3d_module {
    unsigned abi;                 /* P3D_MODULE_ABI */
    const char *name;
    int  (*init)(struct p3d_engine *engine);        /* 0 = failed; the engine skips the module */
    void (*update)(struct p3d_engine *engine, float dt);
    void (*shutdown)(struct p3d_engine *engine);
    const void *api;              /* module-specific function table, found with p3d_module_api() */
} p3d_module;

typedef const p3d_module *(*p3d_module_entry_fn)(void);

int  p3d_module_register(const p3d_module *module);
int  p3d_module_load(const char *path);          /* dynamic; returns 0 where unsupported */
const void *p3d_module_api(const char *name);
void p3d_modules_init(struct p3d_engine *engine);
void p3d_modules_update(struct p3d_engine *engine, float dt);
void p3d_modules_shutdown(struct p3d_engine *engine);

#endif
