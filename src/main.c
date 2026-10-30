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

#include <gtk/gtk.h>

#include "build_info.h"
#include "window.h"

static const GOptionEntry OPTION_ENTRIES[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, NULL, "Show version information and exit", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
};

/* --version is handled here, before "startup"/"activate" - so it needs no
 * display and doesn't register on the session bus just to print a line.
 * Returning a non-negative status tells GApplication to exit immediately
 * with that status instead of continuing on to run the application. */
static gint on_handle_local_options(G_GNUC_UNUSED GApplication *app, GVariantDict *options,
                                    G_GNUC_UNUSED gpointer user_data) {
    if (g_variant_dict_contains(options, "version")) {
        g_print("adifedit %s (git %s)\n", ADIFEDIT_VERSION, adifedit_git_hash());
        return 0;
    }
    return -1; /* not handled - continue normal startup */
}

/* One-time application setup: bundled icons, stylesheet, accelerators. */
static void on_startup(GApplication *app, G_GNUC_UNUSED gpointer user_data) {
    adifedit_app_startup(GTK_APPLICATION(app));
}

static void on_activate(GtkApplication *app, G_GNUC_UNUSED gpointer user_data) {
    adifedit_window_present(app);
}

/* Handles `adifedit file.adi` and files opened from a file manager. */
static void on_open(GApplication *app, GFile **files, gint n_files, G_GNUC_UNUSED const char *hint,
                    G_GNUC_UNUSED gpointer user_data) {
    for (gint i = 0; i < n_files; i++) {
        adifedit_window_open(GTK_APPLICATION(app), files[i]);
    }
}

int main(int argc, char *argv[]) {
#ifdef G_OS_WIN32
    /* On Windows, GTK's GPU renderers paint the margin it keeps around
     * each window for the drop shadow solid black - every window, dialog
     * and menu gets a thick black frame. Cairo draws it transparent. Only
     * a default: a GSK_RENDERER already set still wins. Must happen before
     * GTK initialises (in g_application_run()). */
    g_setenv("GSK_RENDERER", "cairo", FALSE);
#endif

    /* The application ID is also the GResource prefix (window.c,
     * adifedit.gresource.xml) and the base name of the .desktop and mime
     * files, their Icon= keys, the About dialog's logo-icon-name and the
     * installed app icon. Change them together. */
    GtkApplication *app = gtk_application_new("io.github.nheathlabs.adifedit",
                                              G_APPLICATION_HANDLES_OPEN);
    g_application_add_main_option_entries(G_APPLICATION(app), OPTION_ENTRIES);
    g_application_set_option_context_parameter_string(G_APPLICATION(app), "[FILE…]");
    g_application_set_option_context_summary(
        G_APPLICATION(app), "View and edit ham radio ADIF logs as an editable grid.");
    g_application_set_option_context_description(
        G_APPLICATION(app), "FILE is one or more .adi / .adif files to open on start.");
    g_signal_connect(app, "handle-local-options", G_CALLBACK(on_handle_local_options), NULL);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return status;
}
