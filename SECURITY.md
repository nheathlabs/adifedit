<!--
adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
Copyright (C) 2026 Nicholas Heath
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Security policy

## Supported versions

Only the latest release gets fixes.

## What counts

adifedit reads files that other people and programs wrote, so the main
risk is a malformed or malicious `.adi` / `.adif` or `.bst` file. These
are in scope:

- a crash, hang or memory error while opening, editing or saving a file;
- a file that makes adifedit write outside the file you chose to save;
- a saved file that silently loses or corrupts data adifedit showed you.

adifedit has no network access, no accounts and no server, so there is
nothing to report about those.

## Reporting

Please report privately, not in a public issue. Use GitHub's
[private vulnerability reporting](https://github.com/nheathlabs/adifedit/security/advisories/new)
(the repository's **Security** tab → **Report a vulnerability**).

Include the adifedit version (**Help → About**, or `adifedit --version`),
your OS, and, if you can, a minimal file that triggers the problem.
Remove any personal data from a real log before you send it.

You should get a reply within two weeks. Once a fix is released, the
advisory is published, with credit to you unless you'd rather not.
