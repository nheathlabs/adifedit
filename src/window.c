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
 * The main window: build the widget tree (menu bar, toolbar, data view,
 * status bar) and keep the shared UI state - title and
 * QSO count - in step with the AdifDocument. The actions those widgets
 * fire, the threaded load/save, and the unsaved-changes guard live in
 * window_actions.c.
 */

#include "window.h"

#include "window_priv.h"

#include "adif_io.h"

/* ------------------------------------------------------------------- */
/* GObject type                                                         */
/* ------------------------------------------------------------------- */

G_DEFINE_TYPE(AppWindow, app_window, GTK_TYPE_APPLICATION_WINDOW)

static void app_window_dispose(GObject *object) {
    AppWindow *w = APP_WINDOW(object);

    app_window_stamp_menu_dispose(w);
    g_clear_pointer(&w->table, adif_table_free);
    if (w->doc != NULL) {
        g_signal_handlers_disconnect_by_data(adif_document_get_rows(w->doc), w);
        g_signal_handlers_disconnect_by_data(w->doc, w);
    }
    g_clear_object(&w->doc);
    g_clear_pointer(&w->current_filepath, g_free);
    g_clear_pointer(&w->parse_warn_path, g_free);
    g_clear_pointer(&w->file_etag, g_free);
    g_clear_object(&w->search_cursor_row);

    G_OBJECT_CLASS(app_window_parent_class)->dispose(object);
}

static void app_window_class_init(AppWindowClass *klass) {
    G_OBJECT_CLASS(klass)->dispose = app_window_dispose;
}

static void app_window_init(G_GNUC_UNUSED AppWindow *self) {
}

/* ------------------------------------------------------------------- */
/* Shared UI state                                                      */
/* ------------------------------------------------------------------- */

/* The faded-icon watermark shows only when the window is at rest with no
 * file open - not while loading, and not once a file is in. */
static void refresh_empty_watermark(AppWindow *w) {
    gtk_widget_set_visible(w->empty_watermark, w->current_filepath == NULL && !w->loading);
}

void app_window_set_loading(AppWindow *w, gboolean loading, const char *label_text) {
    w->loading = loading;
    if (loading) {
        gtk_label_set_text(GTK_LABEL(w->status_loading_label), label_text);
        gtk_spinner_start(GTK_SPINNER(w->status_spinner));
    } else {
        gtk_spinner_stop(GTK_SPINNER(w->status_spinner));
    }
    gtk_widget_set_visible(w->status_loading_box, loading);
    gtk_widget_set_sensitive(adif_table_get_widget(w->table), !loading);
    app_window_refresh_actions(w);
}

gboolean app_window_is_parse_warn_file(AppWindow *w, const char *path) {
    return path != NULL && w->parse_warn_path != NULL
           && adif_io_same_file(path, w->parse_warn_path);
}

void app_window_update_title(AppWindow *w) {
    if (w->current_filepath == NULL) {
        gtk_window_set_title(GTK_WINDOW(w), "adifedit");
        return;
    }
    char *name = g_path_get_basename(w->current_filepath);
    char *title = g_strdup_printf(
        "%s%s%s — adifedit", adif_document_get_modified(w->doc) ? "• " : "", name,
        app_window_is_parse_warn_file(w, w->current_filepath) ? " (parse_warn)" : "");
    gtk_window_set_title(GTK_WINDOW(w), title);
    g_free(title);
    g_free(name);
}

/* --- status bar ---------------------------------------------------- */

static void status_set_count(AppWindow *w, guint n) {
    char *text = g_strdup_printf(n == 1 ? "%u QSO" : "%u QSOs", n);
    gtk_label_set_text(GTK_LABEL(w->status_count), text);
    g_free(text);
}

/* The document's rows model changed (load, insert row, delete row). */
static void on_rows_items_changed(GListModel *model, G_GNUC_UNUSED guint position,
                                  G_GNUC_UNUSED guint removed, G_GNUC_UNUSED guint added,
                                  gpointer user_data) {
    status_set_count(user_data, g_list_model_get_n_items(model));
}

static void on_doc_modified_changed(G_GNUC_UNUSED AdifDocument *doc, gpointer user_data) {
    app_window_update_title(user_data);
}

static void set_action_enabled(AppWindow *w, const char *name, gboolean enabled) {
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(w), name);
    if (action != NULL) {
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
    }
}

/* Sync one history action (and thus its toolbar button and menu item)
 * and its tooltip. */
static void set_history_action(AppWindow *w, const char *name, gboolean enabled, const char *verb,
                               const char *label, GtkWidget *button) {
    set_action_enabled(w, name, enabled);
    char *tip = (label != NULL) ? g_strdup_printf("%s %s", verb, label) : g_strdup(verb);
    gtk_widget_set_tooltip_text(button, tip);
    g_free(tip);
}

/* Undo/redo follow the document history - but not while a load or save
 * is in flight: an undo during a save would be marked saved when the
 * save lands, though it never reached the file. */
static void on_doc_history_changed(AdifDocument *doc, gpointer user_data) {
    AppWindow *w = user_data;
    set_history_action(w, "undo", !w->loading && adif_document_can_undo(doc), "Undo",
                       adif_document_undo_label(doc), w->undo_button);
    set_history_action(w, "redo", !w->loading && adif_document_can_redo(doc), "Redo",
                       adif_document_redo_label(doc), w->redo_button);
}

void app_window_refresh_actions(AppWindow *w) {
    /* A file is open and nothing is in flight. While a load or save runs,
     * everything that could start another one or change the document
     * waits for it - including Open itself, so two loads can't interleave
     * their rows into one document. */
    gboolean ready = w->current_filepath != NULL && !w->loading;

    for (gsize i = 0; i < WIN_ACTIONS_LEN; i++) {
        if (WIN_ACTIONS[i].needs_file) {
            set_action_enabled(w, WIN_ACTIONS[i].entry.name, ready);
        }
    }
    set_action_enabled(w, "open", !w->loading);
    on_doc_history_changed(w->doc, w);
    refresh_empty_watermark(w);
}

/* ------------------------------------------------------------------- */
/* Construction                                                         */
/* ------------------------------------------------------------------- */

static GMenuModel *create_menu_model(AppWindow *w) {
    GMenu *menu = g_menu_new();
    GMenu *file_menu = g_menu_new();

    g_menu_append(file_menu, "_Open…", "win.open");
    g_menu_append(file_menu, "_Save", "win.save");
    g_menu_append(file_menu, "Save _As…", "win.save_as");
    g_menu_append(file_menu, "_Close File", "win.close_file");

    GMenu *path_section = g_menu_new();
    g_menu_append(path_section, "_Copy File Path", "win.copy_file_path");
    g_menu_append_section(file_menu, NULL, G_MENU_MODEL(path_section));
    g_object_unref(path_section);

    GMenu *exit_section = g_menu_new();
    g_menu_append(exit_section, "E_xit", "win.exit");
    g_menu_append_section(file_menu, NULL, G_MENU_MODEL(exit_section));
    g_object_unref(exit_section);

    g_menu_append_submenu(menu, "_File", G_MENU_MODEL(file_menu));
    g_object_unref(file_menu);

    GMenu *edit_menu = g_menu_new();
    g_menu_append(edit_menu, "_Undo", "win.undo");
    g_menu_append(edit_menu, "_Redo", "win.redo");
    GMenu *find_section = g_menu_new();
    g_menu_append(find_section, "_Find…", "win.find");
    g_menu_append(find_section, "Find and _Replace…", "win.find_replace");
    g_menu_append(find_section, "Find _Next", "win.find_next");
    g_menu_append(find_section, "Find Pre_vious", "win.find_prev");
    g_menu_append_section(edit_menu, NULL, G_MENU_MODEL(find_section));
    g_object_unref(find_section);
    g_menu_append_submenu(menu, "_Edit", G_MENU_MODEL(edit_menu));
    g_object_unref(edit_menu);

    g_menu_append_submenu(menu, "_Batch Stamp", G_MENU_MODEL(w->stamp_menu));

    GMenu *help_menu = g_menu_new();
    char *spec_label = g_strdup_printf("ADIF _Specification (%s)", ADIF_SPEC_VERSION);
    g_menu_append(help_menu, spec_label, "win.adif_spec");
    g_free(spec_label);
    g_menu_append(help_menu, "_About adifedit", "win.about");
    g_menu_append_submenu(menu, "_Help", G_MENU_MODEL(help_menu));
    g_object_unref(help_menu);

    return G_MENU_MODEL(menu);
}

/* A "linked" box of buttons, appended to `toolbar`. */
static GtkWidget *toolbar_group(GtkWidget *toolbar) {
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(group, "linked");
    gtk_box_append(GTK_BOX(toolbar), group);
    return group;
}

/* An icon button bound to `action`, appended to `group`. */
static GtkWidget *toolbar_button(GtkWidget *group, const char *icon, const char *tip,
                                 const char *action) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    if (tip != NULL) {
        gtk_widget_set_tooltip_text(b, tip);
    }
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    gtk_box_append(GTK_BOX(group), b);
    return b;
}

/* Toolbar. Every button is bound to a "win." action, so each one's
 * sensitivity simply follows that action (app_window_refresh_actions). */
static GtkWidget *build_toolbar(AppWindow *w) {
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(toolbar, "toolbar");
    gtk_widget_set_margin_start(toolbar, 6);
    gtk_widget_set_margin_end(toolbar, 6);
    gtk_widget_set_margin_top(toolbar, 4);
    gtk_widget_set_margin_bottom(toolbar, 4);

    /* Linked groups of icon buttons: Open/Save, Undo/Redo, and the
     * insert-relative-to-the-cursor buttons. Undo/Redo are kept for their
     * tooltips, which name what they'd act on (on_doc_history_changed). */
    GtkWidget *file_group = toolbar_group(toolbar);
    toolbar_button(file_group, "adifedit-open-symbolic", "Open an ADIF file", "win.open");
    toolbar_button(file_group, "adifedit-save-symbolic", "Save changes to the ADIF file",
                   "win.save");

    gtk_box_append(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    GtkWidget *history = toolbar_group(toolbar);
    w->undo_button = toolbar_button(history, "adifedit-undo-symbolic", NULL, "win.undo");
    w->redo_button = toolbar_button(history, "adifedit-redo-symbolic", NULL, "win.redo");

    gtk_box_append(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    GtkWidget *insert = toolbar_group(toolbar);
    toolbar_button(insert, "adifedit-insert-row-above-symbolic", "Insert row above the cursor",
                   "win.cursor_insert_row_above");
    toolbar_button(insert, "adifedit-insert-row-below-symbolic", "Insert row below the cursor",
                   "win.cursor_insert_row_below");
    toolbar_button(insert, "adifedit-insert-column-left-symbolic",
                   "Insert column left of the cursor", "win.cursor_insert_column_left");
    toolbar_button(insert, "adifedit-insert-column-right-symbolic",
                   "Insert column right of the cursor", "win.cursor_insert_column_right");
    return toolbar;
}

/* The document, the spreadsheet-like view over it, and the empty-state
 * watermark on top. */
static GtkWidget *build_grid(AppWindow *w) {
    w->doc = adif_document_new();
    w->table = adif_table_new(w->doc);
    g_signal_connect(w->doc, "modified-changed", G_CALLBACK(on_doc_modified_changed), w);
    g_signal_connect(w->doc, "history-changed", G_CALLBACK(on_doc_history_changed), w);
    g_signal_connect(adif_document_get_rows(w->doc), "items-changed",
                     G_CALLBACK(on_rows_items_changed), w);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), adif_table_get_widget(w->table));
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);

    /* A big, faded app icon behind the empty grid until a file is loaded. */
    w->empty_watermark = gtk_image_new_from_icon_name("io.github.nheathlabs.adifedit");
    gtk_image_set_pixel_size(GTK_IMAGE(w->empty_watermark), 280);
    gtk_widget_set_halign(w->empty_watermark, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(w->empty_watermark, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->empty_watermark, "adif-empty-watermark"); /* fade + soft shadow */
    gtk_widget_set_can_target(w->empty_watermark, FALSE);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), scrolled);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), w->empty_watermark);
    return overlay;
}

/* Status bar: "Loading…" / "Saving…" spinner at the left, QSO count at
 * the right. */
static GtkWidget *build_statusbar(AppWindow *w) {
    GtkWidget *statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(statusbar, 8);
    gtk_widget_set_margin_end(statusbar, 8);
    gtk_widget_set_margin_top(statusbar, 3);
    gtk_widget_set_margin_bottom(statusbar, 3);

    w->status_spinner = gtk_spinner_new();
    gtk_widget_set_size_request(w->status_spinner, 14, 14);
    w->status_loading_label = gtk_label_new("");
    w->status_loading_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(w->status_loading_box, "dim-label");
    gtk_box_append(GTK_BOX(w->status_loading_box), w->status_spinner);
    gtk_box_append(GTK_BOX(w->status_loading_box), w->status_loading_label);
    gtk_widget_set_visible(w->status_loading_box, FALSE);
    gtk_box_append(GTK_BOX(statusbar), w->status_loading_box);

    w->status_count = gtk_label_new("");
    gtk_widget_add_css_class(w->status_count, "dim-label");
    gtk_widget_set_hexpand(w->status_count, TRUE);
    gtk_widget_set_halign(w->status_count, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(statusbar), w->status_count);
    return statusbar;
}

static AppWindow *create_app_window(GtkApplication *app) {
    AppWindow *w = g_object_new(APP_TYPE_WINDOW, "application", app, NULL);

    gtk_window_set_title(GTK_WINDOW(w), "adifedit");
    gtk_window_set_default_size(GTK_WINDOW(w), 1100, 650);

    app_window_add_actions(G_ACTION_MAP(w), w);
    g_signal_connect(w, "close-request", G_CALLBACK(on_window_close_request), w);

    app_window_stamp_menu_init(w); /* fills w->stamp_menu before create_menu_model uses it */

    /* Menu bar, always rendered in-window regardless of desktop shell. */
    GMenuModel *menu_model = create_menu_model(w);
    GtkWidget *menu_bar = gtk_popover_menu_bar_new_from_model(menu_model);
    g_object_unref(menu_model);

    GtkWidget *toolbar = build_toolbar(w);
    GtkWidget *grid = build_grid(w);
    GtkWidget *statusbar = build_statusbar(w);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), menu_bar);
    gtk_box_append(GTK_BOX(vbox), toolbar);
    gtk_box_append(GTK_BOX(vbox), app_window_search_bar_new(w));
    gtk_box_append(GTK_BOX(vbox), grid);
    gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(vbox), statusbar);
    gtk_window_set_child(GTK_WINDOW(w), vbox);

    /* Nothing is actionable until a file is loaded; undo/redo start empty. */
    app_window_refresh_actions(w);

    return w;
}

void adifedit_app_startup(GtkApplication *app) {
    /* The icons are embedded as a GResource under this path. GtkApplication
     * adds "<resource base path>/icons" to the icon theme by itself, but
     * that base path follows the application id - and the screenshot
     * helper runs under a different id - so add it explicitly. */
    gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(gdk_display_get_default()),
                                     "/io/github/nheathlabs/adifedit/icons");

    /* Used for the window icon, the task-switcher entry and the About
     * dialog logo. */
    gtk_window_set_default_icon_name("io.github.nheathlabs.adifedit");

    /* The app stylesheet (assets/style.css): the search-match highlight
     * and the empty-state watermark. */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/io/github/nheathlabs/adifedit/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    app_window_install_accels(app);
}

void adifedit_window_present(GtkApplication *app) {
    AppWindow *w = create_app_window(app);
    gtk_window_present(GTK_WINDOW(w));
}

void adifedit_window_open(GtkApplication *app, GFile *file) {
    AppWindow *w = create_app_window(app);
    gtk_window_present(GTK_WINDOW(w));
    app_window_open_file(w, file);
}
