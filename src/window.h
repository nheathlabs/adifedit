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

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Build the application's main window and show it. */
void adifedit_window_present(GtkApplication *app);

/* Same, then start loading `file`. */
void adifedit_window_open(GtkApplication *app, GFile *file);

/* One-time, per-application setup the windows rely on: the bundled icons,
 * the app stylesheet, and the keyboard accelerators for the window actions. Call once, from
 * the application's "startup". */
void adifedit_app_startup(GtkApplication *app);

G_END_DECLS
