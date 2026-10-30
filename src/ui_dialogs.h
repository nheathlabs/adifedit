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

/* Show a simple modal error alert. */
void ui_show_error(GtkWindow *parent, const char *message);

/*
 * A modal, non-resizable dialog window transient for `parent`, titled
 * `title`, whose child is a 12px vertical box with 18px margins. Append
 * your rows to *content. The window is unowned - present it; it destroys
 * itself when closed. `context` / `context_free`, if given, are tied to
 * the window's lifetime (somewhere to hang the caller's state).
 */
GtkWidget *ui_modal_dialog(GtkWindow *parent, const char *title, GtkWidget **content,
                           gpointer context, GDestroyNotify context_free);

/*
 * A right-aligned "[Cancel] [<confirm_label>]" row for a ui_modal_dialog.
 * Cancel closes `dialog`; the confirm button gets the `suggested-action`
 * style, becomes the window's default widget, and is returned via
 * *confirm for the caller to connect a "clicked" handler. Returns the row
 * for the caller to append (or keep, to swap out later).
 */
GtkWidget *ui_dialog_button_row(GtkWidget *dialog, const char *confirm_label, GtkWidget **confirm);

/*
 * The result of a text-prompt confirmation:
 *   NULL  - accept the input and close the dialog.
 *   other - a newly-allocated message; it is shown as an error (parented
 *           to the prompt) and the dialog stays open for another try.
 *           ui_text_prompt takes ownership of the string.
 */
typedef char *(*UiTextPromptFunc)(const char *text, gpointer user_data);

/*
 * Present a modal single-line text prompt transient for `parent`. When
 * the user confirms, `callback` is invoked with the entered text
 * verbatim. Cancel just closes the dialog. `user_data_free`, if given,
 * runs when the dialog is destroyed.
 */
void ui_text_prompt(GtkWindow *parent, const char *title, const char *prompt,
                    const char *placeholder, const char *confirm_label, UiTextPromptFunc callback,
                    gpointer user_data, GDestroyNotify user_data_free);

/*
 * Like ui_text_prompt, but the entry offers autocomplete: a popover
 * listing entries of `suggestions` (a NULL-terminated, ASCII-sorted
 * array - not copied, must outlive the dialog; a static const array is
 * expected) that match what's typed so far - case-insensitive, matching
 * anywhere in the name, with matches whose *start* matches ranked first.
 * Tab (or Enter, or a click) accepts the highlighted row; Down/Up (or
 * the entry's trailing dropdown icon) browse the list, including the
 * full list when nothing's typed yet; Escape dismisses just the popover.
 * Accepting a suggestion only fills the entry - `callback` is still the
 * sole gate on what's actually accepted, exactly as in ui_text_prompt;
 * text that matches no suggestion remains fully valid input.
 */
void ui_text_prompt_with_suggestions(GtkWindow *parent, const char *title, const char *prompt,
                                     const char *placeholder, const char *confirm_label,
                                     const char *const *suggestions, UiTextPromptFunc callback,
                                     gpointer user_data, GDestroyNotify user_data_free);

G_END_DECLS
