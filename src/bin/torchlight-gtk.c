/* Thin GTK executable: own lifecycle and the process allocator release policy. */
#include "torchlight/launcher.h"
#ifdef __GLIBC__
#include <malloc.h>
#endif
/* Destroyed renderers free large heaps which glibc otherwise retains. The
 * launcher invokes this only after dismissal has settled, never while typing. */
static void release_memory(void *context) {
    (void)context;
#ifdef __GLIBC__
    int released = malloc_trim(0);
    (void)released;
#endif
}
int main(int argc, char **argv) {
    tl_launcher *launcher = NULL;
    tl_status status = launcher_create(&launcher);
    int exit_code = 1;
    if (status == TL_OK)
        status = launcher_set_memory_release(launcher, release_memory, NULL);
    if (status == TL_OK)
        status = launcher_run(launcher, argc, argv, &exit_code);
    launcher_destroy(launcher);
    return status == TL_OK ? exit_code : 1;
}
