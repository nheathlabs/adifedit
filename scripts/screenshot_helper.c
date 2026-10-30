/*
 * adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
 * Copyright (C) 2026 Nicholas Heath
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Screenshot driver for scripts/screenshots.sh - NOT part of the app.
 *
 * It opens the real main window on a sample log, then after a delay puts
 * one piece of UI on screen so grim (run by the shell wrapper) can
 * capture it:
 *
 *   main      just the loaded window
 *   filemenu  the File menu, dropped open
 *   colmenu   the column-header context menu, popped open
 *   stamp     the Batch Stamp input dialog for assets/stamps/example.bst
 *   replace   the Find & Replace bar, mid-search
 *
 *   screenshot_helper MODE SAMPLE.adi STAMP.bst TRIGGER_MS
 *
 * Built as the CMake target `screenshot_helper` (EXCLUDE_FROM_ALL): it
 * links the `adifeditui` object library and adds this main(). It uses only
 * window.h plus a walk of the public widget tree, so it needs no private
 * headers. The colmenu model mirrors create_header_menu() in
 * src/adif_table_view.c - keep the two in step (a stale screenshot is
 * the only failure mode, and regenerating fixes it).
 */

#include <gtk/gtk.h>

#include "window.h"

static const char *g_mode;
static char *g_stamp_path;
static guint g_trigger_ms;

static GtkWidget *find_descendant(GtkWidget *widget, GType type) {
    if (widget == NULL) {
        return NULL;
    }
    if (g_type_is_a(G_OBJECT_TYPE(widget), type)) {
        return widget;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *hit = find_descendant(child, type);
        if (hit != NULL) {
            return hit;
        }
    }
    return NULL;
}

/* Mirrors create_header_menu() in src/adif_table_view.c. */
static GMenuModel *build_header_menu(void) {
    GMenu *menu = g_menu_new();

    GMenu *buttons = g_menu_new();
    GMenuItem *left = g_menu_item_new("Insert column to the left", NULL);
    g_menu_item_set_attribute(left, "verb-icon", "s", "adifedit-insert-column-left-symbolic");
    g_menu_item_set_action_and_target_value(left, "win.insert_column_left",
                                            g_variant_new_uint32(0));
    g_menu_append_item(buttons, left);
    g_object_unref(left);
    GMenuItem *right = g_menu_item_new("Insert column to the right", NULL);
    g_menu_item_set_attribute(right, "verb-icon", "s", "adifedit-insert-column-right-symbolic");
    g_menu_item_set_action_and_target_value(right, "win.insert_column_right",
                                            g_variant_new_uint32(0));
    g_menu_append_item(buttons, right);
    g_object_unref(right);

    GMenuItem *section = g_menu_item_new_section(NULL, G_MENU_MODEL(buttons));
    g_menu_item_set_attribute(section, "display-hint", "s", "horizontal-buttons");
    g_menu_append_item(menu, section);
    g_object_unref(section);
    g_object_unref(buttons);

    GMenu *actions = g_menu_new();
    GMenuItem *fill = g_menu_item_new("Fill Column\xE2\x80\xA6", NULL);
    g_menu_item_set_action_and_target_value(fill, "win.fill_column", g_variant_new_uint32(0));
    g_menu_append_item(actions, fill);
    g_object_unref(fill);
    GMenuItem *del = g_menu_item_new("Delete Column", NULL);
    g_menu_item_set_action_and_target_value(del, "win.delete_column", g_variant_new_uint32(0));
    g_menu_append_item(actions, del);
    g_object_unref(del);
    g_menu_append_section(menu, NULL, G_MENU_MODEL(actions));
    g_object_unref(actions);

    return G_MENU_MODEL(menu);
}

static void show_column_menu(GtkWindow *window) {
    GtkWidget *column_view = find_descendant(GTK_WIDGET(window), GTK_TYPE_COLUMN_VIEW);
    if (column_view == NULL) {
        g_warning("no GtkColumnView in the window");
        return;
    }

    GMenuModel *model = build_header_menu();
    GtkWidget *popover = gtk_popover_menu_new_from_model(model);
    g_object_unref(model);
    gtk_widget_set_parent(popover, column_view);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_widget_set_halign(popover, GTK_ALIGN_START);

    /* Drop it out of a column header, a few columns in, so the whole
     * header row stays visible above the menu. */
    GdkRectangle rect = { 330, 24, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    gtk_popover_popup(GTK_POPOVER(popover));
}

/* The Batch Stamp form is a plain GtkWindow, so it is not in
 * gtk_application_get_windows() - find it among all toplevels by title. */
static GtkWindow *find_toplevel(const char *title) {
    GListModel *toplevels = gtk_window_get_toplevels();
    guint n = g_list_model_get_n_items(toplevels);
    for (guint i = 0; i < n; i++) {
        GtkWindow *win = g_list_model_get_item(toplevels, i);
        g_object_unref(win);
        if (g_strcmp0(gtk_window_get_title(win), title) == 0) {
            return win;
        }
    }
    return NULL;
}

static gboolean fill_stamp_entry(gpointer data) {
    (void)data;
    GtkWindow *dialog = find_toplevel("Example");
    if (dialog == NULL) {
        return G_SOURCE_REMOVE;
    }
    GtkWidget *grid = find_descendant(GTK_WIDGET(dialog), GTK_TYPE_GRID);
    if (grid != NULL) {
        GtkWidget *entry = gtk_grid_get_child_at(GTK_GRID(grid), 1, 0);
        if (GTK_IS_EDITABLE(entry)) {
            gtk_editable_set_text(GTK_EDITABLE(entry), "Hexbeam");
        }
    }
    return G_SOURCE_REMOVE;
}

/* Leave the grid with no row selected and nothing focused, so the "at
 * rest" shot doesn't show row 0 half-highlighted. */
static void clear_grid_state(GtkWindow *window) {
    GtkWidget *cv = find_descendant(GTK_WIDGET(window), GTK_TYPE_COLUMN_VIEW);
    if (cv == NULL) {
        return;
    }
    GtkSelectionModel *model = gtk_column_view_get_model(GTK_COLUMN_VIEW(cv));
    if (GTK_IS_SINGLE_SELECTION(model)) {
        GtkSingleSelection *sel = GTK_SINGLE_SELECTION(model);
        gtk_single_selection_set_autoselect(sel, FALSE);
        gtk_single_selection_set_can_unselect(sel, TRUE); /* else set_selected is refused */
        gtk_single_selection_set_selected(sel, GTK_INVALID_LIST_POSITION);
    }
    gtk_selection_model_unselect_all(model);
    GtkWidget *bar = find_descendant(GTK_WIDGET(window), GTK_TYPE_POPOVER_MENU_BAR);
    if (bar != NULL) {
        gtk_widget_grab_focus(bar);
    }
}

/* Drop open the first menu-bar menu (File). */
static void show_file_menu(GtkWindow *window) {
    GtkWidget *bar = find_descendant(GTK_WIDGET(window), GTK_TYPE_POPOVER_MENU_BAR);
    GtkWidget *file_item = bar != NULL ? gtk_widget_get_first_child(bar) : NULL;
    GtkWidget *popover = file_item != NULL ? find_descendant(file_item, GTK_TYPE_POPOVER) : NULL;
    if (popover != NULL) {
        gtk_popover_popup(GTK_POPOVER(popover));
    }
}

static gboolean on_trigger(gpointer data) {
    GtkApplication *app = data;
    GList *windows = gtk_application_get_windows(app);
    if (windows == NULL) {
        return G_SOURCE_REMOVE;
    }
    GtkWindow *window = windows->data;

    if (g_strcmp0(g_mode, "main") == 0) {
        clear_grid_state(window);
    } else if (g_strcmp0(g_mode, "filemenu") == 0) {
        show_file_menu(window);
    } else if (g_strcmp0(g_mode, "colmenu") == 0) {
        show_column_menu(window);
    } else if (g_strcmp0(g_mode, "stamp") == 0) {
        g_action_group_activate_action(G_ACTION_GROUP(window), "run_stamp",
                                       g_variant_new_string(g_stamp_path));
        /* Give the form a beat to appear, then type a value into the
         * first field so the shot shows real use. */
        g_timeout_add(400, fill_stamp_entry, NULL);
    } else if (g_strcmp0(g_mode, "replace") == 0) {
        g_action_group_activate_action(G_ACTION_GROUP(window), "find_replace", NULL);
        GtkWidget *entry = find_descendant(GTK_WIDGET(window), GTK_TYPE_SEARCH_ENTRY);
        if (entry != NULL) {
            gtk_editable_set_text(GTK_EDITABLE(entry), "FT8");
        }
        GtkWidget *revealer = find_descendant(GTK_WIDGET(window), GTK_TYPE_REVEALER);
        GtkWidget *repl = revealer != NULL
                              ? find_descendant(gtk_revealer_get_child(GTK_REVEALER(revealer)),
                                                GTK_TYPE_ENTRY)
                              : NULL;
        if (repl != NULL) {
            gtk_editable_set_text(GTK_EDITABLE(repl), "FT4");
        }
    }
    return G_SOURCE_REMOVE;
}

static void on_startup(GApplication *app, gpointer user_data) {
    (void)user_data;
    adifedit_app_startup(GTK_APPLICATION(app));
}

static void on_open(GApplication *app, GFile **files, gint n_files, const char *hint,
                    gpointer user_data) {
    (void)hint;
    (void)user_data;
    for (gint i = 0; i < n_files; i++) {
        adifedit_window_open(GTK_APPLICATION(app), files[i]);
    }
    g_timeout_add(g_trigger_ms, on_trigger, app);
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        g_printerr("usage: %s MODE SAMPLE.adi STAMP.bst TRIGGER_MS\n", argv[0]);
        return 2;
    }
    g_mode = argv[1];
    g_stamp_path = argv[3];
    g_trigger_ms = (guint)g_ascii_strtoull(argv[4], NULL, 10);

    GtkApplication *app = gtk_application_new("io.github.nheathlabs.adifedit.shot",
                                              G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);

    char *argv2[] = { argv[0], argv[2], NULL };
    int status = g_application_run(G_APPLICATION(app), 2, argv2);
    g_object_unref(app);
    return status;
}
