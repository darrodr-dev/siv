# cmake/install_externals.cmake
# Called from ExternalProject_Add INSTALL_COMMAND to install coda-oss and nitro
# headers/libraries that six-library marks EXCLUDE_FROM_ALL.
#
# Uses direct file copies instead of cmake --install to avoid cmake_install.cmake
# rules that try to install unbuilt EXCLUDE_FROM_ALL executables and fail.
#
# Variables (passed via -D):
#   SIX_BUILD_DIR   — ExternalProject build directory (six_ext-prefix/src/six_ext-build)
#   SIX_SOURCE_DIR  — ExternalProject source directory (six_ext-prefix/src/six_ext)
#   SIX_INSTALL_DIR — install prefix (six-install)

cmake_minimum_required(VERSION 3.17)

set(_inc "${SIX_INSTALL_DIR}/include")
set(_lib "${SIX_INSTALL_DIR}/lib")
file(MAKE_DIRECTORY "${_inc}" "${_lib}")

# ── Helper: copy a source include tree and all built static libs ──────────────
# Usage: install_module(SRC_INCLUDE BUILD_DIR)
#   SRC_INCLUDE — source include/ directory (headers copied to ${_inc}/)
#   BUILD_DIR   — build directory where *.a files live

function(install_module src_include build_dir)
    if(EXISTS "${src_include}")
        file(COPY "${src_include}/" DESTINATION "${_inc}")
    endif()
    if(EXISTS "${build_dir}")
        file(GLOB _libs "${build_dir}/*.a")
        foreach(_l ${_libs})
            file(COPY "${_l}" DESTINATION "${_lib}")
        endforeach()
    endif()
endfunction()

# ── coda-oss C++ modules ──────────────────────────────────────────────────────
set(_coda_src   "${SIX_SOURCE_DIR}/externals/coda-oss")
set(_coda_build "${SIX_BUILD_DIR}/externals/coda-oss")

# config headers (no lib)
foreach(_hdir
    "${_coda_src}/modules/c++/config/include"
    "${_coda_build}/modules/c++/config/include")  # generated headers
    if(EXISTS "${_hdir}")
        file(COPY "${_hdir}/" DESTINATION "${_inc}")
    endif()
endforeach()

# coda_oss umbrella headers (no lib)
if(EXISTS "${_coda_src}/modules/c++/coda_oss/include")
    file(COPY "${_coda_src}/modules/c++/coda_oss/include/" DESTINATION "${_inc}")
endif()

set(_coda_modules std sys io except str logging mem mt re
                  math math.linear math.poly polygon types units
                  gsl sio.lite plugin xml.lite tiff)

foreach(_mod ${_coda_modules})
    install_module(
        "${_coda_src}/modules/c++/${_mod}/include"
        "${_coda_build}/modules/c++/${_mod}")
endforeach()

# Driver libs (openjpeg, xerces-c built under drivers/)
file(GLOB _drv_libs "${_coda_build}/modules/drivers/*/*/*.a")
foreach(_l ${_drv_libs})
    file(COPY "${_l}" DESTINATION "${_lib}")
endforeach()

# ── nitro ─────────────────────────────────────────────────────────────────────
set(_nitro_src   "${SIX_SOURCE_DIR}/externals/nitro")
set(_nitro_build "${SIX_BUILD_DIR}/externals/nitro")

# nrt (C runtime)
install_module(
    "${_nitro_src}/modules/c/nrt/include"
    "${_nitro_build}/modules/c/nrt")

# nitf C
install_module(
    "${_nitro_src}/modules/c/nitf/include"
    "${_nitro_build}/modules/c/nitf")

# XML_DATA_CONTENT — sits alongside libnitf-c.a but has its own name
set(_xml_dc "${_nitro_build}/modules/c/nitf/libXML_DATA_CONTENT-static-c.a")
if(EXISTS "${_xml_dc}")
    file(COPY "${_xml_dc}" DESTINATION "${_lib}")
endif()

# j2k C
install_module(
    "${_nitro_src}/modules/c/j2k/include"
    "${_nitro_build}/modules/c/j2k")

# nitf C++ wrappers
install_module(
    "${_nitro_src}/modules/c++/nitf/include"
    "${_nitro_build}/modules/c++/nitf")

message(STATUS "install_externals: done — ${_inc}, ${_lib}")
