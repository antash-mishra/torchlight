/* Worker-only desktop activation and exact byte file open/reveal actions. */
#include "actions.h"
#include "torchlight/desktop.h"
#include "torchlight/hashmap.h"
#include "torchlight/software_backend.h"
#include <gio/gdesktopappinfo.h>
#include <string.h>
static bool open_path(const char *path, GCancellable *cancel, GError **error) {
    if (g_cancellable_set_error_if_cancelled(cancel, error))
        return false;
    char *argv[] = {"xdg-open", (char *)path, NULL};
    return g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, error);
}
static bool reveal_path(const char *path, GCancellable *cancel) {
    GError *error = NULL;
    char *uri = g_filename_to_uri(path, NULL, &error);
    GDBusConnection *bus =
        error == NULL ? g_bus_get_sync(G_BUS_TYPE_SESSION, cancel, &error) : NULL;
    bool accepted = false;
    if (bus != NULL && uri != NULL) {
        const char *uris[] = {uri, NULL};
        GVariant *reply = g_dbus_connection_call_sync(
            bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.freedesktop.FileManager1", "ShowItems", g_variant_new("(^ass)", uris, ""), NULL,
            G_DBUS_CALL_FLAGS_NONE, IPC_DEADLINE_MS, cancel, &error);
        accepted = reply != NULL;
        if (reply != NULL)
            g_variant_unref(reply);
    }
    if (bus != NULL)
        g_object_unref(bus);
    bus = NULL;
    g_clear_error(&error);
    g_free(uri);
    /* Nemo can acknowledge an invalid-UTF-8 URI without showing its folder.
     * Keep the byte-preserving reveal request, then make its parent reachable. */
    if (accepted && json_utf8(path))
        return true;
    if (g_cancellable_is_cancelled(cancel))
        return accepted;
    char *parent = g_path_get_dirname(path);
    bool parent_accepted = open_path(parent, cancel, &error);
    g_clear_error(&error);
    g_free(parent);
    return accepted || parent_accepted;
}
static bool launch_keys_match(GKeyFile *file, GDesktopAppInfo *info) {
    /* These fields affect execution even when absent in the expected keyfile. */
    static const char *const KEYS[] = {"Exec",      "Path",         "Terminal",  "DBusActivatable",
                                       "TryExec",   "Hidden",       "NoDisplay", "OnlyShowIn",
                                       "NotShowIn", "StartupNotify"};
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        char *expected = g_key_file_get_string(file, "Desktop Entry", KEYS[i], NULL);
        char *actual = g_desktop_app_info_get_string(info, KEYS[i]);
        bool equal = g_strcmp0(expected, actual) == 0;
        g_free(expected);
        g_free(actual);
        if (!equal)
            return false;
    }
    return true;
}
static GDesktopAppInfo *validated_info(const tl_popup_row *row) {
    GKeyFile *file = g_key_file_new();
    GDesktopAppInfo *info = NULL;
    if (!g_key_file_load_from_file(file, row->path, G_KEY_FILE_NONE, NULL))
        goto cleanup;
    gsize length = 0;
    char *data = g_key_file_to_data(file, &length, NULL);
    uint64_t revision = hashmap_hash(HASHMAP_HASH_SEED, row->path, strlen(row->path));
    revision = hashmap_hash(revision, data, length);
    g_free(data);
    if (revision != row->desktop_revision)
        goto cleanup;
    /* Native filename constructor preserves desktop-id D-Bus activation and %k.
     * Compare execution keys to close the read/constructor replacement race. */
    info = g_desktop_app_info_new_from_filename(row->path);
    if (info != NULL && !launch_keys_match(file, info)) {
        g_object_unref(info);
        info = NULL;
    }
cleanup:
    g_key_file_unref(file);
    return info;
}
/* Whether this session's menu or search lists info, as the daemon's catalog
 * decides (desktop_shown_in, desktop_nodisplay_shown): GNOME Terminal launches
 * on Cinnamon, which lists it, and GNOME Settings' Displays panel on GNOME. */
static bool session_lists(GDesktopAppInfo *info) {
    if (g_desktop_app_info_get_is_hidden(info))
        return false;
    char **only_show_in = g_desktop_app_info_get_string_list(info, "OnlyShowIn", NULL);
    char **not_show_in = g_desktop_app_info_get_string_list(info, "NotShowIn", NULL);
    char **categories = g_desktop_app_info_get_string_list(info, "Categories", NULL);
    char *exec = g_desktop_app_info_get_string(info, "Exec");
    const char *desktops = g_getenv("XDG_CURRENT_DESKTOP");
    bool shown = (!g_desktop_app_info_get_nodisplay(info) ||
                  desktop_nodisplay_shown((const char *const *)categories, exec, desktops)) &&
                 desktop_shown_in((const char *const *)only_show_in,
                                  (const char *const *)not_show_in, exec, desktops);
    g_free(exec);
    g_strfreev(categories);
    g_strfreev(not_show_in);
    g_strfreev(only_show_in);
    return shown;
}
/* Desktop actions that open another window, most common first. */
static const char *const NEW_WINDOW_ACTIONS[] = {"new-window", "new-empty-window"};
static bool launch_application(GDesktopAppInfo *info, bool new_window, GAppLaunchContext *context,
                               GError **error) {
    const char *const *actions = new_window ? g_desktop_app_info_list_actions(info) : NULL;
    for (size_t i = 0; actions != NULL && i < G_N_ELEMENTS(NEW_WINDOW_ACTIONS); i++)
        if (g_strv_contains(actions, NEW_WINDOW_ACTIONS[i])) {
            /* GIO reports no error for actions; spawning is accepted like a launch. */
            g_desktop_app_info_launch_action(info, NEW_WINDOW_ACTIONS[i], context);
            return true;
        }
    return g_app_info_launch(G_APP_INFO(info), NULL, context, error);
}
static void cancel_process(GCancellable *cancel, gpointer context) {
    (void)cancel;
    atomic_store((atomic_bool *)context, true);
}
static bool open_software(const tl_popup_row *row, GCancellable *cancel, GError **error) {
    atomic_bool cancelled;
    atomic_init(&cancelled, false);
    gulong signal = g_cancellable_connect(cancel, G_CALLBACK(cancel_process), &cancelled, NULL);
    tl_process_budget budget = {.deadline_ms = process_now() + IPC_DEADLINE_MS,
                                .remaining_bytes = SOFTWARE_TEXT_BYTES,
                                .cancel = &cancelled};
    tl_status status = software_backend_validate(&row->software, row->source_revision, &budget);
    g_cancellable_disconnect(cancel, signal);
    if (status != TL_OK) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "This app’s source changed or is unavailable. Refresh and try again.");
        return false;
    }
    if (g_cancellable_set_error_if_cancelled(cancel, error))
        return false;
    const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
    bool kde = desktop != NULL && strstr(desktop, "KDE") != NULL;
    const char *names[] = {kde ? "plasma-discover" : "gnome-software",
                           kde ? "gnome-software" : "plasma-discover"};
    char *program = g_find_program_in_path(names[0]);
    const char *chosen = names[0];
    if (program == NULL) {
        chosen = names[1];
        program = g_find_program_in_path(chosen);
    }
    if (program == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "No supported software manager is available. Open GNOME Software or "
                            "KDE Discover with Flatpak support.");
        return false;
    }
    bool discover = strcmp(chosen, "plasma-discover") == 0;
    char *target = discover ? g_strdup_printf("appstream://%s", row->software.app_id)
                            : g_strdup_printf("--details=%s", row->software.app_id);
    char *argv[] = {program, discover ? "--application" : target, discover ? target : NULL, NULL};
    /* Both routes open app details. The manager owns source choice and consent. */
    bool accepted = !g_cancellable_is_cancelled(cancel) &&
                    g_spawn_async(NULL, argv, NULL, 0, NULL, NULL, NULL, error);
    g_free(target);
    g_free(program);
    return accepted;
}
static bool open_web(const tl_popup_row *row, GAppLaunchContext *context, GCancellable *cancel,
                     GError **error) {
    char uri[SEARCH_ACTION_URI_BYTES];
    tl_status status = row->web_uri[0] == 0
                           ? search_actions_uri(row->web_engine, row->web_query, uri, sizeof(uri))
                           : search_actions_address(row->web_query, uri, sizeof(uri));
    if (status != TL_OK || (row->web_uri[0] != 0 && strcmp(uri, row->web_uri) != 0)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid browser action.");
        return false;
    }
    if (g_cancellable_set_error_if_cancelled(cancel, error))
        return false;
    /* Resolve the current desktop handler on every activation, including legacy rows. */
    return g_app_info_launch_default_for_uri(uri, context, error);
}
void actions_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel) {
    (void)source;
    struct launch *launch = task_data;
    if (g_task_return_error_if_cancelled(task))
        return;
    GError *error = NULL;
    bool accepted = false;
    if (launch->row.target_kind == POPUP_TARGET_WEB)
        accepted = open_web(&launch->row, launch->context, cancel, &error);
    else if (launch->row.target_kind == POPUP_TARGET_SOFTWARE)
        accepted = open_software(&launch->row, cancel, &error);
    else if (launch->row.application && !launch->reveal) {
        GDesktopAppInfo *info = validated_info(&launch->row);
        if (info != NULL && session_lists(info) && !g_cancellable_is_cancelled(cancel))
            accepted = launch_application(info, launch->new_window, launch->context, &error);
        if (info != NULL)
            g_object_unref(info);
        info = NULL;
    } else if (launch->reveal)
        accepted = reveal_path(launch->row.path, cancel);
    else
        accepted = open_path(launch->row.path, cancel, &error);
    /* Once accepted, cancellation must not erase its history record. */
    g_task_set_check_cancellable(task, false);
    if (!accepted && error != NULL)
        g_task_return_error(task, error);
    else {
        g_clear_error(&error);
        g_task_return_boolean(task, accepted);
    }
}
