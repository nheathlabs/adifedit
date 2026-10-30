# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run at build time by the `adifedit_build_info` target. Rewrites OUT with
# the current git commit and a UTC build timestamp.
#   -DSRC=<source dir>  -DOUT=<header path>  -DGIT=<git executable>

set(hash "unknown")
if(GIT AND EXISTS "${SRC}/.git")
    execute_process(
        COMMAND "${GIT}" -C "${SRC}" rev-parse --short HEAD
        OUTPUT_VARIABLE _hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _git_result
        ERROR_QUIET)
    if(_git_result EQUAL 0 AND NOT _hash STREQUAL "")
        set(hash "${_hash}")
    endif()
endif()

# UTC build timestamp, ISO 8601 to the second.
string(TIMESTAMP built "%Y-%m-%dT%H:%M:%SZ" UTC)

file(WRITE "${OUT}"
    "#pragma once\n"
    "#define ADIFEDIT_GIT_HASH \"${hash}\"\n"
    "#define ADIFEDIT_BUILD_TIME \"${built}\"\n")
