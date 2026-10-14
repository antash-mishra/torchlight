/* KWin script window source (ADR 0053). KWin has no call that returns its windows, so a
 * short script does the work inside KWin and reports back with callDBus. The scripts are
 * ES5, for Plasma 5's QtScript engine; Plasma 6 renamed clientList and activeClient to
 * windowList and activeWindow, which the scripts test for. */
#include "windows_kwin.h"
#include "torchlight/json.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define SCRIPTING_PATH "/Scripting"
#define SCRIPTING_INTERFACE "org.kde.kwin.Scripting"
#define SCRIPT_INTERFACE "org.kde.kwin.Script"
#define REPORT_PATH "/org/torchlight/KWin"
#define REPORT_INTERFACE "org.torchlight.KWin"
#define REPORT_XML                                                                                 \
    "<node><interface name='" REPORT_INTERFACE "'><method name='Report'>"                          \
    "<arg type='s' direction='in'/><arg type='s' direction='in'/></method></interface></node>"
/* The launcher's own application id: its popup is never listed as one of its windows. */
#define LAUNCHER_ID "org.torchlight.Launcher"
#define DESKTOP_SUFFIX ".desktop"
/* KWin's window ids are UUIDs in braces. */
#define WINDOW_ID_BYTES 64
/* The script cuts titles to this many UTF-16 units, at most 3 UTF-8 bytes each, so a decoded
 * title fits TITLE_BYTES; the window list then cuts it to its own size. */
#define SCRIPT_TITLE_UNITS 1000
#define TITLE_BYTES 4096
/* A report lists up to WINDOWS_MAX windows as arrays of five strings. */
#define REPORT_FIELDS 5
#define REPORT_TOKENS (1 + WINDOWS_MAX * (1 + REPORT_FIELDS))
/* Room for a decimal 64-bit call number. */
#define TOKEN_BYTES 24
/* Hex digits of a window id that make its handle: 64 bits of a random UUID. */
#define HANDLE_DIGITS 16
/* The list script's arguments are the window limit, the title limit, the launcher's bus name
 * and the call's token. It sends KWin's normal windows, topmost first, as
 * [id, title, desktop file, class, instance]. */
#define LIST_SCRIPT                                                                                \
    "// Torchlight (ADR 0053): report KWin's normal windows, topmost first.\n"                     \
    "(function () {\n"                                                                             \
    "    var all = workspace.windowList ? workspace.windowList() : workspace.clientList();\n"      \
    "    var windows = [];\n"                                                                      \
    "    for (var i = 0; i < all.length; i++)\n"                                                   \
    "        if (all[i].normalWindow && !all[i].skipTaskbar)\n"                                    \
    "            windows.push(all[i]);\n"                                                          \
    "    windows.sort(function (a, b) { return b.stackingOrder - a.stackingOrder; });\n"           \
    "    var report = [];\n"                                                                       \
    "    for (var j = 0; j < windows.length && j < %d; j++) {\n"                                   \
    "        var w = windows[j];\n"                                                                \
    "        var title = w.captionNormal !== undefined ? w.captionNormal : w.caption;\n"           \
    "        report.push([String(w.internalId), String(title).substring(0, %d),\n"                 \
    "                     String(w.desktopFileName || ''), String(w.resourceClass),\n"             \
    "                     String(w.resourceName)]);\n"                                             \
    "    }\n"                                                                                      \
    "    callDBus('%s', '" REPORT_PATH "', '" REPORT_INTERFACE "', 'Report', '%s',\n"              \
    "             JSON.stringify(report));\n"                                                      \
    "})();\n"
/* The activation script's arguments are the window's id, the launcher's bus name and the
 * call's token. */
#define ACTIVATE_SCRIPT                                                                            \
    "// Torchlight (ADR 0053): switch to one window and say whether it was still open.\n"          \
    "(function () {\n"                                                                             \
    "    var all = workspace.windowList ? workspace.windowList() : workspace.clientList();\n"      \
    "    var found = false;\n"                                                                     \
    "    for (var i = 0; i < all.length && !found; i++) {\n"                                       \
    "        if (String(all[i].internalId) !== '%s')\n"                                            \
    "            continue;\n"                                                                      \
    "        if (workspace.activeWindow !== undefined)\n"                                          \
    "            workspace.activeWindow = all[i];\n"                                               \
    "        else\n"                                                                               \
    "            workspace.activeClient = all[i];\n"                                               \
    "        found = true;\n"                                                                      \
    "    }\n"                                                                                      \
    "    callDBus('%s', '" REPORT_PATH "', '" REPORT_INTERFACE "', 'Report', '%s',\n"              \
    "             found ? '1' : '0');\n"                                                           \
    "})();\n"
struct window_id {
    uint64_t handle;
    char id[WINDOW_ID_BYTES];
};
struct kwin_source {
    /* Only this source's calls iterate the context, which dispatches the reports. */
    GMainContext *context;
    GDBusConnection *bus;
    guint object;
    char *script, *plugin;
    /* Plasma 6 serves loaded scripts at /Scripting/Script<id>, Plasma 5 at /<id>. */
    bool numbered_paths;
    uint64_t calls;
    /* The call waiting for its report, and KWin's unique name: the only sender trusted. */
    char token[TOKEN_BYTES];
    char *kwin;
    bool reported;
    char *report;
    /* KWin's ids of the windows last listed, for activating them by handle. */
    struct window_id ids[WINDOWS_MAX];
    size_t id_count;
    tl_json_token tokens[REPORT_TOKENS];
};
static void receive_report(GDBusConnection *bus, const char *sender, const char *path,
                           const char *interface, const char *method, GVariant *parameters,
                           GDBusMethodInvocation *invocation, gpointer data) {
    struct kwin_source *source = data;
    (void)bus;
    (void)path;
    (void)interface;
    (void)method;
    const char *token = NULL, *report = NULL;
    g_variant_get(parameters, "(&s&s)", &token, &report);
    /* Only the script this call loaded may answer it: reports from anyone but KWin, or
     * late ones from a call that gave up, are dropped. */
    if (!source->reported && g_strcmp0(sender, source->kwin) == 0 &&
        strcmp(token, source->token) == 0) {
        source->report = g_strdup(report);
        source->reported = true;
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}
static const GDBusInterfaceVTable REPORT_VTABLE = {.method_call = receive_report};
/* Milliseconds left until deadline, at least one so a late call still fails quickly. */
static int remaining(gint64 deadline) {
    gint64 left = (deadline - g_get_monotonic_time()) / G_TIME_SPAN_MILLISECOND;
    return left < 1 ? 1 : (int)left;
}
static GVariant *kwin_call(const struct kwin_source *source, const char *path,
                           const char *interface, const char *method, GVariant *arguments,
                           const char *reply, gint64 deadline, GError **error) {
    return g_dbus_connection_call_sync(source->bus, WINDOWS_KWIN_NAME, path, interface, method,
                                       arguments, reply != NULL ? G_VARIANT_TYPE(reply) : NULL,
                                       G_DBUS_CALL_FLAGS_NO_AUTO_START, remaining(deadline), NULL,
                                       error);
}
static bool find_kwin(struct kwin_source *source, gint64 deadline) {
    GVariant *reply = g_dbus_connection_call_sync(
        source->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "GetNameOwner", g_variant_new("(s)", WINDOWS_KWIN_NAME), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, remaining(deadline), NULL, NULL);
    if (reply == NULL)
        return false;
    g_free(source->kwin);
    g_variant_get(reply, "(s)", &source->kwin);
    g_variant_unref(reply);
    return true;
}
static void unload_script(const struct kwin_source *source, gint64 deadline) {
    GVariant *reply = kwin_call(source, SCRIPTING_PATH, SCRIPTING_INTERFACE, "unloadScript",
                                g_variant_new("(s)", source->plugin), "(b)", deadline, NULL);
    if (reply != NULL)
        g_variant_unref(reply);
}
/* Load the script file; its id, or -1. */
static gint32 load_script(const struct kwin_source *source, gint64 deadline) {
    for (int attempt = 0; attempt < 2; attempt++) {
        GVariant *reply =
            kwin_call(source, SCRIPTING_PATH, SCRIPTING_INTERFACE, "loadScript",
                      g_variant_new("(ss)", source->script, source->plugin), "(i)", deadline, NULL);
        if (reply == NULL)
            return -1;
        gint32 id = -1;
        g_variant_get(reply, "(i)", &id);
        g_variant_unref(reply);
        if (id >= 0)
            return id;
        /* KWin refuses a name still loaded, as after a call that gave up: unload, retry. */
        unload_script(source, deadline);
    }
    return -1;
}
static bool unknown_object(const GError *error) {
    return g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT) ||
           g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE) ||
           g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD);
}
static bool run_script(struct kwin_source *source, gint32 id, gint64 deadline) {
    for (int attempt = 0; attempt < 2; attempt++) {
        char *path = source->numbered_paths ? g_strdup_printf("/%d", id)
                                            : g_strdup_printf(SCRIPTING_PATH "/Script%d", id);
        GError *error = NULL;
        GVariant *reply =
            kwin_call(source, path, SCRIPT_INTERFACE, "run", NULL, NULL, deadline, &error);
        g_free(path);
        if (reply != NULL) {
            g_variant_unref(reply);
            return true;
        }
        bool other_version = unknown_object(error);
        g_error_free(error);
        if (!other_version)
            return false;
        source->numbered_paths = !source->numbered_paths;
    }
    return false;
}
static gboolean expire(gpointer data) {
    *(bool *)data = true;
    return G_SOURCE_REMOVE;
}
static bool wait_for_report(struct kwin_source *source, gint64 deadline) {
    bool expired = false;
    GSource *timeout = g_timeout_source_new((guint)remaining(deadline));
    g_source_set_callback(timeout, expire, &expired, NULL);
    g_source_attach(timeout, source->context);
    while (!source->reported && !expired)
        g_main_context_iteration(source->context, true);
    g_source_destroy(timeout);
    g_source_unref(timeout);
    return source->reported;
}
/* Have KWin run text, a script that reports under source->token. Returns the report, which
 * the caller frees, or NULL when KWin did not report in time. */
static char *run_report(struct kwin_source *source, const char *text) {
    gint64 deadline = g_get_monotonic_time() + WINDOWS_KWIN_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND;
    source->reported = false;
    g_clear_pointer(&source->report, g_free);
    if (!g_file_set_contents_full(source->script, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                  NULL) ||
        !find_kwin(source, deadline))
        return NULL;
    gint32 id = load_script(source, deadline);
    bool reported =
        id >= 0 && run_script(source, id, deadline) && wait_for_report(source, deadline);
    /* Unloading gets its own time, so that scripts never pile up in KWin. */
    if (id >= 0)
        unload_script(source,
                      g_get_monotonic_time() + WINDOWS_KWIN_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND);
    return reported ? g_steal_pointer(&source->report) : NULL;
}
/* Start the next call: a new token for its report. */
static void next_call(struct kwin_source *source) {
    (void)snprintf(source->token, sizeof(source->token), "%" G_GUINT64_FORMAT, ++source->calls);
}
/* KWin's window id as a handle: its first 64 bits. Ids are random UUIDs, so two windows
 * practically never share one; zero becomes one. */
static uint64_t id_handle(const char *id) {
    uint64_t handle = 0;
    int digits = 0;
    for (const char *c = id; *c != 0 && digits < HANDLE_DIGITS; c++)
        if (g_ascii_isxdigit(*c)) {
            handle = handle << 4 | (uint64_t)g_ascii_xdigit_value(*c);
            digits++;
        }
    return handle == 0 ? 1 : handle;
}
/* Whether id can go into a script unquoted: hex digits, braces and dashes only. */
static bool safe_id(const char *id) {
    if (id[0] == 0)
        return false;
    for (const char *c = id; *c != 0; c++)
        if (!g_ascii_isxdigit(*c) && *c != '{' && *c != '}' && *c != '-')
            return false;
    return true;
}
/* Decode string token into out, or leave out empty when it is not a string or too long. */
static void read_text(const tl_json *json, size_t token, char *out, size_t capacity) {
    if (json_string(json, token, out, capacity) != TL_OK)
        out[0] = 0;
}
/* A desktop file name as KWin knows it (sometimes a path or with ".desktop") as an id. */
static void desktop_id(char *name) {
    const char *slash = strrchr(name, '/');
    if (slash != NULL)
        memmove(name, slash + 1, strlen(slash + 1) + 1);
    size_t length = strlen(name), suffix = strlen(DESKTOP_SUFFIX);
    if (length > suffix && strcmp(name + length - suffix, DESKTOP_SUFFIX) == 0)
        name[length - suffix] = 0;
}
/* Read report entry (an array of five strings) into out and id; false to skip it. */
static bool read_window(const tl_json *json, size_t entry, bool fold_case, tl_window *out,
                        struct window_id *id) {
    size_t fields[REPORT_FIELDS], count = 0;
    for (size_t token = entry + 1; token < json->tokens[entry].next && count < REPORT_FIELDS;
         token = json->tokens[token].next)
        fields[count++] = token;
    if (json->tokens[entry].type != JSON_ARRAY || count != REPORT_FIELDS)
        return false;
    char title[TITLE_BYTES];
    read_text(json, fields[0], id->id, sizeof(id->id));
    read_text(json, fields[1], title, sizeof(title));
    *out = (tl_window){.fold_case = fold_case};
    read_text(json, fields[2], out->app_id, sizeof(out->app_id));
    read_text(json, fields[3], out->class_name, sizeof(out->class_name));
    read_text(json, fields[4], out->instance, sizeof(out->instance));
    desktop_id(out->app_id);
    if (!safe_id(id->id) || strcmp(out->app_id, LAUNCHER_ID) == 0 ||
        strcmp(out->class_name, LAUNCHER_ID) == 0)
        return false;
    windows_copy_text(out->title, sizeof(out->title), title, strlen(title), false);
    id->handle = out->handle = id_handle(id->id);
    return true;
}
static tl_status kwin_list(void *context, tl_window *out, size_t capacity, size_t *count) {
    struct kwin_source *source = context;
    *count = 0;
    next_call(source);
    char *script = g_strdup_printf(LIST_SCRIPT, WINDOWS_MAX, SCRIPT_TITLE_UNITS,
                                   g_dbus_connection_get_unique_name(source->bus), source->token);
    char *report = run_report(source, script);
    g_free(script);
    tl_json json = {0};
    tl_status status =
        report == NULL ? TL_IO
                       : json_parse(report, strlen(report), source->tokens, REPORT_TOKENS, &json);
    if (status == TL_OK && (json.count == 0 || json.tokens[0].type != JSON_ARRAY))
        status = TL_IO;
    source->id_count = 0;
    for (size_t entry = 1; status == TL_OK && entry < json.tokens[0].next && *count < capacity;
         entry = json.tokens[entry].next)
        /* Plasma 5, which numbers its script objects, lower-cases X11 classes. */
        if (read_window(&json, entry, source->numbered_paths, &out[*count],
                        &source->ids[source->id_count])) {
            (*count)++;
            source->id_count++;
        }
    g_free(report);
    return status == TL_OK ? TL_OK : TL_IO;
}
static tl_status kwin_activate(void *context, uint64_t handle) {
    struct kwin_source *source = context;
    const char *id = NULL;
    for (size_t i = 0; i < source->id_count && id == NULL; i++)
        if (source->ids[i].handle == handle)
            id = source->ids[i].id;
    if (id == NULL)
        return TL_STATE;
    next_call(source);
    char *script = g_strdup_printf(ACTIVATE_SCRIPT, id,
                                   g_dbus_connection_get_unique_name(source->bus), source->token);
    char *report = run_report(source, script);
    g_free(script);
    tl_status status = report == NULL ? TL_IO : strcmp(report, "1") == 0 ? TL_OK : TL_STATE;
    g_free(report);
    return status;
}
static void kwin_destroy(void *context) {
    struct kwin_source *source = context;
    if (source == NULL)
        return;
    if (source->object != 0)
        g_dbus_connection_unregister_object(source->bus, source->object);
    if (source->bus != NULL) {
        (void)g_dbus_connection_close_sync(source->bus, NULL, NULL);
        g_object_unref(source->bus);
    }
    if (source->script != NULL)
        (void)g_remove(source->script);
    g_free(source->script);
    g_free(source->plugin);
    g_free(source->kwin);
    g_free(source->report);
    g_main_context_unref(source->context);
    g_free(source);
}
/* This source's own connection, whose report object dispatches in the source's context. */
static tl_status connect_source(struct kwin_source *source, const char *address) {
    char *owned = address != NULL ? g_strdup(address)
                                  : g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (owned == NULL)
        return TL_IO;
    g_main_context_push_thread_default(source->context);
    source->bus =
        g_dbus_connection_new_for_address_sync(owned,
                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                               NULL, NULL, NULL);
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(REPORT_XML, NULL);
    if (source->bus != NULL && node != NULL)
        source->object = g_dbus_connection_register_object(
            source->bus, REPORT_PATH, node->interfaces[0], &REPORT_VTABLE, source, NULL, NULL);
    g_main_context_pop_thread_default(source->context);
    if (node != NULL)
        g_dbus_node_info_unref(node);
    g_free(owned);
    return source->object != 0 ? TL_OK : TL_IO;
}
tl_status windows_kwin_source(const char *address, const char *folder, tl_window_source *out) {
    if (out == NULL)
        return TL_INVALID;
    *out = (tl_window_source){0};
    struct kwin_source *source = g_try_new0(struct kwin_source, 1);
    if (source == NULL)
        return TL_NOMEM;
    source->context = g_main_context_new();
    /* One script file and KWin script name per launcher process. */
    source->plugin = g_strdup_printf("torchlight-%d", (int)getpid());
    char *file = g_strdup_printf("%s.js", source->plugin);
    source->script =
        g_build_filename(folder != NULL ? folder : g_get_user_runtime_dir(), file, NULL);
    g_free(file);
    tl_status status = connect_source(source, address);
    if (status != TL_OK) {
        kwin_destroy(source);
        return status;
    }
    *out = (tl_window_source){source, kwin_list, kwin_activate, kwin_destroy};
    return TL_OK;
}
