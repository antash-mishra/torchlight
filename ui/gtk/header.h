/* Private GTK widget: Tekst's teletext page header (page number, name, count and clock). */
#ifndef TORCHLIGHT_GTK_HEADER_H
#define TORCHLIGHT_GTK_HEADER_H
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stddef.h>
/** Create a floating header: "P100", "Torchlight", the result count and a clock in
 * teletext's slash-seconds format. Main thread; the caller parents it. It is hidden
 * from assistive technology, which hears the count through announcements. */
GtkWidget *popup_header_new(void);
/** Show count results ("1 result", "12 results"), or nothing for zero. Main thread;
 * header from popup_header_new. No errors or ownership transfer. */
void popup_header_set_count(GtkWidget *header, size_t count);
/** Start or stop the clock. Running shows the time at once and then ticks on each
 * whole second; stopping removes the timer, as disposing the header does. Main
 * thread; header from popup_header_new. No errors. */
void popup_header_set_running(GtkWidget *header, bool running);
/** Borrow the clock's text (empty until first started) until the next tick. Main
 * thread; header from popup_header_new. No errors. */
const char *popup_header_clock(GtkWidget *header);
#endif
