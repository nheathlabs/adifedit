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

#include "ui_dialogs.h"

#include <string.h>

void ui_show_error(GtkWindow *parent, const char *message) {
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", message);
    gtk_alert_dialog_show(dialog, parent);
    g_object_unref(dialog);
}

/* ------------------------------------------------------------------- */
/* Modal-dialog shell (shared by the text prompt and the Batch Stamp    */
/* form)                                                                */
/* ------------------------------------------------------------------- */

GtkWidget *ui_modal_dialog(GtkWindow *parent, const char *title, GtkWidget **content,
                           gpointer context, GDestroyNotify context_free) {
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    if (context != NULL || context_free != NULL) {
        g_object_set_data_full(G_OBJECT(dialog), "ui-modal-context", context, context_free);
    }

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_window_set_child(GTK_WINDOW(dialog), box);

    *content = box;
    return dialog;
}

GtkWidget *ui_dialog_button_row(GtkWidget *dialog, const char *confirm_label, GtkWidget **confirm) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_END);

    GtkWidget *cancel = gtk_button_new_with_mnemonic("_Cancel");
    *confirm = gtk_button_new_with_mnemonic(confirm_label);
    gtk_widget_add_css_class(*confirm, "suggested-action");
    gtk_box_append(GTK_BOX(row), cancel);
    gtk_box_append(GTK_BOX(row), *confirm);

    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_window_set_default_widget(GTK_WINDOW(dialog), *confirm);
    return row;
}

/* ------------------------------------------------------------------- */
/* Text prompt                                                          */
/* ------------------------------------------------------------------- */

typedef struct {
    GtkWidget *dialog;
    GtkWidget *entry;
    UiTextPromptFunc callback;
    gpointer user_data;
    GDestroyNotify user_data_free;
} TextPrompt;

static void text_prompt_free(gpointer data) {
    TextPrompt *prompt = data;
    if (prompt->user_data_free != NULL) {
        prompt->user_data_free(prompt->user_data);
    }
    g_free(prompt);
}

static void on_prompt_confirm(G_GNUC_UNUSED GtkButton *button, gpointer data) {
    TextPrompt *prompt = data;

    const char *text = gtk_editable_get_text(GTK_EDITABLE(prompt->entry));
    char *problem = prompt->callback(text, prompt->user_data);

    if (problem != NULL) {
        ui_show_error(GTK_WINDOW(prompt->dialog), problem);
        g_free(problem);
        return;
    }
    gtk_window_destroy(GTK_WINDOW(prompt->dialog));
}

/* Everything ui_text_prompt and ui_text_prompt_with_suggestions share:
 * the TextPrompt, the dialog, the label, the entry, and the button row.
 * Doesn't present the window or grab focus - the caller does that once
 * it's done adding anything extra (e.g. suggestions) to the entry. */
static TextPrompt *build_text_prompt(GtkWindow *parent, const char *title, const char *prompt_text,
                                     const char *placeholder, const char *confirm_label,
                                     UiTextPromptFunc callback, gpointer user_data,
                                     GDestroyNotify user_data_free) {
    TextPrompt *prompt = g_new0(TextPrompt, 1);
    prompt->callback = callback;
    prompt->user_data = user_data;
    prompt->user_data_free = user_data_free;

    GtkWidget *box = NULL;
    prompt->dialog = ui_modal_dialog(parent, title, &box, prompt, text_prompt_free);

    GtkWidget *label = gtk_label_new(prompt_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);

    prompt->entry = gtk_entry_new();
    if (placeholder != NULL) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(prompt->entry), placeholder);
    }
    gtk_entry_set_activates_default(GTK_ENTRY(prompt->entry), TRUE);

    GtkWidget *confirm = NULL;
    GtkWidget *button_box = ui_dialog_button_row(prompt->dialog, confirm_label, &confirm);
    g_signal_connect(confirm, "clicked", G_CALLBACK(on_prompt_confirm), prompt);

    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), prompt->entry);
    gtk_box_append(GTK_BOX(box), button_box);

    return prompt;
}

void ui_text_prompt(GtkWindow *parent, const char *title, const char *prompt_text,
                    const char *placeholder, const char *confirm_label, UiTextPromptFunc callback,
                    gpointer user_data, GDestroyNotify user_data_free) {
    TextPrompt *prompt = build_text_prompt(parent, title, prompt_text, placeholder, confirm_label,
                                           callback, user_data, user_data_free);
    gtk_window_present(GTK_WINDOW(prompt->dialog));
    gtk_widget_grab_focus(prompt->entry);
}

/* ------------------------------------------------------------------- */
/* Text prompt with suggestions                                         */
/* ------------------------------------------------------------------- */

/*
 * Every rendered row stores its full suggestion string as object data
 * ("adif-suggestion-name", borrowed - the strings live in the caller's
 * static `suggestions` array, never freed here).
 */
typedef struct {
    GtkWidget *entry;             /* borrowed; owned by the dialog */
    GtkWidget *popover;           /* parented to entry; torn down with it */
    GtkWidget *list_box;          /* inside popover's scroller */
    const char *const *suggestions; /* borrowed; caller-owned, NULL-terminated */
    gulong changed_handler;       /* blocked while we set the entry text ourselves */
    gboolean open;                /* mirrors whether popover is popped up */
} SuggestState;

/* Always call through to GTK rather than skipping when `s->open` already
 * says so: GtkPopover's show/hide is transition-animated, and a popup()
 * arriving while a just-started popdown() transition is still finishing
 * can be silently ignored by GTK ("already visible"). If that happens
 * while we'd skipped the real call because our own flag already said
 * TRUE, the popover is stuck closed forever - every later popup() is
 * skipped by the same stale flag, with nothing left to notice the
 * mistake. `open` is kept only for our own dispatch logic (e.g. "Down
 * arrow: open vs move selection"), never to gate whether GTK is asked. */
static void suggest_popup(SuggestState *s) {
    gtk_popover_popup(GTK_POPOVER(s->popover));
    s->open = TRUE;
}

static void suggest_popdown(SuggestState *s) {
    gtk_popover_popdown(GTK_POPOVER(s->popover));
    s->open = FALSE;
}

/* Append one row per name in `names` (already in the desired order). */
static void append_suggestion_rows(GtkWidget *list_box, GPtrArray *names) {
    for (guint i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        GtkWidget *label = gtk_label_new(name);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_margin_start(label, 8);
        gtk_widget_set_margin_end(label, 8);
        gtk_widget_set_margin_top(label, 2);
        gtk_widget_set_margin_bottom(label, 2);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data(G_OBJECT(row), "adif-suggestion-name", (gpointer)name);
        gtk_list_box_append(GTK_LIST_BOX(list_box), row);
    }
}

/* Re-filter `s->suggestions` against the entry's current text and
 * repaint the list: matches whose name *starts with* the typed text
 * first (alphabetical - the source array already is), then matches that
 * merely contain it elsewhere (also alphabetical). Empty text matches
 * everything as a "starts with", i.e. shows the full list. Selects row 0
 * if the result is non-empty; does not itself open/close the popover. */
static void rebuild_suggestions(SuggestState *s) {
    char *query = g_ascii_strup(gtk_editable_get_text(GTK_EDITABLE(s->entry)), -1);
    g_strstrip(query);

    gtk_list_box_remove_all(GTK_LIST_BOX(s->list_box));

    GPtrArray *starts_with = g_ptr_array_new();
    GPtrArray *contains = g_ptr_array_new();
    for (guint i = 0; s->suggestions[i] != NULL; i++) {
        const char *name = s->suggestions[i];
        if (g_str_has_prefix(name, query)) {
            g_ptr_array_add(starts_with, (gpointer)name);
        } else if (strstr(name, query) != NULL) {
            g_ptr_array_add(contains, (gpointer)name);
        }
    }
    g_free(query);

    append_suggestion_rows(s->list_box, starts_with);
    append_suggestion_rows(s->list_box, contains);
    g_ptr_array_unref(starts_with);
    g_ptr_array_unref(contains);

    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(s->list_box), 0);
    if (first != NULL) {
        gtk_list_box_select_row(GTK_LIST_BOX(s->list_box), first);
    }
}

/* Fill the entry with `row`'s suggestion, cursor at the end, and close
 * the popover. No-op if `row` is NULL (nothing highlighted). */
static void accept_suggestion_row(SuggestState *s, GtkListBoxRow *row) {
    if (row == NULL) {
        return;
    }
    const char *name = g_object_get_data(G_OBJECT(row), "adif-suggestion-name");

    /* Block "changed" for our own programmatic set - else this would
     * immediately re-trigger rebuild_suggestions()/re-open the popover
     * we're about to close (same idiom as adif_cell_set_text_silently()
     * in adif_table_view.c). */
    g_signal_handler_block(s->entry, s->changed_handler);
    gtk_editable_set_text(GTK_EDITABLE(s->entry), name);
    g_signal_handler_unblock(s->entry, s->changed_handler);
    gtk_editable_set_position(GTK_EDITABLE(s->entry), -1);

    suggest_popdown(s);
}

static void move_suggestion_selection(SuggestState *s, int delta) {
    GtkListBoxRow *current = gtk_list_box_get_selected_row(GTK_LIST_BOX(s->list_box));
    int index = current != NULL ? gtk_list_box_row_get_index(current) : -1;
    GtkListBoxRow *next = gtk_list_box_get_row_at_index(GTK_LIST_BOX(s->list_box), index + delta);
    if (next != NULL) {
        gtk_list_box_select_row(GTK_LIST_BOX(s->list_box), next);
    }
}

/* Re-filter for the entry's current text, then show the popover if
 * anything matched or leave/put it away if not - shared by every path
 * that can bring the popover up (typing, Down with it closed, the
 * dropdown icon), so they agree on not popping up an empty list. */
static void refresh_and_open_if_matches(SuggestState *s) {
    rebuild_suggestions(s);
    if (gtk_list_box_get_row_at_index(GTK_LIST_BOX(s->list_box), 0) != NULL) {
        suggest_popup(s);
    } else {
        suggest_popdown(s);
    }
}

/* Typing (including backspacing to empty) live-refilters. Doesn't fire
 * from our own accept_suggestion_row() - that blocks this handler
 * first. */
static void on_suggest_changed(G_GNUC_UNUSED GtkEditable *editable, gpointer user_data) {
    refresh_and_open_if_matches(user_data);
}

/* GTK_PHASE_CAPTURE on the entry, so this runs before the entry's own
 * built-in Tab-focus-move / Enter-activates-default handling (same
 * capture-phase idiom as adif_table_view.c's cell right-click gesture,
 * which needs to win over the entry's built-in context menu). Returns
 * GDK_EVENT_STOP to consume a key (Tab/Enter/Escape acting on the
 * popover instead of their usual job), GDK_EVENT_PROPAGATE to let it
 * fall through to normal behaviour. */
static gboolean on_suggest_key(G_GNUC_UNUSED GtkEventControllerKey *controller, guint keyval,
                               G_GNUC_UNUSED guint keycode, G_GNUC_UNUSED GdkModifierType state,
                               gpointer user_data) {
    SuggestState *s = user_data;

    switch (keyval) {
        case GDK_KEY_Down:
        case GDK_KEY_KP_Down:
            if (!s->open) {
                refresh_and_open_if_matches(s);
            } else {
                move_suggestion_selection(s, 1);
            }
            return GDK_EVENT_STOP;

        case GDK_KEY_Up:
        case GDK_KEY_KP_Up:
            if (!s->open) {
                return GDK_EVENT_PROPAGATE;
            }
            move_suggestion_selection(s, -1);
            return GDK_EVENT_STOP;

        case GDK_KEY_Tab:
        case GDK_KEY_KP_Tab:
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter: {
            /* Tab completes; the first Enter also completes (and stops
             * there - a second Enter, with the popover now closed,
             * falls through untouched to the entry's own
             * activates-default handling and submits normally). */
            GtkListBoxRow *row = s->open ? gtk_list_box_get_selected_row(GTK_LIST_BOX(s->list_box))
                                         : NULL;
            if (row == NULL) {
                return GDK_EVENT_PROPAGATE;
            }
            accept_suggestion_row(s, row);
            return GDK_EVENT_STOP;
        }

        case GDK_KEY_Escape:
            if (!s->open) {
                return GDK_EVENT_PROPAGATE;
            }
            suggest_popdown(s);
            return GDK_EVENT_STOP;

        default:
            return GDK_EVENT_PROPAGATE;
    }
}

static void on_suggest_row_activated(G_GNUC_UNUSED GtkListBox *box, GtkListBoxRow *row,
                                     gpointer user_data) {
    SuggestState *s = user_data;
    accept_suggestion_row(s, row);
    gtk_widget_grab_focus(s->entry);
}

/* Losing focus (Cancel/Insert click, tabbing away) closes any open
 * popover so nothing is left floating (same idiom as
 * on_cell_focus_leave() in adif_table_view.c). */
static void on_suggest_focus_leave(G_GNUC_UNUSED GtkEventControllerFocus *controller,
                                   gpointer user_data) {
    suggest_popdown(user_data);
}

/* The trailing dropdown icon toggles the popover using the same
 * filtered-by-current-text view typing would produce - "the dropdown
 * that can be perused" is the same list, not a separate unfiltered one. */
static void on_suggest_icon_press(GtkEntry *entry, GtkEntryIconPosition icon_pos,
                                  gpointer user_data) {
    if (icon_pos != GTK_ENTRY_ICON_SECONDARY) {
        return;
    }
    SuggestState *s = user_data;
    if (s->open) {
        suggest_popdown(s);
    } else {
        refresh_and_open_if_matches(s);
    }
    gtk_widget_grab_focus(GTK_WIDGET(entry));
}

void ui_text_prompt_with_suggestions(GtkWindow *parent, const char *title, const char *prompt_text,
                                     const char *placeholder, const char *confirm_label,
                                     const char *const *suggestions, UiTextPromptFunc callback,
                                     gpointer user_data, GDestroyNotify user_data_free) {
    TextPrompt *prompt = build_text_prompt(parent, title, prompt_text, placeholder, confirm_label,
                                           callback, user_data, user_data_free);

    /* Wide enough for a typical ADIF field name so the dialog isn't
     * oddly narrow next to the popover that opens under it. */
    gtk_editable_set_width_chars(GTK_EDITABLE(prompt->entry), 28);

    SuggestState *suggest = g_new0(SuggestState, 1);
    suggest->entry = prompt->entry;
    suggest->suggestions = suggestions;

    suggest->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(suggest->list_box), GTK_SELECTION_BROWSE);
    g_signal_connect(suggest->list_box, "row-activated", G_CALLBACK(on_suggest_row_activated),
                     suggest);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), 220);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), suggest->list_box);

    /* autohide FALSE: the popover must not steal keyboard focus from the
     * entry, or the user couldn't keep typing while it's open. Works
     * fine nested in this already-modal dialog - a popover is a surface
     * child of its parent's toplevel, so it's inside the same input
     * grab (same mechanism the cell right-click popover in adif_table.c
     * already relies on), and it isn't clipped to the dialog's (small,
     * non-resizable) bounds. */
    suggest->popover = gtk_popover_new();
    gtk_popover_set_autohide(GTK_POPOVER(suggest->popover), FALSE);
    gtk_popover_set_has_arrow(GTK_POPOVER(suggest->popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(suggest->popover), GTK_POS_BOTTOM);
    gtk_popover_set_child(GTK_POPOVER(suggest->popover), scroller);
    gtk_widget_set_parent(suggest->popover, prompt->entry);
    /* A plain GtkEntry doesn't know to unparent an ad-hoc extra child
     * like this on its own teardown (it wasn't designed as a generic
     * container) - without this it's finalized still holding the
     * popover, and GTK warns. Same "destroy" -> unparent hook the cell
     * right-click popover uses in adif_table.c, same reason. */
    g_signal_connect_swapped(prompt->entry, "destroy", G_CALLBACK(gtk_widget_unparent),
                             suggest->popover);

    suggest->changed_handler = g_signal_connect(prompt->entry, "changed",
                                                G_CALLBACK(on_suggest_changed), suggest);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_suggest_key), suggest);
    gtk_widget_add_controller(prompt->entry, keys);

    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_suggest_focus_leave), suggest);
    gtk_widget_add_controller(prompt->entry, focus);

    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(prompt->entry), GTK_ENTRY_ICON_SECONDARY,
                                      "pan-down-symbolic");
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(prompt->entry), GTK_ENTRY_ICON_SECONDARY,
                                    "Browse standard ADIF field names");
    g_signal_connect(prompt->entry, "icon-press", G_CALLBACK(on_suggest_icon_press), suggest);

    /* Ties SuggestState's lifetime to the entry - both are destroyed
     * together with the rest of the dialog in one gtk_window_destroy(). */
    g_object_set_data_full(G_OBJECT(prompt->entry), "adif-suggest-state", suggest, g_free);

    gtk_window_present(GTK_WINDOW(prompt->dialog));
    gtk_widget_grab_focus(prompt->entry);
}
