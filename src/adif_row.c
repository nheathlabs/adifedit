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

#include "adif_row.h"

struct _AdifRow {
    GObject parent_instance;
    GPtrArray *values; /* owned char*, in column order */
    GArray *repaired;  /* guint indices the parser repaired; NULL (the usual
                          case) when there are none */
};

G_DEFINE_TYPE(AdifRow, adif_row, G_TYPE_OBJECT)

static void adif_row_finalize(GObject *object) {
    AdifRow *self = ADIF_ROW(object);
    g_clear_pointer(&self->values, g_ptr_array_unref);
    g_clear_pointer(&self->repaired, g_array_unref);
    G_OBJECT_CLASS(adif_row_parent_class)->finalize(object);
}

static void adif_row_class_init(AdifRowClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = adif_row_finalize;
}

static void adif_row_init(AdifRow *self) {
    self->values = g_ptr_array_new_with_free_func(g_free);
}

AdifRow *adif_row_new(void) {
    return g_object_new(ADIF_TYPE_ROW, NULL);
}

void adif_row_add_value(AdifRow *self, const char *value) {
    g_return_if_fail(ADIF_IS_ROW(self));
    g_ptr_array_add(self->values, g_strdup(value != NULL ? value : ""));
}

const char *adif_row_get_value(AdifRow *self, guint index) {
    g_return_val_if_fail(ADIF_IS_ROW(self), "");
    if (index >= self->values->len) {
        return "";
    }
    const char *value = g_ptr_array_index(self->values, index);
    return value != NULL ? value : "";
}

void adif_row_set_value(AdifRow *self, guint index, const char *value) {
    g_return_if_fail(ADIF_IS_ROW(self));
    if (value == NULL) {
        value = "";
    }
    if (index < self->values->len
        && g_strcmp0(g_ptr_array_index(self->values, index), value) == 0) {
        return; /* unchanged - skip the reallocation */
    }
    while (self->values->len <= index) {
        g_ptr_array_add(self->values, g_strdup(""));
    }
    g_free(g_ptr_array_index(self->values, index));
    g_ptr_array_index(self->values, index) = g_strdup(value);
}

gboolean adif_row_is_repaired(AdifRow *self, guint index) {
    g_return_val_if_fail(ADIF_IS_ROW(self), FALSE);
    if (self->repaired == NULL) {
        return FALSE;
    }
    for (guint i = 0; i < self->repaired->len; i++) {
        if (g_array_index(self->repaired, guint, i) == index) {
            return TRUE;
        }
    }
    return FALSE;
}

void adif_row_set_repaired(AdifRow *self, guint index) {
    g_return_if_fail(ADIF_IS_ROW(self));
    if (adif_row_is_repaired(self, index)) {
        return;
    }
    if (self->repaired == NULL) {
        self->repaired = g_array_new(FALSE, FALSE, sizeof(guint));
    }
    g_array_append_val(self->repaired, index);
}
