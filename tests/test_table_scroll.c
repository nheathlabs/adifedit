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

/* AdifTable view tests. These show a real window, so ctest runs them
 * through tests/run_headless.sh; run by hand without a display they
 * report "skipped". */

#include <gtk/gtk.h>

#include "adif_document.h"
#include "adif_table.h"

#define N_COLUMNS 40
#define N_ROWS 20

static void spin_for(guint ms) {
    gint64 until = g_get_monotonic_time() + ((gint64)ms * 1000);
    while (g_get_monotonic_time() < until) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
}

/* The cell widget under the middle of `scrolled`'s viewport. */
static GtkWidget *cell_in_view(GtkWidget *scrolled) {
    double x = gtk_widget_get_width(scrolled) / 2.0;
    double y = gtk_widget_get_height(scrolled) / 2.0;
    GtkWidget *hit = gtk_widget_pick(scrolled, x, y, GTK_PICK_DEFAULT);
    while (hit != NULL && !gtk_widget_has_css_class(hit, "adif-cell")) {
        hit = gtk_widget_get_parent(hit);
    }
    g_assert_nonnull(hit);
    return hit;
}

static AdifDocument *wide_document(void) {
    AdifDocument *doc = adif_document_new();
    for (guint c = 0; c < N_COLUMNS; c++) {
        char *name = g_strdup_printf("FIELD_%02u", c);
        adif_document_insert_column(doc, c, name);
        g_free(name);
    }
    for (guint r = 0; r < N_ROWS; r++) {
        adif_document_insert_row(doc, r);
        for (guint c = 0; c < N_COLUMNS; c++) {
            char *value = g_strdup_printf("value %u.%u", r, c);
            adif_document_set_value(doc, r, adif_document_column_data_index_at(doc, c), value);
            g_free(value);
        }
    }
    return doc;
}

static void test_insert_keeps_scroll(void) {
    AdifDocument *doc = wide_document();
    AdifTable *table = adif_table_new(doc);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), adif_table_get_widget(table));
    GtkWidget *window = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 400);
    gtk_window_set_child(GTK_WINDOW(window), scrolled);
    gtk_window_present(GTK_WINDOW(window));
    spin_for(1000); /* map, lay out, auto-fit */

    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scrolled));
    double range = gtk_adjustment_get_upper(hadj) - gtk_adjustment_get_page_size(hadj);
    g_assert_cmpfloat(range, >, 1000); /* the table really is wider than the window */

    double before = range / 2;
    gtk_adjustment_set_value(hadj, before);
    spin_for(300);
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(hadj), before, 1);

    /* The user is working in the visible part of the table: a cell there
     * has the focus, as it would after a click or the cell menu. */
    g_assert_true(gtk_widget_grab_focus(cell_in_view(scrolled)));
    spin_for(300);
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(hadj), before, 1);

    guint new_di = adif_document_insert_column(doc, N_COLUMNS / 2, "NEW_ONE");
    spin_for(1000);
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(hadj), before, 1);

    /* ...and taking it out again. */
    g_assert_true(adif_document_delete_column(doc, new_di));
    spin_for(1000);
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(hadj), before, 1);

    /* Deleting the column the focused cell is in takes that cell away
     * while it has the focus; its late focus-leave must stay harmless. */
    guint focused = adif_table_current_column(table);
    g_assert_cmpuint(focused, <, adif_document_get_n_columns(doc));
    g_assert_true(
        adif_document_delete_column(doc, adif_document_column_data_index_at(doc, focused)));
    spin_for(500);

    adif_table_free(table); /* before the widgets go, as the app window does */
    gtk_window_destroy(GTK_WINDOW(window));
    spin_for(100);
    g_object_unref(doc);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check()) {
        g_test_skip("no display");
        return 77;
    }
    g_test_add_func("/table/insert-column-keeps-scroll", test_insert_keeps_scroll);
    return g_test_run();
}
