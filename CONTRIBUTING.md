<!--
adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
Copyright (C) 2026 Nicholas Heath
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Contributing to adifedit

## Scope

adifedit has a deliberate, narrow purpose — see **Purpose and scope** in
the [README](README.md). Before proposing a feature, weigh it against
these; open an issue to discuss anything that isn't a clear fit.

1. **Is it about the open file, or the world outside it?** Editing the
   ADIF in front of you is in scope. Reaching a radio, a website, LoTW /
   eQSL / QRZ, or the DX cluster is not.
2. **Does it move ADIF data, or judge it?** Reshaping rows, columns and
   values — the things you would otherwise do in a spreadsheet or with
   `sed` — is in scope.
3. **Does the model stay *load a file → edit → save*?** adifedit owns no
   hidden store that it manages, migrates, and syncs behind your back —
   that is what keeps it a tool and not a database front-end. A feature
   that has to build and keep its own dataset is a different app.
4. **Does the output stay plain, portable ADIF?** The `APP_ADIFEDIT_*`
   header fields are the ceiling — additive, ignored by other tools,
   documented. Nothing that makes the saved file useful only to adifedit.
5. **One feature per pull request** — a guideline, not a hard rule (some
   changes can't be split cleanly). If the description needs an "and
   also", it is probably two features; split them and weigh each against
   1–4.

Two features already sit near this line and passed:

- **Batch stamps** — a `.bst` is a plain-text template you own and drop
  in a directory; the app only reads it. Not app-managed state (#3).
- **The `APP_ADIFEDIT_*` header fields** — every other tool ignores them
  and the file still round-trips (#4).

## Commits

Keep one logical change per commit, and keep mechanical churn out of
behavioural diffs — e.g. the `clang-format`-the-tree pass is its own
commit, separate from any feature work.

## Disclosing AI assistance

If any code, tests, or docs in a change were generated or materially
drafted by an AI tool, say so — both places:

- **In the commit message**, a trailer naming the tool:

  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```

  One line per tool if more than one was used. Use the model / product
  name, not just "AI".

- **In the pull request description**, a one-line note that the change
  was AI-assisted.

You are still the author: review every line, run the pre-push checks, and
don't submit code you don't understand. The trailer is about
transparency, not a disclaimer of responsibility. Purely human changes
need neither note.

## Before you push

CI (`.github/workflows/ci.yml`) runs all of these, and every CI job
must pass before a pull request can merge into `master`. CI doesn't run
on its own: the maintainer starts it on a pull request's branch. There
is no pre-commit hook either, so run them yourself first:

```sh
cmake --preset debug && cmake --build --preset debug
ctest --test-dir build/debug --output-on-failure
clang-format --dry-run --Werror src/*.c src/*.h tests/*.c scripts/screenshot_helper.c
clang-tidy -p build/debug src/*.c tests/*.c
shellcheck scripts/*.sh
./scripts/check-portability.sh
./scripts/check-dead-code.sh build/debug
```

If the change touches the parser, the model or the file I/O, also run
the tests under AddressSanitizer + UBSan, and fuzz the parser for a few
minutes (see "Fuzzing" below):

```sh
cmake --preset asan && cmake --build --preset asan
ctest --preset asan
```

If the change touches the build, adds a dependency, or makes GTK load
something new by path (a stock icon, a schema, an image format), also
build the Windows zip: `scripts/windows-podman.sh` (see "Portability").

After a change that alters the UI, regenerate the README screenshots
(`./scripts/screenshots.sh`, needs `cage` + `grim`) and commit the PNGs
in the same change. The script builds its `screenshot_helper` target
itself; it defaults to `./build` but takes any configured tree via
`BUILD_DIR=build/debug ./scripts/screenshots.sh`.

`-Wall -Wextra -Wshadow -pedantic-errors` is always on for our code
(`-std=c11`, no extensions), and dead code is a build error (see "Dead
code" below); keep the build warning-free, the tests green,
and `clang-format` / `clang-tidy` / `shellcheck` clean — there is no
outstanding `clang-tidy` finding to work around. Suppress a `shellcheck`
false positive inline with a `# shellcheck disable=SCxxxx` line and a
reason.

gcc and clang diagnose different things. To build with the other one,
set `CC` on the configure step and give it its own build dir:

```sh
CC=clang cmake -S . -B build/clang -DCMAKE_BUILD_TYPE=Debug && cmake --build build/clang
```

## What the tooling covers

- **`.clang-format`** is the authority on layout. Run `clang-format -i`
  on files you touch. Don't hand-fight it; if it mangles something,
  restructure the *expression* (see `pending_differs` for the pattern).
  `.clang-format-ignore` exempts `src/portability_guard.h` (pragma-heavy,
  deliberate include order).
- **`.clang-tidy`** covers naming (`functions_like_this`, `TypesLikeThis`,
  `MACROS_LIKE_THIS`), bug-prone patterns, the Clang Static Analyzer,
  CERT C rules, thread-unsafe libc calls (`concurrency-*`, since load and
  save run on worker threads) and `performance-*`. Tuned for GLib/GObject.
- **Function size** is capped by `.clang-tidy` too: cognitive complexity
  25 (`readability-function-cognitive-complexity`) and 80 lines
  (`readability-function-size`). Split a function that trips either into
  helpers that each do one step - the pattern is a small driver over
  named steps (`fit_column()` / `apply_column_widths()` in
  `adif_table.c`, `handle_tag()` / `parse_contents()` in
  `adif_parser.c`) - rather than raising the limit or adding a `NOLINT`.
  Statement counts are left at the default because GLib's `g_clear_*()`
  macros inflate them. Tests are exempt (`tests/.clang-tidy`): every
  `g_assert_*()` counts as branches, so a flat test scores high.
  To see where a file stands before it trips the gate, run the check on
  its own at a lower threshold:
  `clang-tidy -p build/debug -checks='-*,readability-function-cognitive-complexity' -config="{CheckOptions: {readability-function-cognitive-complexity.Threshold: 15}}" src/foo.c`
- **Dead code** fails at three levels:
  - *The compiler* (`ADIFEDIT_C_FLAGS` in `CMakeLists.txt`): an unused
    static function, variable, parameter, label, value or `.c`-file
    macro is an error. So is a non-static function with no header
    declaration (`-Wmissing-prototypes`), which makes a helper `static`
    and lets `-Wunused-function` see it when it goes dead. A clang build
    also errors on unreachable code. Only these warnings are errors,
    not a blanket `-Werror`. Mark a callback's ignored parameter
    `G_GNUC_UNUSED`.
  - *`clang-tidy`* treats dead stores, unused parameters and unreachable
    code as errors (`WarningsAsErrors`), even on a gcc build's compile
    database.
  - *`scripts/check-dead-code.sh [BUILD_DIR]`* finds what a per-file
    compiler can't: an exported function that nothing calls, or that
    only the tests call. It reads the build's objects with `nm`, then
    checks the source for any other use before reporting one. Delete
    what it reports. A function that is deliberately test-only API
    goes in `scripts/dead-code-allowlist.txt` with a comment saying
    why.
- **`.editorconfig`** covers whitespace basics in every editor.

Everything below is what the tools *can't* check.

## Conventions

### Every file starts with the licence header

The GPL-3.0-or-later block with an `SPDX-License-Identifier` line (see any
existing `.c`). The short `#`/`<!--`-comment form for non-C files.

### Annotate pointer ownership

C has no way to express who frees what, so say it in a comment wherever
it isn't obvious — on struct fields, function parameters, and return
values:

```c
GtkWidget *cell_menu;     /* right-click popover, parented to column_view */
AdifRow *row;             /* SET_VALUE: the edited row ... (ref) */
const char *adif_document_column_name(...);   /* borrowed, or NULL */
```

Use GObject's vocabulary: *borrowed*, *transfer full*, *floating*. When a
`g_object_ref` / `g_steal_pointer` / `g_object_unref` looks unbalanced at
a glance, a one-line comment saying why saves the next reader a hunt.

### Modules

- One `.c` file = one responsibility. When a file gets large, split it at
  a real seam and name the seam (see `adif_table.c` / `adif_table_view.c`,
  `window.c` / `window_actions.c`).
- An opaque struct spread across several `.c` files shares an internal
  `*_priv.h` that only those files include (`adif_table_priv.h`,
  `window_priv.h`). It is not a public header.
- Heap types get `foo_new()` / `foo_free()`; GObject types use
  `G_DECLARE_FINAL_TYPE` + `g_object_new`. `AppWindow` (`window_priv.h`) is
  the latter: a `GtkApplicationWindow` subclass, so `w` in `window*.c` IS
  the top-level window - cast it (`GTK_WINDOW(w)`, `GTK_WIDGET(w)`) rather
  than reaching for a `->window` field, and release document/table/stamp
  state in `app_window_dispose()` (`window.c`), not a separate free
  function.

### Model / view split

- **`AdifDocument`** is the model: schema + rows + undo/redo + the
  modified flag, and **no GTK**. It lives in the `adifcore` library and is
  unit-tested headlessly. New document behaviour goes here, with a test.
- **`AdifTable`** and the `window*` files are the view/controller. They
  own no data — they render `AdifDocument` and call its mutators. They
  build as the `adifeditui` object library; `adifedit` is just `main.c` over
  it, so the screenshot helper (and a future GUI test) can link the same
  objects the app ships.
- A new mutating operation must go through the `Op` / `record_op` path in
  `adif_document.c` (add an `OpType` and its apply/revert cases) so it
  participates in undo. A mutation that bypasses that is a bug.
- A multi-step action (e.g. a batch stamp) wraps its calls in
  `adif_document_begin_group()` / `end_group()` so it undoes as one step.
- Text matching in the model (search / replace) is **ASCII**
  case-insensitive by design (`g_ascii_*`), like the rest of the code
  that treats ADIF as ASCII-oriented — not a bug to "fix" with
  `g_utf8_casefold`.

### View performance

The grid is judged on slow hardware, where GTK's cost per widget, not the
data work, is what shows.

- `GtkColumnView` builds a cell widget for every visible row *in every
  column* (it doesn't virtualise columns), so a cell's cost is multiplied
  by the column count. Keep cells cheap: a cell is a focusable box
  holding a `GtkLabel`, and the one `GtkEntry` exists only while
  that cell is being edited (`begin_edit()` / `end_edit()` in
  `adif_table_view.c`). An entry per cell made a 33-row, 13-column log
  take ~100 ms to show here, against ~13 ms now.
- Don't rebuild columns you don't need to: `on_schema_changed()` skips
  the rebuild when the view's columns already match the schema (a load's
  second "schema-changed" only asks for a re-fit), and `sync_columns()`
  reconciles in place - an insert adds one column, a delete removes one.
  Tearing every column down also destroyed the focused cell, and GTK's
  refocus scrolled a wide table to its far end (`tests/test_table_scroll.c`).
- A cell can outlive its column (GTK still delivers the focus-leave of a
  focused cell whose column was just removed), so each cell holds a
  reference on its `AdifColumnInfo` rather than borrowing it.
- Never unparent a widget from inside a focus-change handler for it;
  GTK is still walking up from it. Defer the removal to an idle (see
  `on_cell_focus_leave()`).
- To find a stall, time it rather than guess: temporary
  `g_printerr()` timestamps around the suspect step plus a
  `gtk_widget_add_tick_callback()` frame logger, run headless in `cage`
  the way `scripts/shot.sh` does. Swapping the suspect widget for a
  plain one is a quick way to bisect cost inside GTK.
- `GtkInscription` looks like the cheap cell widget, but it draws
  ellipsized text a few pixels off from unclipped text in the same row;
  a single-line, end-ellipsized `GtkLabel` costs the same and lines up.

### Window actions

- A menu item or toolbar button is a `GAction` on the window
  (`win.<name>`, in `WIN_ACTIONS`); a toolbar button is `GtkActionable` on
  that action (`toolbar_button()`), never a `clicked` handler. That
  includes the insert-relative-to-cursor buttons
  (`win.cursor_insert_row_above`, …), whose actions read the live cursor
  from `AdifTable` when activated.
- A keyboard accelerator goes in the action's `WIN_ACTIONS` entry
  (`.accel`); `app_window_install_accels()` binds them all at startup.
- An action that only makes sense with a file open sets `.needs_file =
  TRUE` in its entry; `app_window_refresh_actions()` is the one place that
  computes enabled state (it also locks actions while a load or save is
  running). Open and undo / redo deliberately leave `.needs_file` unset.
- An action that could discard unsaved work (close, Open, …) prompts
  first with a `GtkAlertDialog`, then defers the real work behind a
  `*_after_save` flag that `on_save_done()` acts on once the save lands.
  Follow that pattern for any new "this loses data" action.

### Styling

The app ships one small stylesheet, `assets/style.css`, bundled in the
GResource and installed once at startup by `adifedit_app_startup()`
(`window.c`, at `GTK_STYLE_PROVIDER_PRIORITY_APPLICATION`). Put new rules
there; CSS class names are prefixed `adif-` (`adif-search-match`,
`adif-empty-watermark`, `adif-cell`, `adif-cell-editor`).

Every toolbar icon is a custom SVG in
`assets/icons/hicolor/scalable/actions/adifedit-*-symbolic.svg`, drawn in
one deliberate solid-fill style (not Adwaita's thin line-art). A new
toolbar button gets a new SVG in that style, an entry in
`assets/adifedit.gresource.xml`, and a `scripts/render-icons.sh` run to
refresh the tarball.

### GObject discipline

- `g_signal_connect_object` (not plain `g_signal_connect`) when the
  handler's validity is tied to another object's lifetime.
- Release everything in `finalize` with `g_clear_object` /
  `g_clear_pointer`.
- Signals are declared with the `SIGNAL_*` enum + `signals[N_SIGNALS]`
  pattern.

### Prefer GLib over the C library

`gboolean`/`TRUE`/`FALSE`, `g_strdup`, `g_ascii_*`, `GPtrArray`,
`g_new0`, `g_clear_pointer`, `g_steal_pointer` — not `bool`, `strdup`,
`malloc`, raw `free`. If a future change ever needs an atomic
(a counter or flag touched from more than one thread), that's
`g_atomic_int_*` / `g_atomic_pointer_*` / `GAtomicRefCount` — not C11
`<stdatomic.h>`. Same reasoning as everywhere else in this list: stay on
the GLib primitive so the Windows/MinGW goal in "Portability" holds
without a second code path. Nothing in the tree needs an atomic today
(the only concurrency, `adif_io.c`'s async load/save, hands data to a
`GTask` worker thread and back with no state shared between the two
sides at once) — this is a default for if/when that changes, not
something to retrofit now.

A callback parameter the handler doesn't use is marked `G_GNUC_UNUSED` in
the signature (`static void on_thing(G_GNUC_UNUSED GObject *src, …)`), not
cast to `void` in the body.

### C standard

The target is ISO **C11**, enforced: `CMakeLists.txt` sets
`CMAKE_C_STANDARD 11` with extensions off, and `ADIFEDIT_C_FLAGS` adds
`-pedantic-errors`, so `nullptr`, `constexpr`, `[[attributes]]`,
statement expressions, plain `typeof`, and the like are build failures,
not warnings. Raising the standard is a deliberate, separate change to
that one `set()`.

GTK/GLib headers are `-isystem`, so their own non-conformance is exempt —
but a non-conforming *use* of a GLib API from our code still fails (e.g.
passing a function pointer where GLib wants a `gpointer`). Fix the call,
don't widen the exemption.

### Portability (keep the Windows build working)

The code stays on GLib/GIO abstractions and off POSIX, so the MinGW build
(`scripts/windows-podman.sh`, see README "Windows build") needs no
platform code, only packaging. Three things enforce it, and all run in
the pre-push checks:

| Layer | Catches |
|---|---|
| `.clang-tidy` `portability-restrict-system-includes` | `#include <unistd.h>` / `<sys/*>` / `<pwd.h>` / `<pthread.h>` … — you can't reach `open`/`stat`/`fork`/pthreads without one |
| `src/portability_guard.h` (force-included, `#pragma GCC poison`) | direct calls to `fork`, `exec*`, `pthread_*`, `mmap`, `getpwuid` … — a **compile error**, both compilers |
| `scripts/check-portability.sh` | what the above miss: hardcoded `/tmp`-style paths, `system()` / `popen()` / `getenv()` / `realpath()`, `gdk_x11_*` / `<gdk/wayland/…>` backend APIs, `%ld` / `%lu` (`long` is 32-bit on 64-bit Windows) |

Use the GLib form instead:

| instead of | use |
|---|---|
| `getenv` / `setenv` | `g_getenv` / `g_setenv` |
| `"/tmp"`, `$HOME`, `~` | `g_get_tmp_dir()`, `g_get_home_dir()`, `g_get_user_*_dir()` |
| building a path with `"/"` | `g_build_filename()` |
| `pthread_*`, raw threads | `GThread` / `GTask` / `GMutex` / `GCond` |
| `fork` + `exec` / `system` / `popen` | `g_spawn_*` |
| `gettimeofday`, `clock_gettime` | `g_get_real_time()` / `g_get_monotonic_time()` |
| `stat`, `open`, `close`, `unlink` | `g_stat`, `g_open`, `g_close`, `g_remove` (`<glib/gstdio.h>`) |
| `%ld` for a `gsize` / pointer / `gint64` | `%zu`, `%p`, `G_GINT64_FORMAT` |

A change that touches the build, or adds a dependency, runtime data
file or stock icon, should still produce a working zip: run
`scripts/windows-podman.sh`. `scripts/build-windows.sh` copies
everything GTK loads by path (schemas, pixbuf loaders, the hicolor and
Adwaita icon themes) and fails if any bundled binary imports a DLL
missing from the bundle.

Deliberate platform code goes behind `#ifdef G_OS_WIN32` / `G_OS_UNIX`,
ideally in its own file. To silence a checked line that's genuinely fine,
add `/* portable-ok: <reason> */` (the grep) or `// NOLINT(...)` (tidy);
`portability_guard.h` has no escape hatch by design — restructure the
call.

### Tests

Model changes come with a test in `tests/test_document.c` (GLib test
framework, headless). Run `ctest`.

View behaviour that needs a real window (scrolling, focus) goes in
`tests/test_table_scroll.c`, which links `adifeditui`. CTest runs it via
`tests/run_headless.sh` in its own headless `cage` session, so it never
opens a window on your desktop and reports "skipped" if `cage` isn't
installed.

A refactor that should change nothing gets proof that it didn't:

- Model and parser code: the existing tests, plus - for a rewrite with
  many edge cases, like the tokeniser - build the old and new versions
  against a small dump program and diff their output over normal and
  malformed inputs.
- GTK code has no automated tests yet. Compare headless screenshots
  (`scripts/shot.sh`) from before and after the change; they should be
  pixel-identical.

### Fuzzing

`tests/fuzz_parser.c` is a libFuzzer target for everything a file goes
through: parse, load into an `AdifDocument`, save, and re-read. Beyond
the sanitizers' memory and UB checks, it asserts that adifedit's own
output re-parses with no warning and survives a second load and save
with every value, type indicator and header field intact. It needs
clang (the `fuzz` preset sets it):

```sh
cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p build/fuzz/corpus
build/fuzz/tests/fuzz_parser -max_total_time=300 build/fuzz/corpus tests/fuzz_corpus
```

`tests/fuzz_corpus/` holds the seeds. libFuzzer grows the working
corpus in the *first* directory it is given, so keep `build/fuzz/corpus`
first, or the seed directory fills with generated files.

When it finds a failing input it writes a `crash-*` file. Replay it with
`build/fuzz/tests/fuzz_parser crash-*`, fix the cause, and then:

- add a regression test for the cause in `tests/test_document.c`;
- copy the input into `tests/fuzz_corpus/` as `regress-<what-broke>.adi`
  (shrink it first with `-minimize_crash=1 -runs=10000` if it's large).

Every other build compiles the same file with a small `main()` instead
of libFuzzer. The `fuzz_corpus` ctest then replays `tests/fuzz_corpus/`
through the same checks, so a saved input stays fixed on every
`ctest` run and in CI, with no clang needed.
