/* Private GTK widget: Tekst's teletext page header (page number, name, search time and
 * clock). */
#ifndef TORCHLIGHT_GTK_HEADER_H
#define TORCHLIGHT_GTK_HEADER_H
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/** Create a floating header: "P100", "Torchlight", how long the search took and a clock in
 * teletext's slash-seconds format. Main thread; the caller parents it. It is hidden from
 * assistive technology, which hears the result count through announcements. */
GtkWidget *popup_header_new(void);
/** Show how long the search took, given in microseconds ("3.2ms", "140ms"; "<0.1ms"
 * below 50 microseconds), or nothing for POPUP_SEARCH_TIME_UNKNOWN. The field keeps the
 * width of the longest time, so the header does not shift as it changes. Main thread;
 * header from popup_header_new. No errors or ownership transfer. */
void popup_header_set_search_time(GtkWidget *header, uint64_t micros);
/** With widest, show the clock at its widest (a two-digit day's last second), so that
 * measuring the header tells whether its widest form fits; without, put its own text back.
 * Main thread; header from popup_header_new. No errors. */
void popup_header_set_widest(GtkWidget *header, bool widest);
/** Show or hide the search time field (shown by default), for headers too narrow to keep
 * it. Main thread; header from popup_header_new. No errors. */
void popup_header_show_search_time(GtkWidget *header, bool shown);
/** Borrow the search time's text (empty when unknown) until the next set. Main thread;
 * header from popup_header_new. No errors. */
const char *popup_header_search_time(GtkWidget *header);
/** Start or stop the clock. Running shows the time at once and then ticks on each
 * whole second; stopping removes the timer, as disposing the header does. Main
 * thread; header from popup_header_new. No errors. */
void popup_header_set_running(GtkWidget *header, bool running);
/** Borrow the clock's text (empty until first started) until the next tick. Main
 * thread; header from popup_header_new. No errors. */
const char *popup_header_clock(GtkWidget *header);
#endif
