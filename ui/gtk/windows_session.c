/* The open-window source for the popup's session (ADR 0052): the display decides first, then
 * the compositor's task bar protocols, then the desktop shell running on the session bus. */
#include "windows_session.h"
#include "windows_kwin.h"
#include "windows_shell.h"
#include "windows_wayland.h"
#include "windows_x11.h"
/* Desktops whose own interface lists windows. */
typedef enum { SHELL_GNOME, SHELL_CINNAMON, SHELL_KWIN, SHELL_COUNT } shell;
/* Each as XDG_CURRENT_DESKTOP names it, and the bus name its window manager owns. */
static const char *const DESKTOP_NAMES[SHELL_COUNT] = {"GNOME", "X-Cinnamon", "KDE"};
static const char *const BUS_NAMES[SHELL_COUNT] = {WINDOWS_SHELL_GNOME, WINDOWS_SHELL_CINNAMON,
                                                   WINDOWS_KWIN_NAME};
/* Files under the state folder saying that the extension was turned on once. */
#define GNOME_MARKER "gnome-extension-enabled"
#define CINNAMON_MARKER "cinnamon-extension-enabled"
#define CINNAMON_SCHEMA "org.cinnamon"
#define CINNAMON_KEY "enabled-extensions"
/* The first desktop whose window manager owns its bus name, else the first one that
 * XDG_CURRENT_DESKTOP names (its shell may still be starting); SHELL_COUNT for none. The bus
 * name decides first because LXQt, Xfce and others can run on several compositors. */
static shell running_shell(GDBusConnection *bus) {
    for (int i = 0; i < SHELL_COUNT; i++)
        if (windows_shell_name_owned(bus, BUS_NAMES[i]))
            return (shell)i;
    const char *desktops = g_getenv("XDG_CURRENT_DESKTOP");
    for (int i = 0; i < SHELL_COUNT; i++)
        if (windows_shell_desktop(desktops, DESKTOP_NAMES[i]))
            return (shell)i;
    return SHELL_COUNT;
}
/* Turn Cinnamon's extension on once, if this system has Cinnamon's settings at all. Cinnamon
 * finds extensions in the user's data folder, then the system's. */
static void enable_cinnamon(const char *marker) {
    GSettingsSchemaSource *schemas = g_settings_schema_source_get_default();
    GSettingsSchema *schema =
        schemas != NULL ? g_settings_schema_source_lookup(schemas, CINNAMON_SCHEMA, true) : NULL;
    if (schema == NULL)
        return;
    if (g_settings_schema_has_key(schema, CINNAMON_KEY)) {
        const char *const *system = g_get_system_data_dirs();
        size_t count = g_strv_length((char **)system);
        const char **folders = g_new0(const char *, count + 2);
        folders[0] = g_get_user_data_dir();
        for (size_t i = 0; i < count; i++)
            folders[i + 1] = system[i];
        GSettings *settings = g_settings_new_full(schema, NULL, NULL);
        windows_shell_enable_cinnamon_once(settings, folders, marker);
        /* Written now, so Cinnamon loads the extension before the popup next lists. */
        g_settings_sync();
        g_object_unref(settings);
        g_free(folders);
    }
    g_settings_schema_unref(schema);
}
static tl_status shell_source(GDBusConnection *bus, shell running, const char *state_dir,
                              tl_window_source *out) {
    if (running == SHELL_KWIN)
        return windows_kwin_source(NULL, NULL, out);
    if (running != SHELL_GNOME && running != SHELL_CINNAMON)
        return TL_STATE;
    tl_status status = windows_shell_source(bus, BUS_NAMES[running], out);
    if (status != TL_OK)
        return status;
    char *marker =
        g_build_filename(state_dir, running == SHELL_GNOME ? GNOME_MARKER : CINNAMON_MARKER, NULL);
    if (running == SHELL_GNOME)
        windows_shell_enable_gnome_once(bus, marker);
    else
        enable_cinnamon(marker);
    g_free(marker);
    return TL_OK;
}
tl_status windows_session_source(GtkWidget *popup, const char *state_dir, tl_window_source *out) {
    if (out == NULL)
        return TL_INVALID;
    *out = (tl_window_source){0};
    if (popup == NULL || state_dir == NULL)
        return TL_INVALID;
    /* TL_STATE from a source means "not this kind of session": try the next kind. */
    tl_status status = windows_x11_source(popup, out);
    if (status == TL_STATE)
        status = windows_wayland_source(gtk_widget_get_display(popup), out);
    if (status != TL_STATE)
        return status;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus == NULL)
        return TL_STATE;
    status = shell_source(bus, running_shell(bus), state_dir, out);
    g_object_unref(bus);
    return status;
}
