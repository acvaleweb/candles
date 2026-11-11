#ifndef MENU_H
#define MENU_H

#include <stddef.h>
#include "settings.h"

/* Callback that redraws the screen behind the menu (it must begin the frame
 * and draw the chart; the menu adds its own part and ends the frame). */
typedef void (*DrawFn)(void *ctx);

/* One-line prompt on the bottom row, prefilled with buf. Returns 1 if the
 * user pressed Enter on a non-empty line (the text is left in buf), 0 if
 * cancelled with Esc. Left/Right moves the cursor; typing inserts there and
 * Backspace removes the preceding character. Ctrl+A/E moves to start/end. */
int menu_prompt(const Settings *cfg, DrawFn draw, void *ctx, const char *label,
                char *buf, size_t n);

/* Shared bottom-line text editor: cursor is a byte offset in buf. */
int menu_edit_key(char *buf, size_t n, size_t *cursor, int key);
void menu_draw_prompt(const Settings *cfg,const char *label,const char *buf,size_t cursor);

/* Centered list. j/k or the arrow keys move, Enter selects, Esc/q cancels.
 * Returns the chosen index, or -1 if cancelled. */
int menu_select(const Settings *cfg, DrawFn draw, void *ctx, const char *title,
                char *const *items, int count, int selected);

/* menu_pick_stock returns this when the typed name (left in typed) was chosen
 * and it is not one of the items. */
#define PICK_NEW (-2)

/* The stock picker. The keyboard is reserved: letters, digits and . - _ ^ = are
 * typed (shown in capitals) and narrow the list to the items that start with
 * what is typed; up / down move, Enter selects, Esc cancels. When something is
 * typed the last entry is the typed name itself (unless it already is an item),
 * for a stock that has no file yet. current is the item to start on, or -1.
 * Returns the item index, PICK_NEW, or -1 if cancelled. */
int menu_pick_stock(const Settings *cfg, DrawFn draw, void *ctx, char *const *items,
                    int count, int current, char *typed, size_t typed_n);

/* Searchable, scrolling picker for an existing item. Unlike the stock picker,
 * Enter cannot create an item that is not already in the list. */
int menu_pick_existing(const Settings *cfg, DrawFn draw, void *ctx, const char *title,
                       char *const *items, int count, int current);

#endif
