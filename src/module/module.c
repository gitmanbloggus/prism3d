#include "p3d_module.h"
#include "../core/p3d_core.h"

#include <string.h>

#ifdef P3D_DYNAMIC_MODULES
#include <dlfcn.h>
#endif

#define MAX_MODULES 64

static const p3d_module *modules[MAX_MODULES];
static int active[MAX_MODULES];
static int module_count;

int p3d_module_register(const p3d_module *module)
{
    if (module == NULL || module->abi != P3D_MODULE_ABI) {
        P3D_ERROR("module %s: ABI mismatch", module ? module->name : "(null)");
        return 0;
    }
    if (module_count == MAX_MODULES) return 0;
    modules[module_count++] = module;
    return 1;
}

int p3d_module_load(const char *path)
{
#ifdef P3D_DYNAMIC_MODULES
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    p3d_module_entry_fn entry;
    if (lib == NULL) { P3D_ERROR("module %s: %s", path, dlerror()); return 0; }
    *(void **)&entry = dlsym(lib, "p3d_module_entry");
    if (entry == NULL) { P3D_ERROR("module %s: no p3d_module_entry", path); dlclose(lib); return 0; }
    return p3d_module_register(entry());
#else
    P3D_ERROR("module %s: dynamic modules are not available in this build", path);
    return 0;
#endif
}

const void *p3d_module_api(const char *name)
{
    int i;
    for (i = 0; i < module_count; ++i)
        if (active[i] && strcmp(modules[i]->name, name) == 0) return modules[i]->api;
    return NULL;
}

void p3d_modules_init(struct p3d_engine *engine)
{
    int i;
    for (i = 0; i < module_count; ++i) {
        active[i] = modules[i]->init == NULL || modules[i]->init(engine);
        if (!active[i]) P3D_WARN("module %s failed to start", modules[i]->name);
    }
}

void p3d_modules_update(struct p3d_engine *engine, float dt)
{
    int i;
    for (i = 0; i < module_count; ++i)
        if (active[i] && modules[i]->update) modules[i]->update(engine, dt);
}

void p3d_modules_shutdown(struct p3d_engine *engine)
{
    int i;
    for (i = module_count - 1; i >= 0; --i)
        if (active[i] && modules[i]->shutdown) modules[i]->shutdown(engine);
    module_count = 0;
}
