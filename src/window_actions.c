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
 * Everything the user can trigger from the main window: File actions, the
 * context-menu column/row actions, the toolbar buttons, the threaded
 * load/save they drive, and the unsaved-changes guard on close. All of it
 * mutates the AppWindow's AdifDocument; window.c builds the widgets.
 */

#include "window_priv.h"

#include "adif_io.h"
#include "adif_known_fields.h"
#include "build_info.h"
#include "time_slice.h"
#include "ui_dialogs.h"

static void run_save_as(AppWindow *w);
static void run_pending(AppWindow *w);

/* ------------------------------------------------------------------- */
/* Loading                                                              */
/* ------------------------------------------------------------------- */

typedef struct {
    AppWindow *w;              /* ref held for the whole load */
    char *path;                /* becomes current_filepath only if the load succeeds */
    char *etag;                /* the file's etag before it was read: becomes file_etag */
    AdifParseResult *result;   /* owned once the parse succeeds, until finish()/cancel() */
    AdifLoadStep *step;        /* NULL until the parse succeeds, and after finish()/cancel() */
} LoadContext;

/* The main window closed while this load was still in flight.
 * app_window_dispose() clears `doc`, so a NULL doc means there is nothing
 * left to load into - mirrors stamp_abandoned() in window_stamp.c. */
static gboolean load_abandoned(LoadContext *ctx) {
    return ctx->w->doc == NULL;
}

static void load_context_free(LoadContext *ctx) {
    if (ctx->step != NULL) {
        adif_document_load_parse_cancel(ctx->step);
    }
    if (ctx->result != NULL) {
        adif_parse_result_free(ctx->result);
    }
    g_object_unref(ctx->w);
    g_free(ctx->path);
    g_free(ctx->etag);
    g_free(ctx);
}

/* Repeating idle source: builds and splices in one time-boxed chunk of
 * rows per turn, so the main loop (and the status-bar spinner) keeps
 * running between chunks instead of blocking for the whole file. */
static gboolean load_step_idle(gpointer user_data) {
    LoadContext *ctx = user_data;

    if (load_abandoned(ctx)) {
        load_context_free(ctx);
        return G_SOURCE_REMOVE;
    }

    if (adif_document_load_parse_step(ctx->step, adif_slice_deadline())) {
        return G_SOURCE_CONTINUE;
    }

    AppWindow *w = ctx->w;
    adif_document_load_parse_finish(ctx->step);
    ctx->step = NULL;

    char *warning = g_steal_pointer(&ctx->result->warning);
    g_clear_pointer(&ctx->result, adif_parse_result_free);

    g_free(w->current_filepath);
    w->current_filepath = g_steal_pointer(&ctx->path);
    g_free(w->file_etag);
    w->file_etag = g_steal_pointer(&ctx->etag);
    /* A file that didn't parse cleanly is never written back to. */
    g_free(w->parse_warn_path);
    w->parse_warn_path = (warning != NULL) ? g_strdup(w->current_filepath) : NULL;

    app_window_set_loading(w, FALSE, NULL); /* re-enables the actions for the new file */
    app_window_update_title(w);

    load_context_free(ctx);

    if (warning != NULL) {
        char *message = g_strconcat(warning,
                                    "\n\nadifedit won't save over this file (marked parse_warn "
                                    "in the title) - Save will ask for a new file name.",
                                    NULL);
        ui_show_error(GTK_WINDOW(w), message);
        g_free(message);
        g_free(warning);
    }
    return G_SOURCE_REMOVE;
}

static void on_load_done(G_GNUC_UNUSED GObject *source, GAsyncResult *res, gpointer user_data) {
    LoadContext *ctx = user_data;
    GError *error = NULL;

    AdifParseResult *result = adif_io_load_finish(res, &ctx->etag, &error);

    if (load_abandoned(ctx)) {
        if (result != NULL) {
            adif_parse_result_free(result);
        }
        g_clear_error(&error);
        load_context_free(ctx);
        return;
    }

    AppWindow *w = ctx->w;
    if (result == NULL) {
        app_window_set_loading(w, FALSE, NULL);
        ui_show_error(GTK_WINDOW(w),
                      error != NULL ? error->message : "Unknown error while loading the file.");
        g_clear_error(&error);
        load_context_free(ctx);
        return;
    }

    /* The background thread only read and parsed the file; building the
     * document's rows from the result is synchronous and, for a big file,
     * the more expensive half - spread it over turns too, same as the
     * parse's spinner coverage. */
    ctx->result = result;
    ctx->step = adif_document_load_parse_begin(w->doc, result);
    g_idle_add(load_step_idle, ctx);
}

void app_window_start_loading(AppWindow *w, const char *path) {
    app_window_set_loading(w, TRUE, "Loading…");

    LoadContext *ctx = g_new0(LoadContext, 1);
    ctx->w = g_object_ref(w);
    ctx->path = g_strdup(path);
    adif_io_load_async(path, on_load_done, ctx);
}

static void on_open_response(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    AppWindow *w = user_data;
    GError *error = NULL;

    GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);
    if (file == NULL) {
        /* GTK_DIALOG_ERROR_DISMISSED just means the user hit Cancel. */
        if (error != NULL
            && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
            ui_show_error(GTK_WINDOW(w), error->message);
        }
        g_clear_error(&error);
        return;
    }

    app_window_open_file(w, file);
    g_object_unref(file);
}

void app_window_open_file(AppWindow *w, GFile *file) {
    char *path = g_file_get_path(file);
    if (path == NULL) {
        /* No local path: a phone over MTP, some network mounts. */
        char *name = g_file_get_parse_name(file);
        char *msg = g_strdup_printf("adifedit can only open files on a local disk, and %s "
                                    "isn't one. Copy it to a local folder first.",
                                    name);
        ui_show_error(GTK_WINDOW(w), msg);
        g_free(msg);
        g_free(name);
        return;
    }
    app_window_start_loading(w, path);
    g_free(path);
}

/* Show the file chooser and load the picked file into this window. The
 * caller is responsible for the unsaved-changes prompt (action_open). */
static void present_open_dialog(AppWindow *w) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open ADIF File");

    GtkFileFilter *adif_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(adif_filter, "ADIF files (*.adi, *.adif)");
    gtk_file_filter_add_pattern(adif_filter, "*.adi");
    gtk_file_filter_add_pattern(adif_filter, "*.adif");
    gtk_file_filter_add_pattern(adif_filter, "*.ADI");
    gtk_file_filter_add_pattern(adif_filter, "*.ADIF");

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All files");
    gtk_file_filter_add_pattern(all_filter, "*");

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, adif_filter);
    g_list_store_append(filters, all_filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, adif_filter);
    g_object_unref(adif_filter);
    g_object_unref(all_filter);
    g_object_unref(filters);

    gtk_file_dialog_open(dialog, GTK_WINDOW(w), NULL, on_open_response, w);
    g_object_unref(dialog);
}

/* ------------------------------------------------------------------- */
/* Saving                                                               */
/* ------------------------------------------------------------------- */

/* Put the open file's path on the clipboard. */
static void copy_file_path_to_clipboard(AppWindow *w) {
    if (w->current_filepath != NULL) {
        gdk_clipboard_set(gtk_widget_get_clipboard(GTK_WIDGET(w)), G_TYPE_STRING,
                          w->current_filepath);
    }
}

/* Drop the open file - back to the empty window. The caller has already
 * handled any unsaved changes. */
static void close_current_file(AppWindow *w) {
    adif_document_reset(w->doc);
    g_clear_pointer(&w->current_filepath, g_free);
    g_clear_pointer(&w->parse_warn_path, g_free);
    g_clear_pointer(&w->file_etag, g_free);
    gtk_label_set_text(GTK_LABEL(w->status_count), ""); /* not "0 QSOs" */
    app_window_update_title(w);
    app_window_refresh_actions(w); /* also shows the empty-state watermark */
}

/* One save in flight. */
typedef struct {
    AppWindow *w; /* ref held for the whole save: the user can close the window
                     (close guard -> "Discard") while the write is still running */
    char *path;   /* the target: becomes current_filepath only once written */
} SaveContext;

static void save_context_free(SaveContext *ctx) {
    g_object_unref(ctx->w);
    g_free(ctx->path);
    g_free(ctx);
}

static void start_saving(AppWindow *w, const char *path, const char *etag);

/* The changed-on-disk alert's answer. `user_data` is the SaveContext of
 * the refused save; w->pending still waits on whichever save follows. */
static void on_changed_on_disk_choice(GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveContext *ctx = user_data;
    AppWindow *w = ctx->w;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, NULL);
    if (w->doc != NULL) { /* the window may have closed meanwhile */
        switch (choice) {
            case 1: /* Save As */
                run_save_as(w);
                break;
            case 2: /* Overwrite: the same save, without the check */
                start_saving(w, ctx->path, NULL);
                break;
            default: /* Cancel, or dismissed */
                w->pending = APP_WINDOW_PENDING_NONE;
                break;
        }
    }
    save_context_free(ctx);
}

/* The save was refused because the file changed after adifedit read it -
 * typically a logger appending QSOs. Ask rather than lose them. Takes
 * ownership of `ctx`. */
static void ask_changed_on_disk(SaveContext *ctx) {
    char *name = g_path_get_basename(ctx->path);
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s has changed on disk since adifedit opened it",
                                                  name);
    gtk_alert_dialog_set_detail(dialog, "Another program (a logger, perhaps) has written to it. "
                                        "Overwriting replaces those changes with this log; Save As "
                                        "keeps both.");
    gtk_alert_dialog_set_buttons(dialog,
                                 (const char *[]){ "_Cancel", "Save _As…", "_Overwrite", NULL });
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 1);
    gtk_alert_dialog_choose(dialog, GTK_WINDOW(ctx->w), NULL, on_changed_on_disk_choice, ctx);
    g_object_unref(dialog);
    g_free(name);
}

/* The write landed (or failed): adopt the target as the open file, or
 * report why not. */
static void on_save_done(G_GNUC_UNUSED GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveContext *ctx = user_data;
    AppWindow *w = ctx->w;
    GError *error = NULL;
    char *new_etag = NULL;

    gboolean ok = adif_io_save_finish(res, &new_etag, &error);

    if (w->doc == NULL) { /* the window closed mid-save - see load_abandoned() */
        g_clear_error(&error);
        g_free(new_etag);
        save_context_free(ctx);
        return;
    }

    app_window_set_loading(w, FALSE, NULL);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG)) {
        g_clear_error(&error);
        ask_changed_on_disk(ctx);
        return;
    }
    if (!ok) {
        w->pending = APP_WINDOW_PENDING_NONE;
        ui_show_error(GTK_WINDOW(w),
                      error != NULL ? error->message : "Unknown error while saving the file.");
        g_clear_error(&error);
    } else {
        /* Subsequent plain Saves target this file (a Save As's new name). */
        if (g_strcmp0(w->current_filepath, ctx->path) != 0) {
            g_free(w->current_filepath);
            w->current_filepath = g_steal_pointer(&ctx->path);
        }
        g_free(w->file_etag);
        w->file_etag = new_etag;
        adif_document_set_modified(w->doc, FALSE);   /* refreshes the title via the signal */
        app_window_update_title(w);
        run_pending(w);   /* whatever the unsaved-changes guard was waiting on */
    }
    save_context_free(ctx);
}

/* Write the document to `path`. A non-NULL `etag` makes it refuse (and
 * ask) if the file there has changed since adifedit last read or wrote
 * it. */
static void start_saving(AppWindow *w, const char *path, const char *etag) {
    adif_document_commit_cell_edit(w->doc);   /* fold any in-progress cell edit in */
    app_window_set_loading(w, TRUE, "Saving…");
    SaveContext *ctx = g_new0(SaveContext, 1);
    ctx->w = g_object_ref(w);
    ctx->path = g_strdup(path);
    adif_io_save_async(path, adif_document_to_adif(w->doc, ADIFEDIT_VERSION, adifedit_git_hash()),
                       etag, on_save_done, ctx);
}

/* Save to the open file - unless it's the parse_warn file, which adifedit
 * never writes to: then ask for a new name instead. */
static void save_current(AppWindow *w) {
    if (app_window_is_parse_warn_file(w, w->current_filepath)) {
        run_save_as(w);
    } else {
        start_saving(w, w->current_filepath, w->file_etag);
    }
}

/* ------------------------------------------------------------------- */
/* Unsaved-changes guard (Open, Close File, Copy File Path, close)      */
/* ------------------------------------------------------------------- */

/* Shared "detail" line for the three prompts that discard the document. */
#define GUARD_DETAIL "This document has unsaved changes. They will be lost if you don't save."

/* Carry out whatever the guard deferred - either on "Discard" or once the
 * save the user asked for has landed (from on_save_done). */
static void run_pending(AppWindow *w) {
    AppWindowPending pending = w->pending;
    w->pending = APP_WINDOW_PENDING_NONE;
    switch (pending) {
        case APP_WINDOW_PENDING_OPEN:
            present_open_dialog(w);
            break;
        case APP_WINDOW_PENDING_CLOSE_FILE:
            close_current_file(w);
            break;
        case APP_WINDOW_PENDING_COPY_PATH:
            copy_file_path_to_clipboard(w);
            break;
        case APP_WINDOW_PENDING_CLOSE:
            w->force_close = TRUE;
            gtk_window_close(GTK_WINDOW(w));
            break;
        case APP_WINDOW_PENDING_NONE:
            break;
    }
}

static void on_guard_choice(GObject *source, GAsyncResult *res, gpointer user_data) {
    AppWindow *w = user_data;
    GError *error = NULL;

    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, &error);
    if (error != NULL) {
        g_clear_error(&error);   /* dismissed - treat as Cancel */
        w->pending = APP_WINDOW_PENDING_NONE;
        return;
    }
    switch (choice) {
        case 1: /* Discard - do the deferred thing now */
            run_pending(w);
            break;
        case 2: /* Save first; on_save_done runs the deferred thing */
            if (w->current_filepath != NULL) {
                save_current(w);
            } else {
                run_save_as(w);
            }
            break;
        default: /* 0 == Cancel */
            w->pending = APP_WINDOW_PENDING_NONE;
            break;
    }
}

/* If the document has unsaved changes, ask; otherwise (or on "Discard")
 * carry out `pending` straight away. "Save" defers it past the save.
 * `discard_label` / `save_label` are the two non-Cancel buttons. */
static void guard_unsaved(AppWindow *w, const char *question, const char *detail,
                          const char *discard_label, const char *save_label,
                          AppWindowPending pending) {
    w->pending = pending;
    if (!adif_document_get_modified(w->doc)) {
        run_pending(w);
        return;
    }

    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", question);
    gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_set_buttons(dialog,
                                 (const char *[]){ "_Cancel", discard_label, save_label, NULL });
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 2);
    gtk_alert_dialog_choose(dialog, GTK_WINDOW(w), NULL, on_guard_choice, w);
    g_object_unref(dialog);
}

/* Opening a file replaces the current document in place, so warn first if
 * there are unsaved changes. */
static void action_open(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                        gpointer user_data) {
    guard_unsaved(user_data, "Save changes before opening another file?", GUARD_DETAIL, "_Discard",
                  "_Save", APP_WINDOW_PENDING_OPEN);
}

/* Close File also discards the document, so it gets the same guard. */
static void action_close_file(G_GNUC_UNUSED GSimpleAction *action,
                              G_GNUC_UNUSED GVariant *parameter, gpointer user_data) {
    guard_unsaved(user_data, "Save changes before closing the file?", GUARD_DETAIL, "_Discard",
                  "_Save", APP_WINDOW_PENDING_CLOSE_FILE);
}

/* Save to the open file (or Save As, for the parse_warn file). Shared by
 * the File menu, the toolbar button and Ctrl+S. Disabled (via
 * refresh_actions) until a file is open, so current_filepath is always
 * set here. */
static void action_save(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                        gpointer user_data) {
    AppWindow *w = user_data;
    if (w->current_filepath != NULL) {
        save_current(w);
    }
}

/* Put the open file's path on the clipboard. Disabled until a file is
 * open, so current_filepath is set here. If the document has unsaved
 * changes, offer to save first so the file at that path is current. */
static void action_copy_file_path(G_GNUC_UNUSED GSimpleAction *action,
                                  G_GNUC_UNUSED GVariant *parameter, gpointer user_data) {
    AppWindow *w = user_data;
    if (w->current_filepath == NULL) {
        return;
    }
    guard_unsaved(w, "Save before copying the path?",
                  "This file has unsaved changes that won't be at that path until you save.",
                  "Copy _Anyway", "_Save & Copy", APP_WINDOW_PENDING_COPY_PATH);
}

/* The "won't save over the parse_warn file" alert was dismissed - offer
 * Save As again. */
static void on_parse_warn_refused(GObject *source, GAsyncResult *res, gpointer user_data) {
    gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, NULL);
    run_save_as(user_data);
}

static void on_save_as_response(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    AppWindow *w = user_data;
    GError *error = NULL;

    GFile *file = gtk_file_dialog_save_finish(dialog, res, &error);
    if (file == NULL) {
        w->pending = APP_WINDOW_PENDING_NONE;   /* a guard-triggered Save As was cancelled */
        if (error != NULL
            && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
            ui_show_error(GTK_WINDOW(w), error->message);
        }
        g_clear_error(&error);
        return;
    }

    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (path == NULL) {
        w->pending = APP_WINDOW_PENDING_NONE;
        return;
    }

    /* Never over the parse_warn file - say why and ask again (w->pending
     * still waits on this save). */
    if (app_window_is_parse_warn_file(w, path)) {
        char *name = g_path_get_basename(path);
        char *msg = g_strdup_printf("adifedit won't save over %s: it loaded with a parse "
                                    "warning. Choose a different name.",
                                    name);
        GtkAlertDialog *alert = gtk_alert_dialog_new("%s", msg);
        gtk_alert_dialog_choose(alert, GTK_WINDOW(w), NULL, on_parse_warn_refused, w);
        g_object_unref(alert);
        g_free(msg);
        g_free(name);
        g_free(path);
        return;
    }

    /* Saving As over the open file itself still gets the changed-on-disk
     * check; any other existing file, the file chooser has already
     * confirmed replacing. */
    gboolean same = w->current_filepath != NULL && adif_io_same_file(path, w->current_filepath);
    start_saving(w, path, same ? w->file_etag : NULL);
    g_free(path);
}

static void run_save_as(AppWindow *w) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save ADIF File As");

    if (w->current_filepath != NULL) {
        /* For the parse_warn file, suggest a copy beside it. */
        char *basename = g_path_get_basename(w->current_filepath);
        if (app_window_is_parse_warn_file(w, w->current_filepath)) {
            char *copy = adif_io_copy_name(basename);
            g_free(basename);
            basename = copy;
        }
        gtk_file_dialog_set_initial_name(dialog, basename);
        g_free(basename);

        GFile *current = g_file_new_for_path(w->current_filepath);
        GFile *parent = g_file_get_parent(current);
        if (parent != NULL) {
            gtk_file_dialog_set_initial_folder(dialog, parent);
            g_object_unref(parent);
        }
        g_object_unref(current);
    }

    gtk_file_dialog_save(dialog, GTK_WINDOW(w), NULL, on_save_as_response, w);
    g_object_unref(dialog);
}

static void action_save_as(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                           gpointer user_data) {
    run_save_as(user_data);
}

/* ------------------------------------------------------------------- */
/* Unsaved-changes guard on window close                                */
/* ------------------------------------------------------------------- */

gboolean on_window_close_request(G_GNUC_UNUSED GtkWindow *window, gpointer user_data) {
    AppWindow *w = user_data;
    if (w->force_close || !adif_document_get_modified(w->doc)) {
        return GDK_EVENT_PROPAGATE;   /* let the close proceed */
    }

    guard_unsaved(w, "Save changes before closing?", GUARD_DETAIL, "_Discard", "_Save",
                  APP_WINDOW_PENDING_CLOSE);
    return GDK_EVENT_STOP;   /* hold the window open until the choice comes back */
}

/* ------------------------------------------------------------------- */
/* Context-menu actions                                                 */
/* ------------------------------------------------------------------- */

typedef struct {
    AppWindow *w;
    guint arg;   /* fill: column data-index. insert: target visual position. */
} ColumnActionCtx;

/* Turn a name-check result into a sentence for the user, or NULL if OK.
 * Caller frees. */
static char *column_name_problem(AdifColumnNameStatus status, const char *name) {
    switch (status) {
        case ADIF_COLUMN_NAME_OK:
            return NULL;
        case ADIF_COLUMN_NAME_EMPTY:
            return g_strdup("Enter an ADIF field name for the column.");
        case ADIF_COLUMN_NAME_INVALID:
            return g_strdup_printf(
                "\"%s\" is not a valid ADIF field name.\n\n"
                "Use only letters, digits and underscores, starting with a letter or underscore.",
                name);
        case ADIF_COLUMN_NAME_DUPLICATE:
            return g_strdup_printf("A column named \"%s\" already exists.", name);
        default:
            return g_strdup("That column name cannot be used.");
    }
}

static char *on_insert_column_name(const char *text, gpointer user_data) {
    ColumnActionCtx *ctx = user_data;

    char *name = g_strstrip(g_strdup(text));
    char *problem = column_name_problem(adif_document_check_new_column_name(ctx->w->doc, name),
                                        name);
    if (problem == NULL) {
        adif_document_insert_column(ctx->w->doc, ctx->arg, name);
    }
    g_free(name);
    return problem;
}

/* Prompt for a field name and insert a column at visual `position`. */
static void prompt_insert_column(AppWindow *w, guint position) {
    ColumnActionCtx *ctx = g_new0(ColumnActionCtx, 1);
    ctx->w = w;
    ctx->arg = position;

    ui_text_prompt_with_suggestions(
        GTK_WINDOW(w), "Insert Column", "ADIF field name for the new column:", "e.g. MY_NOTE",
        "_Insert", ADIF_KNOWN_FIELDS, on_insert_column_name, ctx, g_free);
}

static void action_insert_column_left(G_GNUC_UNUSED GSimpleAction *action, GVariant *parameter,
                                      gpointer user_data) {
    AppWindow *w = user_data;
    prompt_insert_column(w, adif_document_column_position(w->doc, g_variant_get_uint32(parameter)));
}

static void action_insert_column_right(G_GNUC_UNUSED GSimpleAction *action, GVariant *parameter,
                                       gpointer user_data) {
    AppWindow *w = user_data;
    prompt_insert_column(w, adif_document_column_position(w->doc, g_variant_get_uint32(parameter))
                                + 1);
}

/* A Fill Column in progress, written a slice per idle turn so a big log
 * doesn't freeze the window. The window is busy (app_window_set_loading)
 * meanwhile, so nothing else touches the document until finish(). */
typedef struct {
    AppWindow *w;           /* ref held for the whole fill */
    AdifColumnWrite *write; /* until finish()/cancel() */
} FillContext;

static void fill_context_free(FillContext *ctx) {
    adif_document_column_write_cancel(ctx->write); /* no-op once finished */
    g_object_unref(ctx->w);
    g_free(ctx);
}

/* Write one slice; once every row is written, record the undo step. */
static gboolean fill_step(FillContext *ctx) {
    if (adif_document_column_write_step(ctx->write, adif_slice_deadline())) {
        return TRUE; /* rows remain */
    }
    adif_document_column_write_finish(ctx->write);
    ctx->write = NULL;
    return FALSE;
}

static gboolean fill_step_idle(gpointer user_data) {
    FillContext *ctx = user_data;
    AppWindow *w = ctx->w;
    if (w->doc == NULL) { /* the window closed mid-fill - see load_abandoned() */
        fill_context_free(ctx);
        return G_SOURCE_REMOVE;
    }
    if (fill_step(ctx)) {
        return G_SOURCE_CONTINUE;
    }
    app_window_set_loading(w, FALSE, NULL);
    fill_context_free(ctx);
    return G_SOURCE_REMOVE;
}

static char *on_fill_column_text(const char *text, gpointer user_data) {
    ColumnActionCtx *prompt = user_data;
    AdifColumnWrite *write = adif_document_column_write_begin(prompt->w->doc, prompt->arg, text,
                                                              FALSE);
    if (write == NULL) {
        return NULL;
    }
    FillContext *ctx = g_new0(FillContext, 1);
    ctx->w = g_object_ref(prompt->w);
    ctx->write = write;

    /* A column that fits in one slice is done now, with no busy flicker;
     * only a big one shows the spinner and carries on from an idle. */
    if (!fill_step(ctx)) {
        fill_context_free(ctx);
        return NULL;
    }
    app_window_set_loading(ctx->w, TRUE, "Filling…");
    g_idle_add(fill_step_idle, ctx);
    return NULL;
}

static void action_fill_column(G_GNUC_UNUSED GSimpleAction *action, GVariant *parameter,
                               gpointer user_data) {
    AppWindow *w = user_data;

    guint data_index = g_variant_get_uint32(parameter);
    const char *name = adif_document_column_name(w->doc, data_index);
    if (name == NULL) {
        return;
    }

    ColumnActionCtx *ctx = g_new0(ColumnActionCtx, 1);
    ctx->w = w;
    ctx->arg = data_index;

    char *prompt = g_strdup_printf("Fill every row of \"%s\" with:", name);
    ui_text_prompt(GTK_WINDOW(w), "Fill Column", prompt, NULL, "_Fill", on_fill_column_text, ctx,
                   g_free);
    g_free(prompt);
}

static void action_delete_column(G_GNUC_UNUSED GSimpleAction *action, GVariant *parameter,
                                 gpointer user_data) {
    AppWindow *w = user_data;

    if (!adif_document_delete_column(w->doc, g_variant_get_uint32(parameter))) {
        ui_show_error(GTK_WINDOW(w), "The table must keep at least one column.");
    }
}

static void action_insert_row(G_GNUC_UNUSED GSimpleAction *action, GVariant *parameter,
                              gpointer user_data) {
    AppWindow *w = user_data;
    adif_document_insert_row(w->doc, g_variant_get_uint32(parameter));
}

static void action_delete_row(G_GNUC_UNUSED GSimpleAction *action, GVariant *parameter,
                              gpointer user_data) {
    AppWindow *w = user_data;
    adif_document_delete_row(w->doc, g_variant_get_uint32(parameter));
}

static void action_exit(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                        gpointer user_data) {
    AppWindow *w = user_data;
    gtk_window_close(GTK_WINDOW(w));   /* runs the unsaved-changes guard */
}

static void action_undo(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                        gpointer user_data) {
    adif_document_undo(((AppWindow *)user_data)->doc);
}

static void action_redo(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                        gpointer user_data) {
    adif_document_redo(((AppWindow *)user_data)->doc);
}

static void action_about(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                         gpointer user_data) {
    AppWindow *w = user_data;

    static const char *const authors[] = { "Nicholas Heath", NULL };

    char *comments = g_strdup_printf("A GTK4 viewer and editor for ham radio ADIF logs.\n\n"
                                     "Hash: %s\n"
                                     "Built: %s\n\n"
                                     "Built with GTK %u.%u.%u, released under the GNU LGPL.",
                                     adifedit_git_hash(), adifedit_build_time(),
                                     gtk_get_major_version(), gtk_get_minor_version(),
                                     gtk_get_micro_version());

    GtkWidget *dialog = g_object_new(
        GTK_TYPE_ABOUT_DIALOG, "program-name", "adifedit", "version", ADIFEDIT_VERSION, "comments",
        comments, "copyright", "Copyright \xC2\xA9 2026 Nicholas Heath", "license-type",
        GTK_LICENSE_GPL_3_0, /* GPL v3+; fills in the warranty notice */
        "authors", authors, "logo-icon-name", "io.github.nheathlabs.adifedit", "transient-for",
        GTK_WINDOW(w), "modal", TRUE, NULL);
    g_free(comments);
    gtk_window_present(GTK_WINDOW(dialog));
}

/* Deferred to an idle for the same reason as open_stamps_dir_idle: let
 * the Help menu close before the launcher can map a chooser popup. */
static gboolean open_adif_spec_idle(gpointer user_data) {
    AppWindow *w = user_data;
    GtkUriLauncher *launcher = gtk_uri_launcher_new(ADIF_SPEC_URL);
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(w), NULL, NULL, NULL);
    g_object_unref(launcher);
    return G_SOURCE_REMOVE;
}

/* Open the ADIF specification this build targets in the default browser. */
static void action_adif_spec(G_GNUC_UNUSED GSimpleAction *action, G_GNUC_UNUSED GVariant *parameter,
                             gpointer user_data) {
    g_idle_add(open_adif_spec_idle, user_data);
}

/* ------------------------------------------------------------------- */
/* Toolbar: insert a row / column relative to the cursor cell           */
/* ------------------------------------------------------------------- */

static void action_cursor_insert_row_above(G_GNUC_UNUSED GSimpleAction *action,
                                           G_GNUC_UNUSED GVariant *parameter, gpointer user_data) {
    AppWindow *w = user_data;
    guint row = adif_table_selected_row(w->table);
    adif_document_insert_row(w->doc, row == GTK_INVALID_LIST_POSITION ? 0 : row);
}

static void action_cursor_insert_row_below(G_GNUC_UNUSED GSimpleAction *action,
                                           G_GNUC_UNUSED GVariant *parameter, gpointer user_data) {
    AppWindow *w = user_data;
    guint row = adif_table_selected_row(w->table);
    adif_document_insert_row(w->doc, row == GTK_INVALID_LIST_POSITION ? G_MAXUINT : row + 1);
}

static void action_cursor_insert_column_left(G_GNUC_UNUSED GSimpleAction *action,
                                             G_GNUC_UNUSED GVariant *parameter,
                                             gpointer user_data) {
    AppWindow *w = user_data;
    guint col = adif_table_current_column(w->table);
    if (col != GTK_INVALID_LIST_POSITION) {
        prompt_insert_column(w, col);
    }
}

static void action_cursor_insert_column_right(G_GNUC_UNUSED GSimpleAction *action,
                                              G_GNUC_UNUSED GVariant *parameter,
                                              gpointer user_data) {
    AppWindow *w = user_data;
    guint col = adif_table_current_column(w->table);
    if (col != GTK_INVALID_LIST_POSITION) {
        prompt_insert_column(w, col + 1);
    }
}

/* ------------------------------------------------------------------- */
/* Action table (GAction + accelerator + "needs a file open")           */
/* ------------------------------------------------------------------- */

/* Each row: the GActionEntry (name, handler, and "u"/"s" parameter type
 * for the actions that carry a target), then the keyboard accelerator(s)
 * and whether the action is greyed out until a file is open. undo / redo
 * are history-driven (on_doc_history_changed), not file-driven, so their
 * needs_file stays FALSE. */
#define ENTRY(n, fn) .entry = { .name = (n), .activate = (fn) }
#define ENTRY_T(n, fn, t) .entry = { .name = (n), .activate = (fn), .parameter_type = (t) }

const WinAction WIN_ACTIONS[] = {
    { ENTRY("open", action_open) },
    { ENTRY("save", action_save), .accel = "<Control>s", .needs_file = TRUE },
    { ENTRY("save_as", action_save_as), .needs_file = TRUE },
    { ENTRY("close_file", action_close_file), .accel = "<Control>w", .needs_file = TRUE },
    { ENTRY("copy_file_path", action_copy_file_path), .needs_file = TRUE },
    { ENTRY("exit", action_exit) },
    { ENTRY("about", action_about) },
    { ENTRY("adif_spec", action_adif_spec) },
    { ENTRY("find", win_action_find), .accel = "<Control>f", .needs_file = TRUE },
    { ENTRY("find_replace", win_action_find_replace), .accel = "<Control>h", .needs_file = TRUE },
    { ENTRY("find_next", win_action_find_next), .accel = "<Control>g", .needs_file = TRUE },
    { ENTRY("find_prev", win_action_find_prev), .accel = "<Control><Shift>g", .needs_file = TRUE },
    { ENTRY("replace", win_action_replace), .needs_file = TRUE },
    { ENTRY("replace_all", win_action_replace_all), .needs_file = TRUE },
    { ENTRY("undo", action_undo), .accel = "<Control>z" },
    { ENTRY("redo", action_redo), .accel = "<Control>y", .accel2 = "<Control><Shift>z" },
    { ENTRY("cursor_insert_row_above", action_cursor_insert_row_above), .needs_file = TRUE },
    { ENTRY("cursor_insert_row_below", action_cursor_insert_row_below), .needs_file = TRUE },
    { ENTRY("cursor_insert_column_left", action_cursor_insert_column_left), .needs_file = TRUE },
    { ENTRY("cursor_insert_column_right", action_cursor_insert_column_right), .needs_file = TRUE },
    { ENTRY_T("insert_column_left", action_insert_column_left, "u"), .needs_file = TRUE },
    { ENTRY_T("insert_column_right", action_insert_column_right, "u"), .needs_file = TRUE },
    { ENTRY_T("fill_column", action_fill_column, "u"), .needs_file = TRUE },
    { ENTRY_T("delete_column", action_delete_column, "u"), .needs_file = TRUE },
    { ENTRY_T("insert_row", action_insert_row, "u"), .needs_file = TRUE },
    { ENTRY_T("delete_row", action_delete_row, "u"), .needs_file = TRUE },
    { ENTRY_T("run_stamp", win_action_run_stamp, "s"), .needs_file = TRUE },
    { ENTRY("stamps_open_fm", win_action_stamps_open_fm) },
    { ENTRY("stamps_copy_path", win_action_stamps_copy_path) },
};

#undef ENTRY
#undef ENTRY_T

const gsize WIN_ACTIONS_LEN = G_N_ELEMENTS(WIN_ACTIONS);

void app_window_add_actions(GActionMap *map, gpointer user_data) {
    GActionEntry entries[G_N_ELEMENTS(WIN_ACTIONS)];
    for (gsize i = 0; i < G_N_ELEMENTS(WIN_ACTIONS); i++) {
        entries[i] = WIN_ACTIONS[i].entry;
    }
    g_action_map_add_action_entries(map, entries, G_N_ELEMENTS(entries), user_data);
}

void app_window_install_accels(GtkApplication *app) {
    for (gsize i = 0; i < WIN_ACTIONS_LEN; i++) {
        const WinAction *a = &WIN_ACTIONS[i];
        if (a->accel == NULL) {
            continue;
        }
        char *detailed = g_strconcat("win.", a->entry.name, NULL);
        const char *accels[] = { a->accel, a->accel2, NULL };
        gtk_application_set_accels_for_action(app, detailed, accels);
        g_free(detailed);
    }
}
