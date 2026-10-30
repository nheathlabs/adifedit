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

#include <glib.h>

G_BEGIN_DECLS

/*
 * Time-slicing for work spread over main-loop turns (loading a file's
 * rows, applying a stamp, auto-fitting columns). Each turn runs until a
 * deadline, then yields so the main loop can paint and handle input.
 */

/* How long one turn may run - long enough to amortise the per-turn
 * overhead, short enough that the UI (and any spinner) keeps moving. */
#define ADIF_SLICE_BUDGET_US 8000

/* Rows between clock checks - frequent enough that a turn doesn't overrun
 * its deadline by much, infrequent enough that g_get_monotonic_time()
 * itself isn't a meaningful cost. */
#define ADIF_SLICE_CHECK_ROWS 256

/* The deadline for a turn starting now. */
static inline gint64 adif_slice_deadline(void) {
    return g_get_monotonic_time() + ADIF_SLICE_BUDGET_US;
}

/* Count one row of work against `*since_check`; every
 * ADIF_SLICE_CHECK_ROWS rows, look at the clock. TRUE once `deadline`
 * has passed - time to yield. */
static inline gboolean adif_slice_expired(guint *since_check, gint64 deadline) {
    if (++*since_check < ADIF_SLICE_CHECK_ROWS) {
        return FALSE;
    }
    *since_check = 0;
    return g_get_monotonic_time() >= deadline;
}

G_END_DECLS
