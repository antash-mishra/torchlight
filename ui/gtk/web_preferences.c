/* Read the saved search engine; browser activation always follows the system. */
#include "web_preferences.h"
#include <glib.h>

struct tl_web_preferences {
    tl_web_options options;
};
static void load_options(tl_web_preferences *self) {
    char *path = g_build_filename(g_get_user_config_dir(), "torchlight", "web.ini", NULL);
    GKeyFile *file = g_key_file_new();
    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)) {
        char *engine = g_key_file_get_string(file, "Web", "engine", NULL);
        if (search_actions_engine_parse(engine, &self->options.engine) == TL_OK)
            self->options.override_engine = true;
        g_free(engine);
    }
    g_key_file_unref(file);
    g_free(path);
}
tl_status web_preferences_create(tl_web_preferences **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_web_preferences *self = g_try_new0(tl_web_preferences, 1);
    if (self == NULL)
        return TL_NOMEM;
    /* Do not pin a handler or its name: the desktop default can change while running. */
    g_strlcpy(self->options.browser, "default", sizeof(self->options.browser));
    g_strlcpy(self->options.browser_name, "System default", sizeof(self->options.browser_name));
    load_options(self);
    *out = self;
    return TL_OK;
}
void web_preferences_destroy(tl_web_preferences *self) {
    g_free(self);
}
const tl_web_options *web_preferences_options(const tl_web_preferences *self) {
    return self == NULL ? NULL : &self->options;
}
void web_preferences_inherit(tl_web_preferences *self, tl_search_engine engine) {
    if (self == NULL || self->options.override_engine || search_actions_engine(engine) == NULL)
        return;
    self->options.engine = engine;
}
