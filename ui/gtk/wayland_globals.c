/* Wayland globals the compositor offers, read on a private queue. */
#include "wayland_globals.h"
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#include <string.h>
struct global_query {
    const char *interface;
    uint32_t version;
};
static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    struct global_query *query = data;
    (void)registry;
    (void)name;
    if (strcmp(interface, query->interface) == 0)
        query->version = version;
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
uint32_t wayland_global_version(GdkDisplay *display, const char *interface) {
    if (display == NULL || interface == NULL || !GDK_IS_WAYLAND_DISPLAY(display) ||
        !gdk_wayland_display_query_registry(display, interface))
        return 0;
    struct wl_display *connection = gdk_wayland_display_get_wl_display(display);
    struct wl_event_queue *queue = wl_display_create_queue(connection);
    struct wl_display *wrapper = queue != NULL ? wl_proxy_create_wrapper(connection) : NULL;
    struct global_query query = {interface, 0};
    if (wrapper != NULL) {
        wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);
        struct wl_registry *registry = wl_display_get_registry(wrapper);
        wl_proxy_wrapper_destroy(wrapper);
        if (registry != NULL) {
            wl_registry_add_listener(registry, &REGISTRY_LISTENER, &query);
            (void)wl_display_roundtrip_queue(connection, queue);
            wl_registry_destroy(registry);
        }
    }
    if (queue != NULL)
        wl_event_queue_destroy(queue);
    return query.version;
}
#else
uint32_t wayland_global_version(GdkDisplay *display, const char *interface) {
    (void)display;
    (void)interface;
    return 0;
}
#endif
