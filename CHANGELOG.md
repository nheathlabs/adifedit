<!--
adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
Copyright (C) 2026 Nicholas Heath
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Changelog

Notable changes to adifedit, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions
follow [Semantic Versioning](https://semver.org/). Each release also has a
matching `<release>` in `io.github.nheathlabs.adifedit.metainfo.xml`.

## [Unreleased]

## [0.0.3] - 2026-10-30

### Added

- A Windows installer (`adifedit-x.y.z-windows-x86_64_setup.exe`), for
  all users or just the current one, with a Start-menu entry, `.adi` /
  `.adif` file association and an uninstaller in Apps & features.

### Changed

- The Windows zip is now named
  `adifedit-x.y.z-windows-x86_64_portable.zip`.

### Known limitations

- The Windows installer and `.exe` are not code-signed, so SmartScreen
  warns on first run. Choose "More info" → "Run anyway".
- The test suite runs on Linux only.
- Not yet available: importing an ADIF file into the open log, and
  exporting selected rows as a new file.

## [0.0.2] - 2026-10-29

### Added

- A Debian package (`adifedit_0.0.2_amd64.deb`) for Ubuntu and Debian,
  with its library dependencies worked out at build time, a copyright
  file and a changelog.
- Releases are built by CI from the version tag: the Windows zip and
  its library sources, the `.deb`, and `SHA256SUMS`.

### Changed

- Security issues are reported through GitHub issues or pull requests,
  not private advisories.

### Known limitations

- The Windows `.exe` is not code-signed, so SmartScreen warns on first
  run. Choose "More info" → "Run anyway".
- Windows has no installer, Start-menu entry or `.adi` file association.
  Unzip the folder and run `bin\adifedit.exe`.
- The test suite runs on Linux only.
- Not yet available: importing an ADIF file into the open log, and
  exporting selected rows as a new file.

## [0.0.1] - 2026-10-15

First public release.

### Added

- View any `.adi` / `.adif` log as an editable grid: one row per record,
  one column per field.
- Edit cells in place; insert, delete and fill rows and columns from the
  toolbar or a right-click menu.
- Undo / redo for everything, with multi-step actions as one step.
- Find and Find & Replace across cell values, in all columns or one.
- Batch Stamp: write the same set of fields into every record from a
  reusable `.bst` template.
- Files with mismatched field lengths open on a best-effort basis: each
  fitted field is highlighted, and the original is never saved over
  (`(parse_warn)`).
- Saves plain ADIF 3.1.7, recording the source logger in the header and
  keeping the rest of the loaded header (`USERDEFn`, other programs'
  `APP_` fields) and fields' type indicators. Saving through a symlink
  writes the file it points at and keeps its permissions.
- Threaded load and save; a prompt before unsaved changes are lost, and
  before saving over a file that another program changed after it was
  opened.
- Linux install with desktop entry, MIME type, icons and AppStream
  metadata; a portable 64-bit Windows zip.

### Known limitations

- The Windows `.exe` is not code-signed, so SmartScreen warns on first
  run. Choose "More info" → "Run anyway".
- Windows has no installer, Start-menu entry or `.adi` file association.
  Unzip the folder and run `bin\adifedit.exe`.
- The test suite runs on Linux only.
- Not yet available: importing an ADIF file into the open log, and
  exporting selected rows as a new file.
