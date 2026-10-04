/* Inject an atomic desktop replacement immediately after its first parsed read. */
#include <dlfcn.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/** Load into caller-owned keyfile, then replace the configured fixture once.
 * All pointers are borrowed; preserves the real loader's errors/return value.
 * Replacement source is consumed by rename; later reads use the new file. */
gboolean g_key_file_load_from_file(GKeyFile *file, const gchar *filename, GKeyFileFlags flags,
                                   GError **error) {
    typedef gboolean (*load_function)(GKeyFile *, const gchar *, GKeyFileFlags, GError **);
    void *symbol = dlsym(RTLD_NEXT, "g_key_file_load_from_file");
    load_function load = NULL;
    _Static_assert(sizeof(load) == sizeof(symbol), "ELF function pointer size");
    memcpy(&load, &symbol, sizeof(load));
    if (load == NULL)
        return FALSE;
    gboolean loaded = load(file, filename, flags, error);
    const char *target = getenv("TORCHLIGHT_TEST_DESKTOP_PATH");
    const char *replacement = getenv("TORCHLIGHT_TEST_DESKTOP_REPLACEMENT");
    if (loaded && target != NULL && replacement != NULL && strcmp(target, filename) == 0) {
        /* A successful rename consumes the source, so the hook is one-shot
         * without mutable process-wide test state. */
        if (rename(replacement, target) == 0)
            fputs("Injected atomic desktop replacement\n", stderr);
    }
    return loaded;
}
