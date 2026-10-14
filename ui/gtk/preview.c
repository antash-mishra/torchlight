/* Preview panel (ADR 0048): beside the results on a wide popup, the selected result's
 * icon, name and kind, then what Torchlight knows about it (a file's size, modified time
 * and folder, a folder's items, an application's description and open windows) and the
 * keys that act on it. When the selection rests on a new result its lines rise one after
 * another, like a teletext page arriving. Details that need the disk are read on a
 * worker thread, so neither typing nor arrow keys ever wait for them. */
#include "preview.h"
#include <gio/gdesktopappinfo.h>
#include <stdio.h>
#include <string.h>
/* A new result waits for the selection to rest, so holding an arrow key reads nothing
 * for the rows it passes. */
#define REST_MS 70
#define LINE_SPACING 14
#define HEAD_SPACING 14
#define TITLE_SPACING 4
#define ENTRY_SPACING 10
#define LIST_SPACING 6
#define ICON_SIZE 48
/* Lines past this share the last rise delay. */
#define RISE_STEPS 8U
/* Fact names take this many characters so their values line up (Bedstead is fixed width). */
#define FACT_NAME_CHARS 9
/* About the panel's text width in characters at the body size. */
#define WRAP_CHARS 28
/* A folder's items are counted up to this many; more shows as "999+". */
#define ITEMS_MAX 999
#define COUNT_BYTES 48
/* What the worker reads; each fills one label. */
typedef enum {
    DETAIL_TYPE,        /* a file's type, from its name ("Markdown document") */
    DETAIL_DESCRIPTION, /* an application's description, from its desktop entry */
    DETAIL_SIZE,
    DETAIL_MODIFIED,
    DETAIL_ITEMS, /* a folder's entries, hidden ones aside */
    DETAILS
} detail;
/* One preview, owned: the item, its result and the application's windows. */
typedef struct {
    tl_popup_row row;
    tl_popup_item_kind kind;
    char *windows[POPUP_WINDOWS_SHOWN];
    size_t window_count, windows_open;
    GIcon *icon;
} content;
/* A worker's request and findings; its task owns it. */
typedef struct {
    char *path, *name;
    bool application, folder;
    char *found[DETAILS];
} reading;
typedef struct {
    GtkWidget parent;
    /* Rows are too large for a GObject instance, so both previews live on the heap. */
    content *shown, *next;
    bool showing;
    /* What the worker found for shown; NULL while unread or unknown. */
    char *details[DETAILS];
    bool read;
    /* The labels details fill and the lines left out when one is unknown; NULL if absent. */
    GtkWidget *values[DETAILS], *lines[DETAILS];
    guint rest;
    GCancellable *cancel;
} TlPopupPreviewPanel;
typedef struct {
    GtkWidgetClass parent;
} TlPopupPreviewPanelClass;
G_DEFINE_TYPE(TlPopupPreviewPanel, tl_popup_preview_panel, GTK_TYPE_WIDGET)
/* One action the selected item takes: its colour block, name and keys. */
typedef struct {
    const char *colour, *name, *keys;
} action;
static void remove_timer(guint *timer) {
    if (*timer != 0)
        g_source_remove(*timer);
    *timer = 0;
}
static void content_clear(content *preview) {
    for (size_t i = 0; i < preview->window_count; i++)
        g_clear_pointer(&preview->windows[i], g_free);
    preview->window_count = 0;
    g_clear_object(&preview->icon);
}
static void content_set(content *preview, const tl_popup_preview *source, GIcon *icon) {
    content_clear(preview);
    preview->row = *source->row;
    preview->kind = source->kind;
    preview->window_count = MIN(source->window_count, (size_t)POPUP_WINDOWS_SHOWN);
    for (size_t i = 0; i < preview->window_count; i++)
        preview->windows[i] = g_strdup(source->windows[i] != NULL ? source->windows[i] : "");
    preview->windows_open = MAX(source->windows_open, preview->window_count);
    preview->icon = icon != NULL ? g_object_ref(icon) : NULL;
}
static GtkWidget *text(const char *value, const char *style_class) {
    GtkWidget *label = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_add_css_class(label, style_class);
    return label;
}
/* Wrapping labels ask for about the panel's width in characters. They wrap at whatever
 * width the panel gives them; the cap keeps long text from widening it, and a height
 * measured without a width (GTK sizes Wayland windows that way) stays near the real one
 * instead of folding the text one character per line. */
static GtkWidget *wrapping(const char *value, const char *style_class) {
    GtkWidget *label = text(value, style_class);
    gtk_label_set_wrap(GTK_LABEL(label), true);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), WRAP_CHARS);
    return label;
}
/* A teletext colour block; decoration, so assistive technology skips it. */
static GtkWidget *block(const char *colour) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "block");
    gtk_widget_add_css_class(box, colour);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_accessible_update_state(GTK_ACCESSIBLE(box), GTK_ACCESSIBLE_STATE_HIDDEN, true, -1);
    return box;
}
/* The section headers' colour and a word for what row is. */
static const char *kind_colour(const tl_popup_row *row) {
    return row->settings ? "magenta" : row->application ? "cyan" : row->folder ? "yellow" : "blue";
}
static const char *kind_word(const tl_popup_row *row) {
    return row->settings      ? "Settings panel"
           : row->application ? "Application"
           : row->folder      ? "Folder"
                              : "File";
}
/* The icon, then the name over its kind; a file's kind becomes its type once read. */
static GtkWidget *head(TlPopupPreviewPanel *self) {
    const content *shown = self->shown;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, HEAD_SPACING);
    GtkWidget *image = shown->icon != NULL ? gtk_image_new_from_gicon(shown->icon)
                                           : gtk_image_new_from_icon_name("text-x-generic");
    gtk_image_set_pixel_size(GTK_IMAGE(image), ICON_SIZE);
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    gtk_accessible_update_state(GTK_ACCESSIBLE(image), GTK_ACCESSIBLE_STATE_HIDDEN, true, -1);
    gtk_box_append(GTK_BOX(box), image);
    GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, TITLE_SPACING);
    gtk_widget_set_hexpand(titles, true);
    gtk_widget_set_valign(titles, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(titles), wrapping(shown->row.name, "preview-name"));
    GtkWidget *kind = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, ENTRY_SPACING);
    gtk_widget_add_css_class(kind, "preview-kind");
    gtk_box_append(GTK_BOX(kind), block(kind_colour(&shown->row)));
    self->values[DETAIL_TYPE] = wrapping(kind_word(&shown->row), "preview-type");
    gtk_widget_set_hexpand(self->values[DETAIL_TYPE], true);
    gtk_box_append(GTK_BOX(kind), self->values[DETAIL_TYPE]);
    gtk_box_append(GTK_BOX(titles), kind);
    gtk_box_append(GTK_BOX(box), titles);
    return box;
}
/* One fact: its name, then its value with style_class. which names the detail the
 * worker fills, or DETAILS when value is already known. */
static GtkWidget *fact(TlPopupPreviewPanel *self, const char *name, const char *style_class,
                       detail which, const char *value) {
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, ENTRY_SPACING);
    GtkWidget *label = text(name, "fact-name");
    gtk_label_set_width_chars(GTK_LABEL(label), FACT_NAME_CHARS);
    gtk_widget_set_valign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(line), label);
    GtkWidget *shown = wrapping(value != NULL ? value : "", style_class);
    gtk_widget_add_css_class(shown, "fact-value");
    gtk_widget_set_hexpand(shown, true);
    gtk_box_append(GTK_BOX(line), shown);
    if (which != DETAILS) {
        self->values[which] = shown;
        self->lines[which] = line;
    }
    return line;
}
/* The folder part of a displayed path: "~/docs/a.md" gives "~/docs" and "/a" gives "/". */
static void parent_of(const char *display, char *out, size_t size) {
    g_strlcpy(out, display, size);
    char *slash = strrchr(out, '/');
    if (slash != NULL)
        slash[slash == out ? 1 : 0] = 0;
}
/* A file's size, modified time and folder, or a folder's items, modified time and place. */
static GtkWidget *facts(TlPopupPreviewPanel *self) {
    const tl_popup_row *row = &self->shown->row;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, LIST_SPACING);
    gtk_widget_add_css_class(box, "preview-facts");
    if (row->folder) {
        gtk_box_append(GTK_BOX(box), fact(self, "Items", "preview-items", DETAIL_ITEMS, NULL));
        gtk_box_append(GTK_BOX(box),
                       fact(self, "Modified", "preview-modified", DETAIL_MODIFIED, NULL));
        gtk_box_append(GTK_BOX(box), fact(self, "Where", "preview-where", DETAILS, row->display));
        return box;
    }
    char folder[POPUP_PATH_BYTES];
    parent_of(row->display, folder, sizeof(folder));
    gtk_box_append(GTK_BOX(box), fact(self, "Size", "preview-size", DETAIL_SIZE, NULL));
    gtk_box_append(GTK_BOX(box), fact(self, "Modified", "preview-modified", DETAIL_MODIFIED, NULL));
    gtk_box_append(GTK_BOX(box), fact(self, "Folder", "preview-folder", DETAILS, folder));
    return box;
}
/* A list line: a colour block, its text and, for an action, its keys. */
static GtkWidget *entry(const char *colour, const char *name, const char *keys) {
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, ENTRY_SPACING);
    gtk_box_append(GTK_BOX(line), block(colour));
    GtkWidget *label = text(name, "preview-entry");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, true);
    gtk_box_append(GTK_BOX(line), label);
    if (keys != NULL)
        gtk_box_append(GTK_BOX(line), text(keys, "preview-keys"));
    return line;
}
/* An application's open windows as the list shows them, then how many it leaves out. */
static GtkWidget *windows(const content *shown) {
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, LIST_SPACING);
    for (size_t i = 0; i < shown->window_count; i++)
        gtk_box_append(GTK_BOX(list), entry("green", shown->windows[i], NULL));
    size_t more = shown->windows_open - shown->window_count;
    char count[COUNT_BYTES];
    int written = snprintf(count, sizeof(count), "%zu %s", more,
                           shown->window_count != 0 ? "more"
                           : more == 1              ? "window"
                                                    : "windows");
    if (more != 0 && written > 0 && (size_t)written < sizeof(count))
        gtk_box_append(GTK_BOX(list), text(count, "preview-more"));
    return list;
}
/* The keys that act on the selected item, as the launcher handles them. */
static size_t item_actions(const content *shown, action out[3]) {
    if (shown->kind != POPUP_ITEM_RESULT) {
        out[0] = (action){"red",
                          shown->kind == POPUP_ITEM_WINDOW         ? "Switch to window"
                          : shown->kind == POPUP_ITEM_MORE_WINDOWS ? "Show more windows"
                                                                   : "Open a new window",
                          "Enter"};
        return 1;
    }
    out[0] = (action){"red", "Open", "Enter"};
    if (shown->row.settings)
        return 1;
    if (shown->row.application) {
        /* Enter on a running application switches to its most recent window. */
        if (shown->windows_open != 0)
            out[0].name = "Switch to window";
        out[1] = (action){"green", "Show desktop file", "Ctrl Enter"};
        return 2;
    }
    out[1] = (action){"green", "Show in folder", "Ctrl Enter"};
    out[2] = (action){"yellow", "Copy path", "Ctrl C"};
    return 3;
}
static GtkWidget *actions(const content *shown) {
    action list[3];
    size_t count = item_actions(shown, list);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, LIST_SPACING);
    for (size_t i = 0; i < count; i++)
        gtk_box_append(GTK_BOX(box), entry(list[i].colour, list[i].name, list[i].keys));
    return box;
}
static GtkWidget *rule(void) {
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(line, "preview-rule");
    gtk_accessible_update_state(GTK_ACCESSIBLE(line), GTK_ACCESSIBLE_STATE_HIDDEN, true, -1);
    return line;
}
/* Append line; a newly arrived preview's lines rise one after another. */
static GtkWidget *add_line(TlPopupPreviewPanel *self, GtkWidget *line, bool arriving,
                           size_t *index) {
    gtk_widget_add_css_class(line, "preview-line");
    char rise[16];
    int written = snprintf(rise, sizeof(rise), "rise-%zu", MIN(*index, RISE_STEPS - 1));
    if (arriving && written > 0 && (size_t)written < sizeof(rise))
        gtk_widget_add_css_class(line, rise);
    (*index)++;
    gtk_widget_set_parent(line, GTK_WIDGET(self));
    return line;
}
static void remove_lines(TlPopupPreviewPanel *self) {
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self)); child != NULL;
         child = gtk_widget_get_first_child(GTK_WIDGET(self)))
        gtk_widget_unparent(child);
    memset(self->values, 0, sizeof(self->values));
    memset(self->lines, 0, sizeof(self->lines));
}
/* Put the details read so far into their labels. Until the worker finishes, facts wait
 * with empty values and the description stays hidden; then unknown ones are left out. */
static void fill(TlPopupPreviewPanel *self) {
    for (size_t i = 0; i < DETAILS; i++) {
        if (self->values[i] == NULL)
            continue;
        if (self->details[i] != NULL)
            gtk_label_set_text(GTK_LABEL(self->values[i]), self->details[i]);
        if (self->lines[i] != NULL)
            gtk_widget_set_visible(self->lines[i], self->details[i] != NULL ||
                                                       (!self->read && i != DETAIL_DESCRIPTION));
    }
}
static void draw(TlPopupPreviewPanel *self, bool arriving) {
    remove_lines(self);
    const content *shown = self->shown;
    size_t index = 0;
    add_line(self, head(self), arriving, &index);
    if (shown->row.application) {
        GtkWidget *description = wrapping("", "preview-description");
        self->values[DETAIL_DESCRIPTION] = description;
        self->lines[DETAIL_DESCRIPTION] = add_line(self, description, arriving, &index);
    } else
        add_line(self, facts(self), arriving, &index);
    if (shown->row.application && shown->windows_open != 0) {
        add_line(self, text("Open windows", "preview-label"), arriving, &index);
        add_line(self, windows(shown), arriving, &index);
    }
    add_line(self, rule(), arriving, &index);
    add_line(self, text("Actions", "preview-label"), arriving, &index);
    add_line(self, actions(shown), arriving, &index);
    fill(self);
}
/* A file type's description from its name alone, capitalized, or NULL when the name
 * says nothing. GIO reads each type's description from the MIME database once. */
static char *type_text(const char *name) {
    gboolean uncertain = false;
    char *type = g_content_type_guess(name, NULL, 0, &uncertain);
    char *description = uncertain || type == NULL ? NULL : g_content_type_get_description(type);
    g_free(type);
    if (description != NULL && g_ascii_islower(description[0]))
        description[0] = g_ascii_toupper(description[0]);
    return description;
}
/* The modified time as teletext prints it: "13 Oct 21:40", or "2 Oct 2025" for an
 * earlier year. */
static char *modified_text(GFileInfo *info) {
    if (!g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
        return NULL;
    GDateTime *utc = g_file_info_get_modification_date_time(info);
    GDateTime *local = utc != NULL ? g_date_time_to_local(utc) : NULL;
    GDateTime *now = g_date_time_new_now_local();
    char *formatted = NULL;
    if (local != NULL && now != NULL)
        formatted = g_date_time_format(
            local, g_date_time_get_year(local) == g_date_time_get_year(now) ? "%-d %b %H:%M"
                                                                            : "%-d %b %Y");
    if (now != NULL)
        g_date_time_unref(now);
    if (local != NULL)
        g_date_time_unref(local);
    if (utc != NULL)
        g_date_time_unref(utc);
    return formatted;
}
/* How many entries a file manager shows in folder (names without a leading dot), up to
 * ITEMS_MAX; NULL when it cannot be listed. Asks for names only, which GIO lists without
 * a stat per entry. */
static char *items_text(GFile *folder, GCancellable *cancel) {
    GError *error = NULL;
    GFileEnumerator *entries = g_file_enumerate_children(folder, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                         G_FILE_QUERY_INFO_NONE, cancel, NULL);
    if (entries == NULL)
        return NULL;
    size_t count = 0;
    while (count <= ITEMS_MAX) {
        GFileInfo *info = g_file_enumerator_next_file(entries, cancel, &error);
        if (info == NULL)
            break;
        if (g_file_info_get_name(info)[0] != '.')
            count++;
        g_object_unref(info);
    }
    g_object_unref(entries);
    bool failed = error != NULL;
    g_clear_error(&error);
    if (failed)
        return NULL;
    return count > ITEMS_MAX ? g_strdup_printf("%d+", ITEMS_MAX) : g_strdup_printf("%zu", count);
}
/* An application's description from its desktop entry, or NULL. */
static char *description_text(const char *path) {
    GDesktopAppInfo *info = g_desktop_app_info_new_from_filename(path);
    if (info == NULL)
        return NULL;
    const char *description = g_app_info_get_description(G_APP_INFO(info));
    char *found =
        description != NULL && description[0] != 0 ? g_utf8_make_valid(description, -1) : NULL;
    g_object_unref(info);
    return found;
}
static void read_file(reading *job, GCancellable *cancel) {
    GFile *file = g_file_new_for_path(job->path);
    GFileInfo *info =
        g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
                          G_FILE_QUERY_INFO_NONE, cancel, NULL);
    if (info != NULL) {
        if (!job->folder && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
            job->found[DETAIL_SIZE] = g_format_size((guint64)MAX(0, g_file_info_get_size(info)));
        job->found[DETAIL_MODIFIED] = modified_text(info);
        g_object_unref(info);
    }
    if (job->folder)
        job->found[DETAIL_ITEMS] = items_text(file, cancel);
    else
        job->found[DETAIL_TYPE] = type_text(job->name);
    g_object_unref(file);
}
/* GTask worker: reads the details for one preview into its reading. */
static void read_details(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    reading *job = data;
    if (job->application)
        job->found[DETAIL_DESCRIPTION] = description_text(job->path);
    else
        read_file(job, cancel);
    g_task_return_boolean(task, true);
}
static void reading_free(gpointer data) {
    reading *job = data;
    g_free(job->path);
    g_free(job->name);
    for (size_t i = 0; i < DETAILS; i++)
        g_free(job->found[i]);
    g_free(job);
}
static void details_read(GObject *source, GAsyncResult *result, gpointer context) {
    (void)context;
    TlPopupPreviewPanel *self = (TlPopupPreviewPanel *)source;
    GTask *task = G_TASK(result);
    /* A later show, clear or dispose cancelled this read; its preview is gone. */
    if (g_cancellable_is_cancelled(g_task_get_cancellable(task)))
        return;
    reading *job = g_task_get_task_data(task);
    for (size_t i = 0; i < DETAILS; i++) {
        g_free(self->details[i]);
        self->details[i] = g_steal_pointer(&job->found[i]);
    }
    self->read = true;
    fill(self);
}
static void stop_reading(TlPopupPreviewPanel *self) {
    if (self->cancel != NULL)
        g_cancellable_cancel(self->cancel);
    g_clear_object(&self->cancel);
}
static void forget_details(TlPopupPreviewPanel *self) {
    for (size_t i = 0; i < DETAILS; i++)
        g_clear_pointer(&self->details[i], g_free);
    self->read = false;
}
/* Read the shown preview's details on a worker thread; the task keeps the panel alive. */
static void start_reading(TlPopupPreviewPanel *self) {
    stop_reading(self);
    forget_details(self);
    const tl_popup_row *row = &self->shown->row;
    if (row->path[0] == 0) {
        self->read = true;
        return;
    }
    reading *job = g_new0(reading, 1);
    job->path = g_strdup(row->path);
    job->name = g_strdup(row->name);
    job->application = row->application;
    job->folder = row->folder;
    self->cancel = g_cancellable_new();
    GTask *task = g_task_new(self, self->cancel, details_read, NULL);
    g_task_set_task_data(task, job, reading_free);
    g_task_run_in_thread(task, read_details);
    g_object_unref(task);
}
static void swap(TlPopupPreviewPanel *self) {
    content *previous = self->shown;
    self->shown = self->next;
    self->next = previous;
}
/* The waiting preview takes the panel: its lines rise in and its details are read. */
static void present(TlPopupPreviewPanel *self) {
    swap(self);
    self->showing = true;
    start_reading(self);
    draw(self, true);
}
static gboolean rested(gpointer context) {
    TlPopupPreviewPanel *self = context;
    self->rest = 0;
    present(self);
    return G_SOURCE_REMOVE;
}
static bool same_result(const tl_popup_row *a, const tl_popup_row *b) {
    return a->id == b->id && a->desktop_revision == b->desktop_revision &&
           strcmp(a->path, b->path) == 0;
}
/* Whether a and b describe the same item of the same result with the same windows. */
static bool same_content(const content *a, const content *b) {
    if (!same_result(&a->row, &b->row) || a->kind != b->kind ||
        a->window_count != b->window_count || a->windows_open != b->windows_open ||
        !g_icon_equal(a->icon, b->icon))
        return false;
    for (size_t i = 0; i < a->window_count; i++)
        if (strcmp(a->windows[i], b->windows[i]) != 0)
            return false;
    return true;
}
static GtkWidget *find_label(GtkWidget *widget, const char *style_class) {
    if (GTK_IS_LABEL(widget) && gtk_widget_has_css_class(widget, style_class))
        return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_label(child, style_class);
        if (found != NULL)
            return found;
    }
    return NULL;
}
/* Whether widget and its ancestors below panel are all shown. */
static bool shown_within(GtkWidget *widget, GtkWidget *panel) {
    for (; widget != NULL && widget != panel; widget = gtk_widget_get_parent(widget))
        if (!gtk_widget_get_visible(widget))
            return false;
    return true;
}
static void dispose(GObject *object) {
    TlPopupPreviewPanel *self = (TlPopupPreviewPanel *)object;
    remove_timer(&self->rest);
    stop_reading(self);
    remove_lines(self);
    G_OBJECT_CLASS(tl_popup_preview_panel_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
    TlPopupPreviewPanel *self = (TlPopupPreviewPanel *)object;
    forget_details(self);
    content_clear(self->shown);
    content_clear(self->next);
    g_free(self->shown);
    g_free(self->next);
    G_OBJECT_CLASS(tl_popup_preview_panel_parent_class)->finalize(object);
}
static void tl_popup_preview_panel_class_init(TlPopupPreviewPanelClass *class) {
    GtkWidgetClass *widget = GTK_WIDGET_CLASS(class);
    gtk_widget_class_set_layout_manager_type(widget, GTK_TYPE_BOX_LAYOUT);
    gtk_widget_class_set_accessible_role(widget, GTK_ACCESSIBLE_ROLE_GROUP);
    G_OBJECT_CLASS(class)->dispose = dispose;
    G_OBJECT_CLASS(class)->finalize = finalize;
}
static void tl_popup_preview_panel_init(TlPopupPreviewPanel *self) {
    gtk_widget_add_css_class(GTK_WIDGET(self), "preview-page");
    GtkLayoutManager *layout = gtk_widget_get_layout_manager(GTK_WIDGET(self));
    gtk_orientable_set_orientation(GTK_ORIENTABLE(layout), GTK_ORIENTATION_VERTICAL);
    gtk_box_layout_set_spacing(GTK_BOX_LAYOUT(layout), LINE_SPACING);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self), GTK_ACCESSIBLE_PROPERTY_LABEL, "Details",
                                   -1);
    self->shown = g_new0(content, 1);
    self->next = g_new0(content, 1);
}
GtkWidget *popup_preview_new(void) {
    return g_object_new(tl_popup_preview_panel_get_type(), NULL);
}
void popup_preview_show(GtkWidget *panel, const tl_popup_preview *preview, GIcon *icon) {
    TlPopupPreviewPanel *self = (TlPopupPreviewPanel *)panel;
    if (preview == NULL || preview->row == NULL) {
        popup_preview_clear(panel);
        return;
    }
    remove_timer(&self->rest);
    content_set(self->next, preview, icon);
    if (self->showing && same_result(&self->shown->row, preview->row)) {
        /* The result shown. Typing that keeps the top result changes nothing, so nothing is
         * rebuilt; another item of it, or its windows, redraw in place and keep the details
         * already read. */
        if (same_content(self->shown, self->next))
            return;
        swap(self);
        draw(self, false);
        return;
    }
    self->rest = g_timeout_add(REST_MS, rested, self);
}
void popup_preview_clear(GtkWidget *panel) {
    TlPopupPreviewPanel *self = (TlPopupPreviewPanel *)panel;
    remove_timer(&self->rest);
    stop_reading(self);
    forget_details(self);
    remove_lines(self);
    self->showing = false;
}
void popup_preview_flush(GtkWidget *panel) {
    TlPopupPreviewPanel *self = (TlPopupPreviewPanel *)panel;
    if (self->rest == 0)
        return;
    remove_timer(&self->rest);
    present(self);
}
const char *popup_preview_text(GtkWidget *panel, const char *style_class) {
    GtkWidget *label = find_label(panel, style_class);
    return label != NULL && shown_within(label, panel) ? gtk_label_get_text(GTK_LABEL(label))
                                                       : NULL;
}
