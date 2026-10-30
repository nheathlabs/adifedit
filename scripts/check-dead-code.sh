#!/usr/bin/env bash
#
# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Finds exported (non-static) functions in an adifedit build that nothing
# calls, and ones only the tests call - dead code a per-file compiler
# can't see. (Unused statics, variables, parameters and
# macros are already build errors; see CMakeLists.txt.) Reads the object
# files with nm, so build first:
#
#   scripts/check-dead-code.sh [BUILD_DIR]     (default: build)
#
# A function that is deliberately test-only API goes in
# scripts/dead-code-allowlist.txt with a comment saying why. See
# CONTRIBUTING.md "Dead code".

set -euo pipefail
export LC_ALL=C # comm needs sort's byte order
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

build=${1:-build}
allowlist=scripts/dead-code-allowlist.txt

# Only this build tree's own objects - a nested build (build/debug) has
# its own CMakeFiles and would double-count.
mapfile -t objects < <(find "$build/CMakeFiles" "$build/tests/CMakeFiles" \
                            -name '*.o' 2>/dev/null | sort)
if [ "${#objects[@]}" -eq 0 ]; then
    echo "check-dead-code: no object files under $build - build it first" >&2
    exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

: > "$tmp/defined"
: > "$tmp/used_app"
: > "$tmp/used_test"
for o in "${objects[@]}"; do
    case $o in
        */tests/CMakeFiles/*) used=$tmp/used_test ;;
        *)
            used=$tmp/used_app
            nm -g --defined-only "$o" | awk '$2 == "T" { print $3 }' >> "$tmp/defined"
            ;;
    esac
    nm -u "$o" | awk '{ print $NF }' >> "$used"
done

# The allowlist; keep() also drops generated or framework-called names.
{ grep -vE '^[[:space:]]*(#|$)' "$allowlist" || true; } \
    | awk '{ print $1 }' | sort -u > "$tmp/allowed"
# nm can't see a call inside the same object (and -O3 may have inlined
# it), so a name nm calls unused is checked against the source: its
# definition and header declaration account for two lines in src/, any
# more is a real use.
used_in_src() { # <name>
    [ "$(cat src/*.c src/*.h | grep -cw -- "$1")" -gt 2 ]
}
keep() { # drop framework-called, allowlisted and source-used names
    local name
    { grep -vE '^(main|.*_get_type|.*_get_resource|.*_error_quark)$' || true; } \
        | comm -23 - "$tmp/allowed" \
        | while read -r name; do
              used_in_src "$name" || echo "$name"
          done
}

sort -u -o "$tmp/defined" "$tmp/defined"
sort -u -o "$tmp/used_app" "$tmp/used_app"
sort -u -o "$tmp/used_test" "$tmp/used_test"
sort -u "$tmp/used_app" "$tmp/used_test" > "$tmp/used_any"

unused=$(comm -23 "$tmp/defined" "$tmp/used_any" | keep)
test_only=$(comm -23 "$tmp/defined" "$tmp/used_app" \
            | comm -12 - "$tmp/used_test" | keep)

status=0
if [ -n "$unused" ]; then
    printf '\nExported, but nothing calls it -> delete it:\n%s\n' "$unused"
    status=1
fi
if [ -n "$test_only" ]; then
    printf '\nCalled only from tests -> delete it (and its tests), or list it in %s with a reason:\n%s\n' \
        "$allowlist" "$test_only"
    status=1
fi
if [ "$status" -ne 0 ]; then
    exit 1
fi

echo "dead code: clean"
