/* Window source over a Wayland compositor's task bar protocol, on GTK's own connection
 * (ADR 0052). The wlroots family announces each window with zwlr_foreign_toplevel_manager_v1;
 * COSMIC announces them with ext_foreign_toplevel_list_v1 and reports their state and takes
 * activations through its own toplevel info and manager. Either way windows_track keeps the
 * windows, so this file only translates protocol events. */
#include "windows_wayland.h"
#include "windows_track.h"
#ifdef GDK_WINDOWING_WAYLAND
#include "cosmic-toplevel-info-unstable-v1-client.h"
#include "cosmic-toplevel-management-unstable-v1-client.h"
#include "ext-foreign-toplevel-list-v1-client.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client.h"
#include <gdk/wayland/gdkwayland.h>
#include <string.h>
/* The launcher's own application id: its popup is never listed as one of its windows. */
#define LAUNCHER_ID "org.torchlight.Launcher"
/* The newest protocol versions this source was written and tested against; a compositor's
 * newer versions are bound at these. COSMIC's info needs version 2, which pairs its state
 * with the standard list's windows (get_cosmic_toplevel). */
#define WLR_VERSION 3U
#define EXT_LIST_VERSION 1U
#define COSMIC_INFO_MIN_VERSION 2U
#define COSMIC_INFO_VERSION 3U
#define COSMIC_MANAGER_VERSION 4U
/* Round trips after binding: the windows' first descriptions, then their COSMIC handles'
 * state, plus one spare for windows announced meanwhile. */
#define START_ROUND_TRIPS 3
/* A protocol global from the registry; version zero when the compositor lacks it. */
typedef struct {
    uint32_t name, version;
} protocol_global;
struct wayland_source {
    GdkDisplay *display;
    struct wl_display *connection;
    /* A private queue while starting, so that GTK's own events are not dispatched under
     * it; NULL afterwards, when everything lives on GTK's default queue. */
    struct wl_event_queue *queue;
    protocol_global wlr_global, list_global, info_global, manager_global;
    struct zwlr_foreign_toplevel_manager_v1 *wlr;
    struct ext_foreign_toplevel_list_v1 *list;
    struct zcosmic_toplevel_info_v1 *info;
    struct zcosmic_toplevel_manager_v1 *manager;
    windows_track *track;
};
/* One window's protocol objects: a wlr handle, or a standard handle and its COSMIC one. */
struct toplevel {
    struct wayland_source *source;
    uint64_t id;
    struct zwlr_foreign_toplevel_handle_v1 *wlr;
    struct ext_foreign_toplevel_handle_v1 *ext;
    struct zcosmic_toplevel_handle_v1 *cosmic;
};
/* Put a new proxy on the queue its source currently uses. Proxies inherit their factory's
 * queue, except those created by events read before the move to GTK's queue. */
static void adopt(const struct wayland_source *source, void *proxy) {
    if (proxy != NULL)
        wl_proxy_set_queue(proxy, source->queue);
}
static struct toplevel *new_toplevel(struct wayland_source *source) {
    struct toplevel *toplevel = g_try_new0(struct toplevel, 1);
    if (toplevel == NULL)
        return NULL;
    toplevel->source = source;
    if (windows_track_add(source->track, toplevel, &toplevel->id) != TL_OK) {
        g_free(toplevel);
        return NULL;
    }
    return toplevel;
}
static void destroy_proxies(struct toplevel *toplevel) {
    if (toplevel->wlr != NULL)
        zwlr_foreign_toplevel_handle_v1_destroy(toplevel->wlr);
    if (toplevel->cosmic != NULL)
        zcosmic_toplevel_handle_v1_destroy(toplevel->cosmic);
    if (toplevel->ext != NULL)
        ext_foreign_toplevel_handle_v1_destroy(toplevel->ext);
}
/* The compositor closed the window: forget it. Destroying a proxy inside its own event is
 * allowed; libwayland keeps it alive until the handler returns. */
static void close_toplevel(struct toplevel *toplevel) {
    windows_track_remove(toplevel->source->track, toplevel->id);
    destroy_proxies(toplevel);
    g_free(toplevel);
}
static bool has_state(const struct wl_array *states, uint32_t wanted) {
    const uint32_t *state = NULL;
    wl_array_for_each(state, states) {
        if (*state == wanted)
            return true;
    }
    return false;
}
static void wlr_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                      const char *title) {
    const struct toplevel *toplevel = data;
    (void)handle;
    windows_track_set_title(toplevel->source->track, toplevel->id, title);
}
static void wlr_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                       const char *app_id) {
    const struct toplevel *toplevel = data;
    (void)handle;
    windows_track_set_app_id(toplevel->source->track, toplevel->id, app_id);
}
static void wlr_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                      struct wl_array *states) {
    const struct toplevel *toplevel = data;
    (void)handle;
    windows_track_set_active(toplevel->source->track, toplevel->id,
                             has_state(states, ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED));
}
static void wlr_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    const struct toplevel *toplevel = data;
    (void)handle;
    windows_track_set_ready(toplevel->source->track, toplevel->id);
}
static void wlr_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)handle;
    close_toplevel(data);
}
/* Outputs and parent windows do not matter for listing. */
static void wlr_output(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                       struct wl_output *output) {
    (void)data;
    (void)handle;
    (void)output;
}
static void wlr_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                       struct zwlr_foreign_toplevel_handle_v1 *parent) {
    (void)data;
    (void)handle;
    (void)parent;
}
static const struct zwlr_foreign_toplevel_handle_v1_listener WLR_HANDLE_LISTENER = {
    .title = wlr_title,
    .app_id = wlr_app_id,
    .output_enter = wlr_output,
    .output_leave = wlr_output,
    .state = wlr_state,
    .done = wlr_done,
    .closed = wlr_closed,
    .parent = wlr_parent,
};
static void wlr_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
                         struct zwlr_foreign_toplevel_handle_v1 *handle) {
    struct wayland_source *source = data;
    (void)manager;
    struct toplevel *toplevel = new_toplevel(source);
    if (toplevel == NULL) {
        /* Out of memory: this window goes unlisted rather than half-tracked. */
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    toplevel->wlr = handle;
    adopt(source, handle);
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &WLR_HANDLE_LISTENER, toplevel);
}
/* The compositor stopped announcing windows (it is shutting down). */
static void wlr_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager) {
    struct wayland_source *source = data;
    zwlr_foreign_toplevel_manager_v1_destroy(manager);
    source->wlr = NULL;
}
static const struct zwlr_foreign_toplevel_manager_v1_listener WLR_MANAGER_LISTENER = {
    .toplevel = wlr_toplevel,
    .finished = wlr_finished,
};
static void ext_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
                      const char *title) {
    const struct toplevel *toplevel = data;
    (void)handle;
    windows_track_set_title(toplevel->source->track, toplevel->id, title);
}
static void ext_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
                       const char *app_id) {
    const struct toplevel *toplevel = data;
    (void)handle;
    windows_track_set_app_id(toplevel->source->track, toplevel->id, app_id);
}
static void ext_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
    const struct toplevel *toplevel = data;
    (void)handle;
    windows_track_set_ready(toplevel->source->track, toplevel->id);
}
static void ext_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)handle;
    close_toplevel(data);
}
/* The compositor's own window identifier; the track numbers windows itself. */
static void ext_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
                           const char *identifier) {
    (void)data;
    (void)handle;
    (void)identifier;
}
static const struct ext_foreign_toplevel_handle_v1_listener EXT_HANDLE_LISTENER = {
    .closed = ext_closed,
    .done = ext_done,
    .title = ext_title,
    .app_id = ext_app_id,
    .identifier = ext_identifier,
};
static void cosmic_state(void *data, struct zcosmic_toplevel_handle_v1 *handle,
                         struct wl_array *states) {
    const struct toplevel *toplevel = data;
    (void)handle;
    windows_track_set_active(toplevel->source->track, toplevel->id,
                             has_state(states, ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED));
}
/* COSMIC's handle only adds the state here: since version 2 the standard handle carries the
 * title, app id, done and closed events, and outputs, workspaces and geometry do not matter. */
static void cosmic_event(void *data, struct zcosmic_toplevel_handle_v1 *handle) {
    (void)data;
    (void)handle;
}
static void cosmic_text(void *data, struct zcosmic_toplevel_handle_v1 *handle, const char *text) {
    (void)data;
    (void)handle;
    (void)text;
}
static void cosmic_output(void *data, struct zcosmic_toplevel_handle_v1 *handle,
                          struct wl_output *output) {
    (void)data;
    (void)handle;
    (void)output;
}
static void cosmic_workspace(void *data, struct zcosmic_toplevel_handle_v1 *handle,
                             struct zcosmic_workspace_handle_v1 *workspace) {
    (void)data;
    (void)handle;
    (void)workspace;
}
static void cosmic_ext_workspace(void *data, struct zcosmic_toplevel_handle_v1 *handle,
                                 struct ext_workspace_handle_v1 *workspace) {
    (void)data;
    (void)handle;
    (void)workspace;
}
static void cosmic_geometry(void *data, struct zcosmic_toplevel_handle_v1 *handle,
                            struct wl_output *output, int32_t x, int32_t y, int32_t width,
                            int32_t height) {
    (void)data;
    (void)handle;
    (void)output;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}
static const struct zcosmic_toplevel_handle_v1_listener COSMIC_HANDLE_LISTENER = {
    .closed = cosmic_event,
    .done = cosmic_event,
    .title = cosmic_text,
    .app_id = cosmic_text,
    .output_enter = cosmic_output,
    .output_leave = cosmic_output,
    .workspace_enter = cosmic_workspace,
    .workspace_leave = cosmic_workspace,
    .state = cosmic_state,
    .geometry = cosmic_geometry,
    .ext_workspace_enter = cosmic_ext_workspace,
    .ext_workspace_leave = cosmic_ext_workspace,
};
static void ext_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
                         struct ext_foreign_toplevel_handle_v1 *handle) {
    struct wayland_source *source = data;
    (void)list;
    struct toplevel *toplevel = new_toplevel(source);
    if (toplevel == NULL) {
        ext_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    toplevel->ext = handle;
    adopt(source, handle);
    ext_foreign_toplevel_handle_v1_add_listener(handle, &EXT_HANDLE_LISTENER, toplevel);
    /* COSMIC's handle for the same window carries its state and takes activations. */
    if (source->info == NULL)
        return;
    toplevel->cosmic = zcosmic_toplevel_info_v1_get_cosmic_toplevel(source->info, handle);
    if (toplevel->cosmic == NULL)
        return;
    adopt(source, toplevel->cosmic);
    zcosmic_toplevel_handle_v1_add_listener(toplevel->cosmic, &COSMIC_HANDLE_LISTENER, toplevel);
}
static void ext_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {
    struct wayland_source *source = data;
    ext_foreign_toplevel_list_v1_destroy(list);
    source->list = NULL;
}
static const struct ext_foreign_toplevel_list_v1_listener EXT_LIST_LISTENER = {
    .toplevel = ext_toplevel,
    .finished = ext_finished,
};
/* Since version 2 the standard list announces the windows, so the info object's own
 * events and the manager's capabilities (activation is always offered) are not needed. */
static void info_toplevel(void *data, struct zcosmic_toplevel_info_v1 *info,
                          struct zcosmic_toplevel_handle_v1 *handle) {
    (void)data;
    (void)info;
    (void)handle;
}
static void info_event(void *data, struct zcosmic_toplevel_info_v1 *info) {
    (void)data;
    (void)info;
}
static const struct zcosmic_toplevel_info_v1_listener COSMIC_INFO_LISTENER = {
    .toplevel = info_toplevel,
    .finished = info_event,
    .done = info_event,
};
static void manager_capabilities(void *data, struct zcosmic_toplevel_manager_v1 *manager,
                                 struct wl_array *capabilities) {
    (void)data;
    (void)manager;
    (void)capabilities;
}
static const struct zcosmic_toplevel_manager_v1_listener COSMIC_MANAGER_LISTENER = {
    .capabilities = manager_capabilities,
};
static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    struct wayland_source *source = data;
    (void)registry;
    protocol_global global = {name, version};
    if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0)
        source->wlr_global = global;
    else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0)
        source->list_global = global;
    else if (strcmp(interface, zcosmic_toplevel_info_v1_interface.name) == 0)
        source->info_global = global;
    else if (strcmp(interface, zcosmic_toplevel_manager_v1_interface.name) == 0)
        source->manager_global = global;
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}
static const struct wl_registry_listener REGISTRY_LISTENER = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};
static void *bind_global(struct wl_registry *registry, protocol_global global,
                         const struct wl_interface *interface, uint32_t newest) {
    return wl_registry_bind(registry, global.name, interface,
                            global.version < newest ? global.version : newest);
}
static tl_status bind_wlr(struct wayland_source *source, struct wl_registry *registry) {
    source->wlr = bind_global(registry, source->wlr_global,
                              &zwlr_foreign_toplevel_manager_v1_interface, WLR_VERSION);
    if (source->wlr == NULL)
        return TL_NOMEM;
    zwlr_foreign_toplevel_manager_v1_add_listener(source->wlr, &WLR_MANAGER_LISTENER, source);
    return TL_OK;
}
static tl_status bind_cosmic(struct wayland_source *source, struct wl_registry *registry) {
    /* The info object must exist before the list announces its first window. */
    source->info = bind_global(registry, source->info_global, &zcosmic_toplevel_info_v1_interface,
                               COSMIC_INFO_VERSION);
    source->manager = bind_global(registry, source->manager_global,
                                  &zcosmic_toplevel_manager_v1_interface, COSMIC_MANAGER_VERSION);
    if (source->info == NULL || source->manager == NULL)
        return TL_NOMEM;
    zcosmic_toplevel_info_v1_add_listener(source->info, &COSMIC_INFO_LISTENER, source);
    zcosmic_toplevel_manager_v1_add_listener(source->manager, &COSMIC_MANAGER_LISTENER, source);
    source->list = bind_global(registry, source->list_global,
                               &ext_foreign_toplevel_list_v1_interface, EXT_LIST_VERSION);
    if (source->list == NULL)
        return TL_NOMEM;
    ext_foreign_toplevel_list_v1_add_listener(source->list, &EXT_LIST_LISTENER, source);
    return TL_OK;
}
/* Bind the wlroots manager, or COSMIC's three objects; TL_STATE when neither is complete. */
static tl_status bind_protocol(struct wayland_source *source, struct wl_registry *registry) {
    if (source->wlr_global.version != 0)
        return bind_wlr(source, registry);
    if (source->list_global.version == 0 || source->manager_global.version == 0 ||
        source->info_global.version < COSMIC_INFO_MIN_VERSION)
        return TL_STATE;
    return bind_cosmic(source, registry);
}
/* Move every proxy onto the source's current queue (GTK's default one, once started). */
static void adopt_all(const struct wayland_source *source) {
    adopt(source, source->wlr);
    adopt(source, source->list);
    adopt(source, source->info);
    adopt(source, source->manager);
    for (size_t i = 0;; i++) {
        const struct toplevel *toplevel = windows_track_data_at(source->track, i);
        if (toplevel == NULL)
            break;
        adopt(source, toplevel->wlr);
        adopt(source, toplevel->ext);
        adopt(source, toplevel->cosmic);
    }
}
/* Read the current windows on a private queue, then hand everything to GTK's queue. */
static tl_status start(struct wayland_source *source) {
    struct wl_event_queue *queue = wl_display_create_queue(source->connection);
    struct wl_display *wrapper = queue != NULL ? wl_proxy_create_wrapper(source->connection) : NULL;
    if (wrapper == NULL) {
        if (queue != NULL)
            wl_event_queue_destroy(queue);
        return TL_NOMEM;
    }
    source->queue = queue;
    wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);
    struct wl_registry *registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    tl_status status = TL_NOMEM;
    if (registry != NULL) {
        wl_registry_add_listener(registry, &REGISTRY_LISTENER, source);
        status = wl_display_roundtrip_queue(source->connection, queue) < 0
                     ? TL_IO
                     : bind_protocol(source, registry);
        for (int i = 0; status == TL_OK && i < START_ROUND_TRIPS; i++)
            if (wl_display_roundtrip_queue(source->connection, queue) < 0)
                status = TL_IO;
        wl_registry_destroy(registry);
    }
    /* Events already read onto the private queue are dispatched here; any window they
     * announce is adopted onto GTK's queue at once. The queue can then go. */
    source->queue = NULL;
    adopt_all(source);
    (void)wl_display_dispatch_queue_pending(source->connection, queue);
    adopt_all(source);
    wl_event_queue_destroy(queue);
    return status;
}
static tl_status wayland_list(void *context, tl_window *out, size_t capacity, size_t *count) {
    const struct wayland_source *source = context;
    *count = windows_track_list(source->track, LAUNCHER_ID, out, capacity);
    return TL_OK;
}
static tl_status wayland_activate(void *context, uint64_t handle) {
    const struct wayland_source *source = context;
    const struct toplevel *toplevel = windows_track_find(source->track, handle);
    GdkSeat *seat = gdk_display_get_default_seat(source->display);
    if (toplevel == NULL || seat == NULL || !GDK_IS_WAYLAND_SEAT(seat))
        return TL_STATE;
    struct wl_seat *wl_seat = gdk_wayland_seat_get_wl_seat(seat);
    if (toplevel->wlr != NULL)
        zwlr_foreign_toplevel_handle_v1_activate(toplevel->wlr, wl_seat);
    else if (toplevel->cosmic != NULL && source->manager != NULL)
        zcosmic_toplevel_manager_v1_activate(source->manager, toplevel->cosmic, wl_seat);
    else
        return TL_STATE;
    /* A full socket buffer is flushed later by GTK; only a broken connection fails. */
    (void)wl_display_flush(source->connection);
    return wl_display_get_error(source->connection) == 0 ? TL_OK : TL_IO;
}
static void wayland_destroy(void *context) {
    struct wayland_source *source = context;
    if (source == NULL)
        return;
    for (struct toplevel *toplevel = windows_track_data_at(source->track, 0); toplevel != NULL;
         toplevel = windows_track_data_at(source->track, 0)) {
        windows_track_remove(source->track, toplevel->id);
        destroy_proxies(toplevel);
        g_free(toplevel);
    }
    if (source->wlr != NULL) {
        zwlr_foreign_toplevel_manager_v1_stop(source->wlr);
        zwlr_foreign_toplevel_manager_v1_destroy(source->wlr);
    }
    if (source->list != NULL) {
        ext_foreign_toplevel_list_v1_stop(source->list);
        ext_foreign_toplevel_list_v1_destroy(source->list);
    }
    if (source->info != NULL)
        zcosmic_toplevel_info_v1_destroy(source->info);
    /* Only forgotten here, never destroyed: COSMIC's compositor has no case for the manager's
     * destroy request and stops the whole session on it (cosmic-comp up to 2026-10, at least).
     * It releases the object when the launcher disconnects. */
    if (source->manager != NULL)
        wl_proxy_destroy((struct wl_proxy *)source->manager);
    (void)wl_display_flush(source->connection);
    windows_track_destroy(source->track);
    g_object_unref(source->display);
    g_free(source);
}
/* Whether the compositor offers a protocol this source speaks, from the globals GDK has
 * already seen, so other desktops cost no round trip. */
static bool offers_protocol(GdkDisplay *display) {
    return gdk_wayland_display_query_registry(display, "zwlr_foreign_toplevel_manager_v1") ||
           (gdk_wayland_display_query_registry(display, "ext_foreign_toplevel_list_v1") &&
            gdk_wayland_display_query_registry(display, "zcosmic_toplevel_info_v1") &&
            gdk_wayland_display_query_registry(display, "zcosmic_toplevel_manager_v1"));
}
tl_status windows_wayland_source(GdkDisplay *display, tl_window_source *out) {
    if (out == NULL || display == NULL)
        return TL_INVALID;
    *out = (tl_window_source){0};
    if (!GDK_IS_WAYLAND_DISPLAY(display) || !offers_protocol(display))
        return TL_STATE;
    struct wayland_source *source = g_try_new0(struct wayland_source, 1);
    if (source == NULL)
        return TL_NOMEM;
    source->display = g_object_ref(display);
    source->connection = gdk_wayland_display_get_wl_display(display);
    tl_status status = windows_track_create(&source->track);
    if (status == TL_OK)
        status = start(source);
    if (status != TL_OK) {
        wayland_destroy(source);
        return status;
    }
    *out = (tl_window_source){source, wayland_list, wayland_activate, wayland_destroy};
    return TL_OK;
}
#else
tl_status windows_wayland_source(GdkDisplay *display, tl_window_source *out) {
    (void)display;
    if (out == NULL)
        return TL_INVALID;
    *out = (tl_window_source){0};
    return TL_STATE;
}
#endif
