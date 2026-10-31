/* Saved search-engine preferences with system-default browser selection. */
#ifndef TORCHLIGHT_GTK_WEB_PREFERENCES_H
#define TORCHLIGHT_GTK_WEB_PREFERENCES_H
#include "torchlight/search_actions.h"
typedef struct tl_web_preferences tl_web_preferences;
/** Create owned preferences by reading the user's torchlight/web.ini once.
 * Missing, malformed or unreadable files retain defaults. Only the engine is
 * loaded; legacy browser keys are ignored. Browser options always name the system
 * default, resolved during activation. Never writes settings or creates controls.
 * TL_INVALID/NOMEM, out NULL on error. Release with web_preferences_destroy. */
tl_status web_preferences_create(tl_web_preferences **out);
/** Free owned preferences. NULL safe; no errors. */
void web_preferences_destroy(tl_web_preferences *preferences);
/** Borrow current options until destroy. NULL returns NULL. No I/O. */
const tl_web_options *web_preferences_options(const tl_web_preferences *preferences);
/** Update the inherited engine from a daemon response when no user override is
 * saved. Main thread, NULL safe, no I/O or ownership changes. */
void web_preferences_inherit(tl_web_preferences *preferences, tl_search_engine engine);
#endif
