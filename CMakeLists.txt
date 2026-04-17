cmake_minimum_required(VERSION 3.17)  # 3.17 required for GIT_SUBMODULES_RECURSE
project(SIV VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # feeds IntelliSense via compile_commands.json

# ── Qt5 ───────────────────────────────────────────────────────────────────────
find_package(Qt5 COMPONENTS Widgets Concurrent REQUIRED)

# ── Six-library ───────────────────────────────────────────────────────────────
# Option A — point at a pre-built installation:
#   cmake -B build -DSIX_ROOT=/opt/six -DCMAKE_BUILD_TYPE=Release
#
# Option B — leave SIX_ROOT empty and six-library will be fetched and built
#   automatically from GitHub on the first configure (requires internet access).
#   Build instructions for reference:
#     git clone --recurse-submodules https://github.com/ngageoint/six-library.git
#     cd six-library && git checkout SIX-3.3.3 && git submodule update --init --recursive
#     cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/opt/six -DCMAKE_BUILD_TYPE=Release \
#           -DENABLE_TESTS=OFF -DENABLE_PYTHON=OFF
#     cmake --build build --parallel
#     cmake --install build

set(SIX_ROOT "" CACHE PATH "six-library install prefix — leave empty to auto-fetch")

set(SIX_FOUND FALSE)
set(SIX_LINK_TARGETS "")
set(SIX_INCLUDE_DIR "")

if(SIX_ROOT)
    # ── Pre-built path ────────────────────────────────────────────────────────
    list(APPEND CMAKE_PREFIX_PATH "${SIX_ROOT}")

    find_package(six.sicd-c++ QUIET)
    find_package(six.sidd-c++ QUIET)

    if(TARGET six.sicd-c++ AND TARGET six.sidd-c++)
        set(SIX_FOUND TRUE)
        set(SIX_LINK_TARGETS six.sicd-c++ six.sidd-c++)
        message(STATUS "six-library: found via CMake targets in ${SIX_ROOT}")
    else()
        find_path(SIX_INCLUDE_DIR NAMES six/NITFReadControl.h
            HINTS ${SIX_ROOT}/include)
        find_library(SIX_SICD_LIB NAMES six.sicd-c++ HINTS ${SIX_ROOT}/lib ${SIX_ROOT}/lib64)
        find_library(SIX_SIDD_LIB NAMES six.sidd-c++ HINTS ${SIX_ROOT}/lib ${SIX_ROOT}/lib64)
        find_library(SIX_BASE_LIB NAMES six-c++       HINTS ${SIX_ROOT}/lib ${SIX_ROOT}/lib64)
        find_library(NITF_LIB     NAMES nitf-c++      HINTS ${SIX_ROOT}/lib ${SIX_ROOT}/lib64)
        find_library(CODA_LIB     NAMES coda-oss       HINTS ${SIX_ROOT}/lib ${SIX_ROOT}/lib64)

        if(SIX_INCLUDE_DIR AND SIX_BASE_LIB)
            set(SIX_FOUND TRUE)
            set(SIX_LINK_TARGETS
                ${SIX_SICD_LIB} ${SIX_SIDD_LIB} ${SIX_BASE_LIB} ${NITF_LIB} ${CODA_LIB})
            message(STATUS "six-library: found via manual search in ${SIX_ROOT}")
        else()
            message(FATAL_ERROR
                "SIX_ROOT was set to '${SIX_ROOT}' but six-library was not found there.\n"
                "Check the path or leave SIX_ROOT empty to auto-fetch.")
        endif()
    endif()
else()
    # ── Auto-build via ExternalProject ────────────────────────────────────────
    # FetchContent doesn't work for six-library because coda_initialize_build()
    # is guarded by (CMAKE_PROJECT_NAME STREQUAL six-library) and is skipped
    # when six-library is a subdirectory of another project.
    # ExternalProject_Add builds it as its own top-level project, which works.
    message(STATUS "SIX_ROOT not set — six-library SIX-3.3.3 will be cloned and built automatically")
    message(STATUS "(First run downloads ~500 MB and builds six-library; subsequent runs are cached)")

    include(ExternalProject)

    set(SIX_AUTO_INSTALL_DIR ${CMAKE_BINARY_DIR}/six-install)

    set(_D ${SIX_AUTO_INSTALL_DIR}/lib)  # shorthand

    ExternalProject_Add(six_ext
        GIT_REPOSITORY    https://github.com/ngageoint/six-library.git
        GIT_TAG           SIX-3.3.3
        GIT_SUBMODULES_RECURSE TRUE
        # Patch six-library to build with C++17 (matches our app's ABI for
        # sys::filesystem::path) and suppress std::not1 deprecation errors.
        PATCH_COMMAND
            ${CMAKE_COMMAND}
            -DSIX_SOURCE=<SOURCE_DIR>
            -P ${CMAKE_SOURCE_DIR}/cmake/patch_six.cmake
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${SIX_AUTO_INSTALL_DIR}
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DBUILD_SHARED_LIBS=OFF
            -DENABLE_PYTHON=OFF
        # After installing six itself, also install the coda-oss and nitro
        # headers/libs that six marks EXCLUDE_FROM_ALL and therefore skips.
        INSTALL_COMMAND
            ${CMAKE_COMMAND} --install <BINARY_DIR>
            COMMAND ${CMAKE_COMMAND}
                    -DSIX_BUILD_DIR=<BINARY_DIR>
                    -DSIX_SOURCE_DIR=<SOURCE_DIR>
                    -DSIX_INSTALL_DIR=${SIX_AUTO_INSTALL_DIR}
                    -P ${CMAKE_SOURCE_DIR}/cmake/install_externals.cmake
        # Key byproducts so Ninja knows to wait for them before linking SIV.
        BUILD_BYPRODUCTS
            ${_D}/libsix.sicd-c++.a
            ${_D}/libsix.sidd-c++.a
            ${_D}/libsix-c++.a
            ${_D}/libscene-c++.a
            ${_D}/libnitf-c++.a
            ${_D}/libnitf-c.a
            ${_D}/libnrt-c.a
            ${_D}/libsys-c++.a
            ${_D}/libio-c++.a
            ${_D}/libstr-c++.a
            ${_D}/libexcept-c++.a
            ${_D}/liblogging-c++.a
            ${_D}/libmem-c++.a
            ${_D}/libmt-c++.a
            ${_D}/libxml.lite-c++.a
            ${_D}/libxerces-c.a
            ${_D}/libXML_DATA_CONTENT-static-c.a
            ${_D}/libj2k-c.a
            ${_D}/libtiff-c++.a
            ${_D}/libpolygon-c++.a
    )

    # The include directory must exist at configure time for INTERFACE targets.
    file(MAKE_DIRECTORY ${SIX_AUTO_INSTALL_DIR}/include)

    # Interface target that carries headers + libs for SIV to consume.
    # All transitive static deps must be listed explicitly for the linker.
    add_library(six_auto INTERFACE)
    add_dependencies(six_auto six_ext)
    target_include_directories(six_auto INTERFACE ${SIX_AUTO_INSTALL_DIR}/include)
    # Wrap all static libs in --start-group/--end-group so GNU ld rescans
    # archives to resolve circular dependencies between coda-oss modules.
    target_link_libraries(six_auto INTERFACE
        -Wl,--start-group
        ${_D}/libsix.sicd-c++.a
        ${_D}/libsix.sidd-c++.a
        ${_D}/libsix-c++.a
        ${_D}/libsix.convert-c++.a
        ${_D}/libscene-c++.a
        ${_D}/libnitf-c++.a
        ${_D}/libnitf-c.a
        ${_D}/libXML_DATA_CONTENT-static-c.a
        ${_D}/libj2k-c.a
        ${_D}/libnrt-c.a
        ${_D}/libxml.lite-c++.a
        ${_D}/libxerces-c.a
        ${_D}/libtiff-c++.a
        ${_D}/libio-c++.a
        ${_D}/libsys-c++.a
        ${_D}/libstr-c++.a
        ${_D}/libexcept-c++.a
        ${_D}/liblogging-c++.a
        ${_D}/libmem-c++.a
        ${_D}/libmt-c++.a
        ${_D}/libsio.lite-c++.a
        ${_D}/libplugin-c++.a
        ${_D}/libmath-c++.a
        ${_D}/libmath.linear-c++.a
        ${_D}/libpolygon-c++.a
        ${_D}/libtypes-c++.a
        ${_D}/libopenjpeg.a
        ${_D}/libCSMAPI-static.a
        -Wl,--end-group
    )

    set(SIX_FOUND TRUE)
    set(SIX_LINK_TARGETS six_auto)
    set(SIX_INCLUDE_DIR ${SIX_AUTO_INSTALL_DIR}/include)
endif()

# ── Application ───────────────────────────────────────────────────────────────
add_executable(SIV
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/ImageViewer.cpp
    src/ui/HistogramWidget.cpp
    src/io/SARImageLoader.cpp
    src/io/TileProvider.cpp
)

target_include_directories(SIV PRIVATE
    src
    ${SIX_INCLUDE_DIR}
)

target_compile_options(SIV PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(SIV PRIVATE
    Qt5::Widgets
    Qt5::Concurrent
    ${SIX_LINK_TARGETS}
)

target_compile_definitions(SIV PRIVATE SARVIEWER_HAS_SIX)

# Embed RPATH so the binary finds bundled libs in ../lib relative to itself.
set_target_properties(SIV PROPERTIES
    OUTPUT_NAME             "siv"
    INSTALL_RPATH           "$ORIGIN/../lib"
    BUILD_WITH_INSTALL_RPATH FALSE
    INSTALL_RPATH_USE_LINK_PATH FALSE
)

# ── Install ───────────────────────────────────────────────────────────────────
# Install prefix defaults to <repo>/install so "cmake --install build" works
# without any extra flags.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OR
   CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH
        "Install prefix — defaults to <repo>/install" FORCE)
endif()

install(TARGETS SIV RUNTIME DESTINATION bin)

# Copy the six-library test NITF/SICD/SIDD files so there are sample images
# to open immediately after install.  Only copy when six_ext was auto-fetched
# (SIX_ROOT not set); with a pre-built install the user manages their own data.
if(NOT SIX_ROOT)
    set(_SIX_TEST_IMAGES
        # Cropped SICD samples (one per spec version)
        croppedNitfs/SICD/cropped_sicd_040.nitf
        croppedNitfs/SICD/cropped_sicd_041.nitf
        croppedNitfs/SICD/cropped_sicd_050.nitf
        croppedNitfs/SICD/cropped_sicd_100.nitf
        croppedNitfs/SICD/cropped_sicd_101.nitf
        croppedNitfs/SICD/cropped_sicd_110.nitf
        croppedNitfs/SICD/cropped_sicd_120.nitf
        croppedNitfs/SICD/cropped_sicd_extra_des.nitf
        # Cropped SIDD sample
        croppedNitfs/SIDD/cropped_sidd.nitf
    )

    # Resolve paths relative to the six-library source fetched by ExternalProject.
    ExternalProject_Get_Property(six_ext SOURCE_DIR)
    set(_six_src "${SOURCE_DIR}")

    foreach(_img ${_SIX_TEST_IMAGES})
        set(_full "${_six_src}/${_img}")
        if(EXISTS "${_full}")
            install(FILES "${_full}" DESTINATION sample_images)
        endif()
    endforeach()
endif()

# ── Self-contained packaging ───────────────────────────────────────────────────
# Locate the Qt plugin directory via qmake so bundle_libs.cmake can copy them.
find_program(_qmake NAMES qmake-qt5 qmake REQUIRED)
execute_process(
    COMMAND "${_qmake}" -query QT_INSTALL_PLUGINS
    OUTPUT_VARIABLE _qt_plugins
    OUTPUT_STRIP_TRAILING_WHITESPACE)

# At install time: copy all .so dependencies and Qt plugins, write launcher.
install(CODE "set(_siv_prefix \"${CMAKE_INSTALL_PREFIX}\")")
install(CODE "set(_qt_plugins  \"${_qt_plugins}\")")
install(SCRIPT "${CMAKE_SOURCE_DIR}/cmake/bundle_libs.cmake")

# ── CPack RPM ─────────────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME                "SIV")
set(CPACK_PACKAGE_VENDOR              "SARViewer")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "SAR Image Viewer — SICD and SIDD NITF files")
set(CPACK_PACKAGE_VERSION             "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR       "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR       "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH       "${PROJECT_VERSION_PATCH}")

set(CPACK_GENERATOR "RPM")
set(CPACK_RPM_PACKAGE_LICENSE         "Proprietary")
set(CPACK_RPM_PACKAGE_GROUP           "Applications/Multimedia")
set(CPACK_RPM_PACKAGE_AUTOREQPROV     "no")   # all deps are bundled
set(CPACK_RPM_PACKAGE_DESCRIPTION
    "SIV is a high-performance viewer for SICD and SIDD NITF SAR imagery.")

# Prevent RPM from complaining about directories it doesn't own.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr /usr/bin /usr/lib /usr/share /usr/local)

include(CPack)
