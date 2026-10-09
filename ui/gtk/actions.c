/* Worker-only desktop activation and exact byte file open/reveal actions. */
#include "actions.h"
#include "torchlight/hashmap.h"
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
void actions_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel) {
    (void)source;
    struct launch *launch = task_data;
    if (g_task_return_error_if_cancelled(task))
        return;
    GError *error = NULL;
    bool accepted = false;
    if (launch->row.application && !launch->reveal) {
        GDesktopAppInfo *info = validated_info(&launch->row);
        if (info != NULL && !g_desktop_app_info_get_is_hidden(info) &&
            g_app_info_should_show(G_APP_INFO(info)) && !g_cancellable_is_cancelled(cancel))
            accepted = launch_application(info, launch->new_window, launch->context, &error);
        if (info != NULL)
            g_object_unref(info);
        info = NULL;
    } else if (launch->reveal)
        accepted = reveal_path(launch->row.path, cancel);
    else
        accepted = open_path(launch->row.path, cancel, &error);
    g_clear_error(&error);
    /* Once accepted, cancellation must not erase its history record. */
    g_task_set_check_cancellable(task, false);
    g_task_return_boolean(task, accepted);
}
