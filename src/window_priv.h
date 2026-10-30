/*
 * adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
 * Copyright (C) 2026 Nicholas Heath
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Internal glue shared between the main-window implementation files:
 *   window.c          - window construction, menu bar, status bar, title
 *   window_actions.c  - GAction / toolbar callbacks, load/save, close guard
 *   window_stamp.c    - the Batch Stamp menu and its input dialog
 *   window_search.c   - the Find / Find & Replace bar
 *
 * Not a public header - only those .c files include it.
 */
#pragma once

#include <gtk/gtk.h>

#include "adif_document.h"
#include "adif_table.h"
#include "window.h"

G_BEGIN_DECLS

/* A follow-up the unsaved-changes guard deferred: run when the user picks
 * "Discard", or after the save they asked for completes. */
typedef enum {
    APP_WINDOW_PENDING_NONE,
    APP_WINDOW_PENDING_OPEN,        /* re-show the Open file chooser */
    APP_WINDOW_PENDING_CLOSE_FILE,  /* empty the window */
    APP_WINDOW_PENDING_COPY_PATH,   /* put the file path on the clipboard */
    APP_WINDOW_PENDING_CLOSE,       /* close the window */
} AppWindowPending;

/*
 * AppWindow is a GtkApplicationWindow subclass, not a plain struct hung
 * off one via object data - `w` IS the top-level window (cast it with
 * GTK_WINDOW(w) / GTK_WIDGET(w) where a plain GTK call wants one), and
 * its lifetime is the normal GObject one (dispose in window.c releases
 * doc/table/stamp state; GTK drops the last ref when the window closes).
 */
#define APP_TYPE_WINDOW (app_window_get_type())
G_DECLARE_FINAL_TYPE(AppWindow, app_window, APP, WINDOW, GtkApplicationWindow)

struct _AppWindow {
    GtkApplicationWindow parent_instance;

    AdifDocument *doc;       /* the open document; owns the data */
    AdifTable *table;        /* the view over `doc` */
    GtkWidget *undo_button;  /* toolbar; tooltip names the next undo step */
    GtkWidget *redo_button;
    gboolean loading;           /* a load, save or Fill Column is in flight */
    GtkWidget *status_loading_box;   /* bottom-left: spinner + "Loading…" / "Saving…" / … */
    GtkWidget *status_spinner;
    GtkWidget *status_loading_label;
    GtkWidget *empty_watermark; /* faded app icon over the grid while no file is open */
    GtkWidget *status_count;    /* bottom-right: "N QSOs" */
    char *current_filepath;  /* the file Open loaded; default target for Save */
    char *parse_warn_path;   /* the file the last load flagged with a parse warning;
                                adifedit never writes to it (Save becomes Save As).
                                NULL normally. */
    char *file_etag;         /* current_filepath's etag as adifedit last read or wrote
                                it; a save there is refused if the file has changed
                                since (the changed-on-disk prompt). NULL: no check. */

    GMenu *stamp_menu;              /* "Batch Stamp" submenu contents (rebuilt live) */
    GFileMonitor *stamp_monitor;    /* watches the user stamps dir */

    /* Find / Replace bar (window_search.c owns the wiring). */
    GtkWidget *search_bar;      /* GtkSearchBar, below the toolbar */
    GtkWidget *search_entry;    /* GtkSearchEntry */
    GtkWidget *replace_revealer; /* holds the replace row; shown only in replace mode */
    GtkWidget *replace_entry;   /* GtkEntry */
    GtkWidget *search_info;     /* "3 / 17" (or "No matches") */
    GtkWidget *search_scope;    /* GtkDropDown: All columns / <column> */
    GtkWidget *search_case;     /* GtkCheckButton "Match case" */
    GtkWidget *search_whole;    /* GtkCheckButton "Whole field" */
    AdifRow *search_cursor_row; /* (ref) current match's row, or NULL for "none yet" -
                                   the row itself, so inserts/deletes above it
                                   don't move the cursor onto another row */
    guint search_cursor_di;

    /* The unsaved-changes guard (Open, Close File, Copy File Path, window
     * close all share it): what to do once the deferred save lands, or
     * straight away on "Discard". */
    gboolean force_close;    /* the close guard is satisfied; let the next close through */
    AppWindowPending pending;
};

/* --- window.c: shared UI state ------------------------------------- */

/* Show/hide the status-bar "Loading…" / "Saving…" spinner, (de)sensitise
 * the data view, and refresh the actions (see refresh_actions). `label_text`
 * is shown next to the spinner; ignored (may be NULL) when `loading` is
 * FALSE. */
void app_window_set_loading(AppWindow *w, gboolean loading, const char *label_text);

/* Re-derive which actions are enabled from the window's state: the
 * needs-a-file ones only while a file is open and no load/save is in
 * flight; Open and undo/redo not while one is. Call after changing
 * w->current_filepath or w->loading. */
void app_window_refresh_actions(AppWindow *w);

/* Refresh the window title from w->current_filepath, the document's
 * modified flag, and whether it is the parse_warn file. */
void app_window_update_title(AppWindow *w);

/* TRUE if `path` (may be NULL) is the file the last load flagged with a
 * parse warning (w->parse_warn_path) - one adifedit must not write to. */
gboolean app_window_is_parse_warn_file(AppWindow *w, const char *path);

/* --- window_actions.c: actions, toolbar, load/save, close guard --- */

/*
 * The window's actions, in one table (window_actions.c): the GActionEntry
 * itself, its keyboard accelerator(s), and whether it should be disabled
 * until a document is open. window.c feeds `.entry` to
 * g_action_map_add_action_entries() and reads `.needs_file` in
 * app_window_refresh_actions(); app_window_install_accels() reads
 * `.accel` / `.accel2`.
 */
typedef struct {
    GActionEntry entry;
    const char *accel;   /* "<Control>s", or NULL for none */
    const char *accel2;  /* a second binding, or NULL */
    gboolean needs_file; /* greyed out until a file is open (refresh_actions) */
} WinAction;

extern const WinAction WIN_ACTIONS[];
extern const gsize WIN_ACTIONS_LEN;

/* Register the WIN_ACTIONS entries on `map` (called from window.c). */
void app_window_add_actions(GActionMap *map, gpointer user_data);

/* Bind the WIN_ACTIONS accelerators on `app` (from adifedit_app_startup). */
void app_window_install_accels(GtkApplication *app);

/* Kick off a threaded load of `path` into the window. */
void app_window_start_loading(AppWindow *w, const char *path);

/* Load `file` into the window (app_window_start_loading), or explain why
 * not if it has no local path. */
void app_window_open_file(AppWindow *w, GFile *file);

/* GtkWindow::close-request handler: prompts if the document is modified. */
gboolean on_window_close_request(GtkWindow *window, gpointer user_data);

/* --- window_stamp.c: the Batch Stamp submenu + input dialog ------- */

/* Create w->stamp_menu, fill it, and start watching the stamps dir.
 * Call before create_menu_model(). */
void app_window_stamp_menu_init(AppWindow *w);
void app_window_stamp_menu_dispose(AppWindow *w); /* drop the file monitor */

/* GActionEntry handlers (referenced from WIN_ACTIONS in window_actions.c). */
void win_action_run_stamp(GSimpleAction *action, GVariant *parameter, gpointer user_data);
void win_action_stamps_open_fm(GSimpleAction *action, GVariant *parameter, gpointer user_data);
void win_action_stamps_copy_path(GSimpleAction *action, GVariant *parameter, gpointer user_data);

/* --- window_search.c: the Find / Find & Replace bar -------------- */

/* Build w->search_bar (and the widgets it holds); returns it for the
 * caller to pack into the window, just below the toolbar. */
GtkWidget *app_window_search_bar_new(AppWindow *w);

/* GActionEntry handlers for the bar. */
void win_action_find(GSimpleAction *action, GVariant *parameter, gpointer user_data);
void win_action_find_replace(GSimpleAction *action, GVariant *parameter, gpointer user_data);
void win_action_find_next(GSimpleAction *action, GVariant *parameter, gpointer user_data);
void win_action_find_prev(GSimpleAction *action, GVariant *parameter, gpointer user_data);
void win_action_replace(GSimpleAction *action, GVariant *parameter, gpointer user_data);
void win_action_replace_all(GSimpleAction *action, GVariant *parameter, gpointer user_data);

G_END_DECLS
