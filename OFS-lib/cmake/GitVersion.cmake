# Writes the header that carries the version shown in the title bar and the
# about box. Run on every build rather than once at configure time, which is
# what it used to be: the title then went on naming whatever commit the build
# directory was first configured at. The file is only rewritten when the
# version changes, so an unchanged commit recompiles nothing.
#
# Called with -DGIT=<git> -DSRC=<source dir> -DOUT=<header> -DFALLBACK_TAG=<tag>.

set(tag "")
set(hash "")
if(GIT)
    execute_process(COMMAND "${GIT}" -C "${SRC}" describe --abbrev=0 --tags
        OUTPUT_VARIABLE tag ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${GIT}" -C "${SRC}" rev-parse --short HEAD
        OUTPUT_VARIABLE hash ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

# OFS-SE is often built from a source drop rather than a git checkout, in which
# case git has nothing to say. Fixed values keep the title meaningful.
if(tag STREQUAL "")
    set(tag "${FALLBACK_TAG}")
endif()
if(hash STREQUAL "")
    set(hash "sashimi")
endif()

set(content "#pragma once\n// Generated at build time by OFS-lib/cmake/GitVersion.cmake.\n#define OFS_LATEST_GIT_TAG \"${tag}\"\n#define OFS_LATEST_GIT_HASH \"${hash}\"\n")

set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
