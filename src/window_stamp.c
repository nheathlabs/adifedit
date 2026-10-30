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
 * The Batch Stamp menu (one item per .bst file, rebuilt when the stamps
 * directory changes) and the dialog that collects a value per field and
 * writes them into every non-blank record as one undo step.
 */

#include "window_priv.h"

#include "batch_stamp.h"
#include "time_slice.h"
#include "ui_dialogs.h"

/* ------------------------------------------------------------------- */
/* Menu                                                                 */
/* ------------------------------------------------------------------- */

/* `title` as a menu label: menus read '_' as a mnemonic marker, so a
 * stamp named "MY_RIG" would show as "MYRIG" - double each one. Caller
 * frees. */
static char *menu_label_for(const char *title) {
    char **parts = g_strsplit(title, "_", -1);
    char *label = g_strjoinv("__", parts);
    g_strfreev(parts);
    return label;
}

static void stamp_menu_fill(GMenu *menu, GPtrArray *stamps) {
    GMenu *section = g_menu_new();
    if (stamps->len == 0) {
        GMenuItem *item = g_menu_item_new("No stamps found", NULL); /* no action -> greyed */
        g_menu_append_item(section, item);
        g_object_unref(item);
    }
    for (guint i = 0; i < stamps->len; i++) {
        AdifStamp *s = g_ptr_array_index(stamps, i);
        char *label = menu_label_for(s->title);
        GMenuItem *item = g_menu_item_new(label, NULL);
        g_free(label);
        g_menu_item_set_action_and_target_value(item, "win.run_stamp",
                                                g_variant_new_string(s->path));
        g_menu_append_item(section, item);
        g_object_unref(item);
    }
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);

    GMenu *dir = g_menu_new();
    g_menu_append(dir, "In File Manager", "win.stamps_open_fm");
    g_menu_append(dir, "Copy Path", "win.stamps_copy_path");
    g_menu_append_submenu(menu, "Open Stamps Directory", G_MENU_MODEL(dir));
    g_object_unref(dir);
}

static void refresh_stamp_menu(AppWindow *w) {
    g_menu_remove_all(w->stamp_menu);
    GPtrArray *stamps = adif_stamp_list();
    stamp_menu_fill(w->stamp_menu, stamps);
    adif_stamp_list_free(stamps);
}

static void on_stamp_dir_changed(G_GNUC_UNUSED GFileMonitor *monitor, G_GNUC_UNUSED GFile *file,
                                 G_GNUC_UNUSED GFile *other, G_GNUC_UNUSED GFileMonitorEvent event,
                                 gpointer user_data) {
    refresh_stamp_menu(user_data);
}

void app_window_stamp_menu_init(AppWindow *w) {
    w->stamp_menu = g_menu_new();
    refresh_stamp_menu(w);

    char *dir = adif_stamp_user_dir(); /* also creates it */
    GFile *gdir = g_file_new_for_path(dir);
    g_free(dir);
    w->stamp_monitor = g_file_monitor_directory(gdir, G_FILE_MONITOR_NONE, NULL, NULL);
    g_object_unref(gdir);
    if (w->stamp_monitor != NULL) {
        g_signal_connect(w->stamp_monitor, "changed", G_CALLBACK(on_stamp_dir_changed), w);
    }
}

void app_window_stamp_menu_dispose(AppWindow *w) {
    if (w->stamp_monitor != NULL) {
        g_signal_handlers_disconnect_by_data(w->stamp_monitor, w);
        g_clear_object(&w->stamp_monitor);
    }
    g_clear_object(&w->stamp_menu);
}

/* ------------------------------------------------------------------- */
/* "Open Stamps Directory" actions                                      */
/* ------------------------------------------------------------------- */

/* Deferred to an idle so the menu popover this ran from has closed and
 * dropped its grab first: launching can bring up a portal/app-chooser
 * popup, and GTK warns ("grabbing popup with a non-top most parent") if
 * the menu is still mapped over it. */
static gboolean open_stamps_dir_idle(gpointer user_data) {
    AppWindow *w = user_data;
    char *dir = adif_stamp_user_dir();
    GFile *gdir = g_file_new_for_path(dir);
    g_free(dir);
    GtkFileLauncher *launcher = gtk_file_launcher_new(gdir);
    gtk_file_launcher_launch(launcher, GTK_WINDOW(w), NULL, NULL, NULL);
    g_object_unref(launcher);
    g_object_unref(gdir);
    return G_SOURCE_REMOVE;
}

void win_action_stamps_open_fm(G_GNUC_UNUSED GSimpleAction *action,
                               G_GNUC_UNUSED GVariant *parameter, gpointer user_data) {
    g_idle_add(open_stamps_dir_idle, user_data);
}

void win_action_stamps_copy_path(G_GNUC_UNUSED GSimpleAction *action,
                                 G_GNUC_UNUSED GVariant *parameter, gpointer user_data) {
    AppWindow *w = user_data;
    char *dir = adif_stamp_user_dir();
    gdk_clipboard_set(gtk_widget_get_clipboard(GTK_WIDGET(w)), G_TYPE_STRING, dir);
    g_free(dir);
}

/* ------------------------------------------------------------------- */
/* The input dialog                                                     */
/* ------------------------------------------------------------------- */

/*
 * The form lives as long as its dialog (freed when the dialog finalises).
 * Applying runs as a chain of idles and paint waits, during which the
 * main window could close; so the form holds a ref on the main window
 * (the memory stays valid) and the chain holds one on the dialog (the
 * form stays valid), and each step checks stamp_abandoned() first.
 */
typedef struct {
    AppWindow *appwin;      /* (ref) */
    AdifStamp *stamp;
    GtkWidget *window;
    GtkWidget *footer;      /* GtkStack: "buttons" until Apply, then "busy" */
    GtkWidget *buttons;     /* Cancel / Apply */
    GPtrArray *entries;     /* GtkWidget*, parallel to stamp->fields */
    gboolean busy;          /* applying: the window can't be closed */
    gboolean destroyed;     /* the dialog has been destroyed (e.g. with its parent) */
    guint field_index;      /* the next field to (or currently being) applied */
    AdifColumnWrite *step; /* the field currently being written in slices, or NULL between fields */
} StampForm;

static void stamp_form_free(gpointer data) {
    StampForm *form = data;
    if (form->step != NULL) { /* shouldn't happen - defence in depth, see stamp_apply_step() */
        adif_document_column_write_cancel(form->step);
    }
    adif_stamp_free(form->stamp);
    g_ptr_array_unref(form->entries);
    g_object_unref(form->appwin);
    g_free(form);
}

static void on_stamp_destroy(G_GNUC_UNUSED GtkWidget *window, gpointer data) {
    StampForm *form = data;
    form->destroyed = TRUE;
}

/* The dialog or the main window has gone since the apply was queued.
 * app_window_dispose() clears `doc`, so a NULL doc means the main window
 * is closing and there is nothing left to stamp. */
static gboolean stamp_abandoned(StampForm *form) {
    return form->destroyed || form->appwin->doc == NULL;
}

/*
 * Run `func` once `widget`'s window has put its next frame on screen.
 *
 * Showing a widget only *queues* a repaint, and the frame clock waits for
 * the compositor before painting, so a plain idle queued behind it runs
 * first - main-thread work started that way blocks the very frame that
 * would have shown the "busy" state. Waiting for "after-paint" (the frame
 * is committed to the compositor by then) guarantees it is visible. The
 * timeout covers a window that never paints (e.g. minimised).
 */
typedef struct {
    GdkFrameClock *clock; /* ref */
    gulong handler;
    guint timeout;
    GSourceFunc func;
    gpointer data;
} PaintWait;

static void paint_wait_finish(PaintWait *wait) {
    g_signal_handler_disconnect(wait->clock, wait->handler);
    if (wait->timeout != 0) {
        g_source_remove(wait->timeout);
    }
    g_object_unref(wait->clock);
    g_idle_add(wait->func, wait->data); /* out of the frame clock's dispatch */
    g_free(wait);
}

static void on_wait_painted(G_GNUC_UNUSED GdkFrameClock *clock, gpointer data) {
    paint_wait_finish(data);
}

static gboolean on_wait_timeout(gpointer data) {
    PaintWait *wait = data;
    wait->timeout = 0; /* removed by returning G_SOURCE_REMOVE */
    paint_wait_finish(wait);
    return G_SOURCE_REMOVE;
}

static void run_after_next_paint(GtkWidget *widget, GSourceFunc func, gpointer data) {
    GdkFrameClock *clock = gtk_widget_get_frame_clock(widget);
    if (clock == NULL) { /* not realised - nothing to wait for */
        g_idle_add(func, data);
        return;
    }
    PaintWait *wait = g_new0(PaintWait, 1);
    wait->clock = g_object_ref(clock);
    wait->func = func;
    wait->data = data;
    wait->handler = g_signal_connect(clock, "after-paint", G_CALLBACK(on_wait_painted), wait);
    wait->timeout = g_timeout_add(500, on_wait_timeout, wait);
    gtk_widget_queue_draw(widget); /* make sure a frame is coming */
}

/* End of the apply chain, finished or abandoned: close the dialog (unless
 * it already went with its parent) and drop the chain's ref on it - which
 * may free the form, so it is not touched after. */
static void stamp_finish(StampForm *form) {
    GtkWidget *window = form->window;
    form->busy = FALSE;
    if (!form->destroyed) {
        gtk_window_destroy(GTK_WINDOW(window));
    }
    g_object_unref(window); /* taken in stamp_start_apply */
}

/* Last step: the main window has painted the stamped columns. */
static gboolean stamp_close_idle(gpointer data) {
    stamp_finish(data);
    return G_SOURCE_REMOVE;
}

/* After the apply: the view's column auto-fit is queued at default-idle
 * priority, so this runs once it has, and then waits for the main window
 * to paint the result before closing - the dialog stays up for the whole
 * wait, rather than vanishing into a window that is still busy. */
static gboolean stamp_settle_idle(gpointer data) {
    StampForm *form = data;
    if (stamp_abandoned(form)) {
        stamp_finish(form);
        return G_SOURCE_REMOVE;
    }
    run_after_next_paint(GTK_WIDGET(form->appwin), stamp_close_idle, form);
    return G_SOURCE_REMOVE;
}

/* What the user typed for stamp field `i`, trimmed, or NULL if that's
 * blank (caller frees). */
static char *stamp_entry_value(StampForm *form, guint i) {
    const char *raw = gtk_editable_get_text(GTK_EDITABLE(g_ptr_array_index(form->entries, i)));
    char *value = g_strstrip(g_strdup(raw));
    if (value[0] == '\0') {
        g_free(value);
        return NULL;
    }
    return value;
}

/* One turn of applying the stamp: writes up to ADIF_SLICE_BUDGET_US
 * worth of the current field's rows, or starts the next non-blank field,
 * then yields (G_SOURCE_CONTINUE) so the main loop can paint - a
 * repeating idle rather than a big loop, and a plain idle rather than a
 * frame-clock tick, so it keeps making progress (and stamp_abandoned()
 * below still gets to run stamp_finish()) even if the dialog is
 * destroyed mid-chain, when a tick tied to that widget would simply stop
 * being called.
 *
 * A group stays open across every turn here (adif_document_undo()/
 * redo() assert it isn't). That's only safe because the dialog is modal:
 * nothing can reach Undo/Redo on the main window while it's up, so this
 * open group is never visible to them. */
static gboolean stamp_apply_step(gpointer data) {
    StampForm *form = data;
    if (stamp_abandoned(form)) {
        if (form->step != NULL) {
            adif_document_column_write_cancel(form->step);
            form->step = NULL;
        }
        stamp_finish(form);
        return G_SOURCE_REMOVE;
    }
    AdifDocument *doc = form->appwin->doc;

    if (form->step != NULL) {
        if (adif_document_column_write_step(form->step, adif_slice_deadline())) {
            return G_SOURCE_CONTINUE; /* rows remain in this field */
        }
        adif_document_column_write_finish(form->step);
        form->step = NULL;
        return G_SOURCE_CONTINUE; /* let a frame paint before starting the next field */
    }

    while (form->field_index < form->stamp->fields->len) {
        guint i = form->field_index++;
        const char *field = g_ptr_array_index(form->stamp->fields, i);
        char *value = stamp_entry_value(form, i);
        if (value == NULL) {
            continue;
        }
        guint di;
        if (!adif_document_find_column(doc, field, &di)) {
            di = adif_document_insert_column(doc, adif_document_get_n_columns(doc), field);
        }
        form->step = adif_document_column_write_begin(doc, di, value, TRUE);
        g_free(value);
        return G_SOURCE_CONTINUE;
    }

    adif_document_end_group(doc);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE + 10, stamp_settle_idle, form, NULL);
    return G_SOURCE_REMOVE;
}

/* Starts the apply chain: one undo group for every field, applied by
 * stamp_apply_step() one time-sliced turn at a time. The document holds
 * "schema-changed" until end_group, so the view rebuilds once. */
static gboolean stamp_apply_idle(gpointer data) {
    StampForm *form = data;
    if (stamp_abandoned(form)) {
        stamp_finish(form);
        return G_SOURCE_REMOVE;
    }
    char *label = g_strdup_printf("Batch Stamp: %s", form->stamp->title);
    adif_document_begin_group(form->appwin->doc, label);
    g_free(label);
    g_idle_add(stamp_apply_step, form);
    return G_SOURCE_REMOVE;
}

static void stamp_start_apply(StampForm *form) {
    g_object_ref(form->window); /* the apply chain's; released by stamp_finish */
    form->busy = TRUE;
    gtk_widget_set_sensitive(form->buttons, FALSE); /* no Enter-to-Apply twice */
    for (guint i = 0; i < form->entries->len; i++) {
        gtk_widget_set_sensitive(g_ptr_array_index(form->entries, i), FALSE);
    }
    gtk_stack_set_visible_child_name(GTK_STACK(form->footer), "busy");
    run_after_next_paint(form->window, stamp_apply_idle, form);
}

/* The title-bar close button, while applying. */
static gboolean on_stamp_close_request(G_GNUC_UNUSED GtkWindow *window, gpointer data) {
    StampForm *form = data;
    return form->busy; /* TRUE blocks the close */
}

static void on_overwrite_choice(GObject *source, GAsyncResult *res, gpointer data) {
    StampForm *form = data;
    GtkWidget *window = form->window; /* ref taken before choose() */
    GError *error = NULL;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, &error);
    if (error != NULL) {
        g_clear_error(&error); /* dismissed -> treat as Cancel */
    } else if (choice == 1 && !stamp_abandoned(form)) { /* Overwrite */
        stamp_start_apply(form);
    }
    g_object_unref(window); /* may free the form */
}

/* Rows where column `field` already has a value (0 if there's no such
 * column yet). */
static guint count_filled(AdifDocument *doc, const char *field) {
    guint di;
    if (!adif_document_find_column(doc, field, &di)) {
        return 0;
    }
    guint n = 0;
    for (guint r = 0; r < adif_document_get_n_rows(doc); r++) {
        if (adif_document_get_value(doc, r, di)[0] != '\0') {
            n++;
        }
    }
    return n;
}

/* The existing values this stamp would overwrite, one "\n  FIELD - N rows"
 * line per field (empty if none). `*any` says whether any field has a
 * value to write at all. */
static GString *describe_overwrites(StampForm *form, gboolean *any) {
    GString *clash = g_string_new(NULL);
    *any = FALSE;
    for (guint i = 0; i < form->stamp->fields->len; i++) {
        char *value = stamp_entry_value(form, i);
        if (value == NULL) {
            continue;
        }
        g_free(value);
        *any = TRUE;
        const char *field = g_ptr_array_index(form->stamp->fields, i);
        guint n = count_filled(form->appwin->doc, field);
        if (n > 0) {
            g_string_append_printf(clash, "\n  %s - %u row%s", field, n, n == 1 ? "" : "s");
        }
    }
    return clash;
}

static void on_stamp_apply(G_GNUC_UNUSED GtkButton *button, gpointer data) {
    StampForm *form = data;
    gboolean any;
    GString *clash = describe_overwrites(form, &any);

    if (!any) {
        gtk_window_destroy(GTK_WINDOW(form->window));
        g_string_free(clash, TRUE);
        return;
    }

    if (clash->len > 0) {
        char *msg = g_strdup_printf("This stamp will overwrite existing values:%s", clash->str);
        GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", msg);
        g_free(msg);
        gtk_alert_dialog_set_buttons(dialog, (const char *[]){ "_Cancel", "_Overwrite", NULL });
        gtk_alert_dialog_set_cancel_button(dialog, 0);
        gtk_alert_dialog_set_default_button(dialog, 1);
        g_object_ref(form->window); /* keeps the form alive until the answer */
        gtk_alert_dialog_choose(dialog, GTK_WINDOW(form->window), NULL, on_overwrite_choice, form);
        g_object_unref(dialog);
    } else {
        stamp_start_apply(form);
    }
    g_string_free(clash, TRUE);
}

static void open_stamp_form(AppWindow *w, AdifStamp *stamp /* transfer */) {
    StampForm *form = g_new0(StampForm, 1);
    form->appwin = g_object_ref(w);
    form->stamp = stamp;
    form->entries = g_ptr_array_new();

    GtkWidget *box = NULL;
    form->window = ui_modal_dialog(GTK_WINDOW(w), stamp->title, &box, form, stamp_form_free);
    gtk_window_set_default_size(GTK_WINDOW(form->window), 460, -1);

    GtkWidget *intro = gtk_label_new("A value for each field. Leave one blank to skip it.");
    gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
    gtk_widget_add_css_class(intro, "dim-label");
    gtk_box_append(GTK_BOX(box), intro);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    for (guint i = 0; i < stamp->fields->len; i++) {
        GtkWidget *label = gtk_label_new(g_ptr_array_index(stamp->fields, i));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        GtkWidget *entry = gtk_entry_new();
        gtk_editable_set_width_chars(GTK_EDITABLE(entry), 48);
        gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
        gtk_grid_attach(GTK_GRID(grid), label, 0, (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, (int)i, 1, 1);
        g_ptr_array_add(form->entries, entry);
    }
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), 420);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), grid);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);

    GtkWidget *apply = NULL;
    form->buttons = ui_dialog_button_row(form->window, "_Apply", &apply);
    g_signal_connect(apply, "clicked", G_CALLBACK(on_stamp_apply), form);

    /* The busy row replaces the buttons in place; the stack is sized to
     * the larger of the two, so the dialog doesn't jump when they swap. */
    GtkWidget *busy = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(busy, GTK_ALIGN_END);
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_box_append(GTK_BOX(busy), spinner);
    gtk_box_append(GTK_BOX(busy), gtk_label_new("Applying\xe2\x80\xa6"));

    form->footer = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(form->footer), form->buttons, "buttons");
    gtk_stack_add_named(GTK_STACK(form->footer), busy, "busy");
    gtk_stack_set_visible_child_name(GTK_STACK(form->footer), "buttons");
    gtk_box_append(GTK_BOX(box), form->footer);
    g_signal_connect(form->window, "close-request", G_CALLBACK(on_stamp_close_request), form);
    g_signal_connect(form->window, "destroy", G_CALLBACK(on_stamp_destroy), form);
    /* Close along with the main window rather than outliving it. */
    gtk_window_set_destroy_with_parent(GTK_WINDOW(form->window), TRUE);

    gtk_window_present(GTK_WINDOW(form->window));
    if (form->entries->len > 0) {
        gtk_widget_grab_focus(g_ptr_array_index(form->entries, 0));
    }
}

void win_action_run_stamp(G_GNUC_UNUSED GSimpleAction *action, GVariant *parameter,
                          gpointer user_data) {
    AppWindow *w = user_data;
    const char *path = g_variant_get_string(parameter, NULL);

    GError *error = NULL;
    AdifStamp *stamp = adif_stamp_load(path, &error);
    if (stamp == NULL) {
        ui_show_error(GTK_WINDOW(w),
                      error != NULL ? error->message : "The stamp file could not be read.");
        g_clear_error(&error);
        return;
    }

    if (stamp->bad->len > 0) {
        GString *msg = g_string_new(NULL);
        g_string_append_printf(msg, "\"%s\" can't be used until these lines are fixed:\n",
                               stamp->title);
        for (guint i = 0; i < stamp->bad->len; i++) {
            g_string_append_printf(msg, "\n  %s", (const char *)g_ptr_array_index(stamp->bad, i));
        }
        ui_show_error(GTK_WINDOW(w), msg->str);
        g_string_free(msg, TRUE);
        adif_stamp_free(stamp);
        return;
    }

    open_stamp_form(w, stamp); /* takes ownership of stamp */
}
