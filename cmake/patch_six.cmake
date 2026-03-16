# cmake/patch_six.cmake
# Applied via ExternalProject_Add PATCH_COMMAND after every source update.
# All replacements are idempotent — safe to run multiple times.
#
# Variables (passed via -D):
#   SIX_SOURCE — path to the six-library source root (<SOURCE_DIR>)

cmake_minimum_required(VERSION 3.17)

set(_cml "${SIX_SOURCE}/CMakeLists.txt")
file(READ "${_cml}" _content)

# 1. Build with C++17 so sys::filesystem::path == std::filesystem::path,
#    matching the ABI our C++17 application code sees when including six headers.
string(REPLACE
    "set(CMAKE_CXX_STANDARD 14)"
    "set(CMAKE_CXX_STANDARD 17)"
    _content "${_content}")

# 2. Suppress deprecated-declarations as error — std::not1 was deprecated in
#    C++17 and six's code uses it; keep it a warning only.
string(REPLACE
    "add_compile_options(-Werror)"
    "add_compile_options(-Werror -Wno-error=deprecated-declarations)"
    _content "${_content}")

file(WRITE "${_cml}" "${_content}")
message(STATUS "patch_six: applied C++17 + deprecated-declarations patches")
