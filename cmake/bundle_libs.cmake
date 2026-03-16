# cmake/bundle_libs.cmake
# Runs at install time (via install(SCRIPT ...)) to copy all required shared
# libraries into <prefix>/lib and write a launcher script.
#
# Variables expected to be set by the caller:
#   _siv_prefix  — CMAKE_INSTALL_PREFIX
#   _qt_plugins  — Qt plugin directory (e.g. /usr/lib/qt5/plugins)

cmake_minimum_required(VERSION 3.16)

set(_bin     "${_siv_prefix}/bin/SIV")
set(_lib_dst "${_siv_prefix}/lib")
set(_plug_dst "${_siv_prefix}/plugins")

file(MAKE_DIRECTORY "${_lib_dst}" "${_plug_dst}")

# ── 1. Collect runtime shared-library dependencies ────────────────────────────
# Exclude only the libraries that are tied to the kernel ABI (glibc, ld-linux)
# or to the GPU driver stack (libGL/libEGL — must match the host GPU).
# Everything else (Qt, XCB, libstdc++, …) gets bundled.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${_bin}"
    RESOLVED_DEPENDENCIES_VAR   _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    CONFLICTING_DEPENDENCIES_PREFIX _conflict   # don't error on duplicates
    PRE_EXCLUDE_REGEXES
        "^ld-linux"
        "^libc\\.so"
        "^libdl\\.so"
        "^libm\\.so"
        "^libpthread\\.so"
        "^librt\\.so"
        "^libresolv\\.so"
        "^libnss"
        "^libGL\\.so"
        "^libGLX\\.so"
        "^libEGL\\.so"
        "^libGLdispatch\\.so"
    POST_EXCLUDE_REGEXES
        # Don't pick up libs we installed in a previous run — prefer system copy
        "^${_siv_prefix}"
)

foreach(_dep ${_resolved})
    file(INSTALL "${_dep}" DESTINATION "${_lib_dst}" FOLLOW_SYMLINK_CHAIN)
endforeach()

if(_unresolved)
    message(STATUS "bundle_libs: unresolved (will use system): ${_unresolved}")
endif()

# ── 2. Qt plugins ─────────────────────────────────────────────────────────────
# Platform plugin (xcb required for X11; wayland optional).
# Image format + icon engine plugins are loaded on demand by Qt.
foreach(_cat platforms xcbglintegrations imageformats iconengines)
    set(_src "${_qt_plugins}/${_cat}")
    if(EXISTS "${_src}")
        file(GLOB _so "${_src}/*.so")
        foreach(_f ${_so})
            file(INSTALL "${_f}" DESTINATION "${_plug_dst}/${_cat}")
        endforeach()
    endif()
endforeach()

# ── 3. Launcher script ────────────────────────────────────────────────────────
# Sets LD_LIBRARY_PATH and QT_PLUGIN_PATH relative to the script location so
# the package works from any install prefix.
set(_launcher "${_siv_prefix}/bin/siv")
file(WRITE "${_launcher}"
[=[#!/bin/sh
DIR=$(dirname $(readlink -f "$0"))
if [ -n "$LD_LIBRARY_PATH" ]; then
    export LD_LIBRARY_PATH="$DIR/../lib:$LD_LIBRARY_PATH"
else
    export LD_LIBRARY_PATH="$DIR/../lib"
fi
export QT_PLUGIN_PATH="$DIR/../plugins"
exec "$DIR/SIV" "$@"
]=])
file(CHMOD "${_launcher}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE)

message(STATUS "bundle_libs: bundled ${_resolved}")
