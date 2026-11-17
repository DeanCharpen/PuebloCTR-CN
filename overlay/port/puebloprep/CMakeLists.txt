# PuebloPrep (R7, 2026-11-17): the console program and, with PUEBLOPREP_WINDOW=ON, its window (Dear ImGui on SDL3), for
# Windows (mingw-w64), macOS (universal: -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64") and Linux, from the patched tree
# (tools/make_tree.py). SDL3 and Dear ImGui are fetched at fixed versions and linked statically.
#
#   cmake -S port/puebloprep -B build/puebloprep -DCMAKE_BUILD_TYPE=Release [-DPUEBLOPREP_WINDOW=ON]
#   cmake --build build/puebloprep
#
# MIT licence (the repository's LICENSE).
cmake_minimum_required(VERSION 3.20)
project(PuebloPrep C CXX)

# (the console program is puebloprep-cli: macOS and Windows do not tell puebloprep from PuebloPrep)
option(PUEBLOPREP_WINDOW "Build the window (PuebloPrep) besides the console program (puebloprep-cli)" OFF)
set(PUEBLOPREP_VERSION "dev" CACHE STRING "The version the programs show")
set(SDL3_TAG "release-3.4.16" CACHE STRING "")
set(IMGUI_TAG "v1.92.9b" CACHE STRING "")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(PORT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(RT "${PORT}/whole/runtime")
set(T3 "${PORT}/whole/3ds")
set(AD "${PORT}/android-decomp/app/src/main/cpp")  # CubeEvil4's RVZ reader and zstd (decoder only)

# the texture key hash: cut from the port's own gx_decode.cpp (hashBytes, hashWords), so the keys are the console's
set(GXD "${RT}/gx_decode.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GXD}")
file(READ "${GXD}" gxd)
string(FIND "${gxd}" "u64 hashBytes(const u8* p, size_t n, u64 seed)" a)
string(FIND "${gxd}" "\nu64 hashWords(const u32* w, int n)" b)
if(a LESS 0 OR b LESS 0)
  message(FATAL_ERROR "gx_decode.cpp: hashBytes / hashWords not found")
endif()
math(EXPR len "${b} + 1 - ${a}")
string(SUBSTRING "${gxd}" ${a} ${len} part1)
math(EXPR b1 "${b} + 1")
string(SUBSTRING "${gxd}" ${b1} -1 rest)
string(FIND "${rest}" "\n}\n" e)
math(EXPR e "${e} + 3")
string(SUBSTRING "${rest}" 0 ${e} part2)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gen/tex_hash.inc" CONTENT "${part1}${part2}" @ONLY)

# the parts both programs share
add_library(zstd_dec STATIC "${AD}/third_party/zstd/zstddeclib.c")
add_library(disc_rvz STATIC "${AD}/disc_rvz.cpp")
target_include_directories(disc_rvz PRIVATE "${AD}")
add_library(texpak STATIC "${T3}/tools/texpak.cpp")
target_compile_definitions(texpak PRIVATE main=texpak_main)
set(PREP_SOURCES "${T3}/pica_texture.cpp" "${T3}/gx_texture.cpp" "${RT}/gci.cpp" "${RT}/card_files.cpp")
set(PREP_INCLUDES "${CMAKE_CURRENT_BINARY_DIR}/gen" "${AD}" "${RT}")
if(WIN32)
  # pread / pwrite / fsync of the RVZ reader on the CRT's
  target_compile_definitions(disc_rvz PRIVATE _FILE_OFFSET_BITS=64)
  target_compile_options(disc_rvz PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/win_posix.h")
endif()

function(prep_link tgt)
  target_link_libraries(${tgt} PRIVATE texpak disc_rvz zstd_dec)
  target_compile_definitions(${tgt} PRIVATE PUEBLOPREP_VERSION="${PUEBLOPREP_VERSION}")
  if(MINGW)
    target_link_options(${tgt} PRIVATE -static -s)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(${tgt} PRIVATE -static-libstdc++ -static-libgcc)  # (glibc only: 2.34 and later)
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${tgt} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
  endif()
endfunction()

add_executable(puebloprep-cli puebloprep.cpp ${PREP_SOURCES})
target_include_directories(puebloprep-cli PRIVATE ${PREP_INCLUDES})
prep_link(puebloprep-cli)

if(PUEBLOPREP_WINDOW)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG ${SDL3_TAG} GIT_SHALLOW TRUE)
  FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG ${IMGUI_TAG} GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL3 imgui)

  add_library(imgui_sdl3 STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp" "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
  target_include_directories(imgui_sdl3 PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
  target_link_libraries(imgui_sdl3 PUBLIC SDL3::SDL3-static)

  # the console part without its main, called by the window on a worker thread
  add_executable(PuebloPrep WIN32 MACOSX_BUNDLE puebloprep_gui.cpp puebloprep.cpp ${PREP_SOURCES})
  target_include_directories(PuebloPrep PRIVATE ${PREP_INCLUDES})
  target_compile_definitions(PuebloPrep PRIVATE PUEBLOPREP_WINDOW)
  target_link_libraries(PuebloPrep PRIVATE imgui_sdl3)
  prep_link(PuebloPrep)
  set_target_properties(PuebloPrep PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "PuebloPrep"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.puebloctr.puebloprep"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PUEBLOPREP_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PUEBLOPREP_VERSION}")
endif()
