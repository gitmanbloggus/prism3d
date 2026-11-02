/* p3d_script.h: SIPL/SEB scripts as components. Each script is its own SEB VM with the `prism`
 * device bound (scripts/lib/prism.def). Scripts run once per frame until they write prism.frame,
 * which yields back to the engine (SEB_YIELD), so the engine owns the main loop on every platform. */
#ifndef P3D_SCRIPT_H
#define P3D_SCRIPT_H

#include "../scene/p3d_scene.h"

struct p3d_engine;

#define P3D_SCRIPT_STEPS_PER_FRAME 5000000u

/* Loads a compiled .bin and attaches it to object `owner` (0 = a new empty object). -1 on error. */
int  p3d_script_attach(struct p3d_engine *engine, const char *path, p3d_object_id owner);
void p3d_scripts_update(struct p3d_engine *engine);
int  p3d_scripts_running(void);   /* scripts that have not killed or failed */
void p3d_scripts_shutdown(void);

#endif
