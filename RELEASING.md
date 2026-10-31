<!--
adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
Copyright (C) 2026 Nicholas Heath
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Releasing adifedit

Maintainer notes for cutting a release.

1. Bump `project(... VERSION x.y.z)` in `CMakeLists.txt`.
2. Add the release to `CHANGELOG.md` and a matching `<release>` to
   `io.github.nheathlabs.adifedit.metainfo.xml` (`ctest` validates the
   metainfo). The `CHANGELOG.md` entry becomes the release text, so keep
   its note that the Windows `.exe` and installer are unsigned
   (SmartScreen will warn on first run).
3. Merge to `master`, then tag that commit and push the tag:
   `git tag -a vx.y.z -m "adifedit x.y.z" && git push origin vx.y.z`.
4. The Release workflow (`.github/workflows/release.yml`) checks that the
   tag, `CMakeLists.txt`, `CHANGELOG.md` and the metainfo agree, then
   builds:
   - the Windows installer (`_setup.exe`) and portable zip
     (`_portable.zip`), and the library sources the LGPL asks for as a
     `-sources.tar.gz`;
   - the `.deb`, checked with lintian;
   - `SHA256SUMS` for all of them.

   It creates a draft GitHub release with those attached and the
   `CHANGELOG.md` entry as its text.
5. Review the draft on GitHub and publish it.

## Building the release files without a release

Run the Release workflow by hand (Actions → Release → Run workflow): it
builds the same files as artifacts and skips the draft release.

Locally, `scripts/windows-podman.sh --with-sources` builds the Windows
installer, portable zip and sources (see the README's [Windows build](README.md#windows-build)),
and `cmake --preset release && cmake --build --preset release &&
(cd build/release && cpack)` writes `build/release/adifedit_x.y.z_amd64.deb`.
