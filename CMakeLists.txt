cmake_minimum_required(VERSION 3.16)
project(UMDPlay VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# CI passes -DCMAKE_CXX_FLAGS="-Werror"; expose the same bar as a first-class
# option so a local build can opt in without hand-rolling flags.
option(UMDPLAY_WERROR "Treat project warnings as errors" OFF)

# AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer for the project
# targets. Mirrors the flags used for local verification; see tests/lsan.supp
# for the system-library leak suppressions (SDL2/dbus, Mesa/llvmpipe).
option(UMDPLAY_SANITIZE "Build with ASan + UBSan (+ LSan)" OFF)

# Use the built-in ATRAC3+ decoder (src/atrac3p/) instead of libavcodec's
# atrac3p + libswresample. With this ON, libswresample is neither required nor
# linked; the audio path is entirely UMDPlay's own code. Turn OFF to A/B the
# audio against FFmpeg's decoder.
option(UMDPLAY_NATIVE_ATRAC3P "Use the native ATRAC3+ decoder (no libswresample)" ON)

# Use the built-in H.264 decoder (third_party/h264dec) instead of libavcodec,
# and the built-in YUV deinterlacer instead of libavfilter. With this ON the
# player and core need no FFmpeg library at all. Turn OFF to build the optional
# libavcodec backends and A/B the video against FFmpeg (they are only built
# when FFmpeg is installed).
option(UMDPLAY_NATIVE_VIDEO "Use the native H.264 decoder (no libavcodec)" ON)

# Build the AveraUI Qt 6 Quick front end (libs/avera-ui + apps/avera-player).
# Off by default so the core, tests and CLI never need Qt installed.
option(UMDPLAY_AVERA_UI "Build the AveraUI Qt 6 Quick front end" OFF)

# Project warning bar, selected per compiler so the build is clean under GCC,
# Clang, MinGW and MSVC alike. Third-party code is silenced separately so
# vendored sources never fail the project's -Werror.
if(MSVC)
    set(UMDPLAY_WARN_FLAGS /W4)
    if(UMDPLAY_WERROR)
        list(APPEND UMDPLAY_WARN_FLAGS /WX)
    endif()
    set(UMDPLAY_THIRDPARTY_WARN_FLAGS /w)
else()
    set(UMDPLAY_WARN_FLAGS -Wall -Wextra -Wpedantic)
    if(UMDPLAY_WERROR)
        list(APPEND UMDPLAY_WARN_FLAGS -Werror)
    endif()
    set(UMDPLAY_THIRDPARTY_WARN_FLAGS -w)
endif()

if(UMDPLAY_SANITIZE)
    if(MSVC)
        # MSVC only implements ASan (no UBSan); enable what it has.
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/third_party)

# --- SDL2 (system) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# --- FFmpeg (only for the optional A/B backends) ---
# The default build decodes H.264 and ATRAC3+ with UMDPlay's own code and needs
# no FFmpeg at all. Setting either NATIVE option to OFF pulls in the matching
# libav* backend, which then has to be installed.
set(UMDPLAY_USE_FFMPEG OFF)
if(NOT UMDPLAY_NATIVE_VIDEO OR NOT UMDPLAY_NATIVE_ATRAC3P)
    set(UMDPLAY_USE_FFMPEG ON)
    pkg_check_modules(AVCODEC REQUIRED libavcodec)
    pkg_check_modules(AVUTIL REQUIRED libavutil)
    if(NOT UMDPLAY_NATIVE_ATRAC3P)
        pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
    endif()
endif()

# On macOS the SDL2main shim is unnecessary: it only redirects main() to
# SDL_main() on Windows. Homebrew's SDL2 (the sdl2-compat layer) nevertheless
# advertises -lSDL2main in sdl2.pc, and that archive is not linkable there, so
# drop it to avoid "ld: library 'SDL2main' not found".
if(APPLE)
    list(REMOVE_ITEM SDL2_LIBRARIES SDL2main)
    list(REMOVE_ITEM SDL2_STATIC_LIBRARIES SDL2main)
endif()

include_directories(
    ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${SWRESAMPLE_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
)

# pkg-config reports -L directories for each module. On Linux these are already
# default linker search paths, but on Homebrew (macOS) FFmpeg/SDL2 live under
# /opt/homebrew/opt/*/lib, so pass the directories through or linking fails with
# "ld: library 'SDL2' not found". Must precede the targets that link them.
link_directories(
    ${AVCODEC_LIBRARY_DIRS} ${AVUTIL_LIBRARY_DIRS} ${SWRESAMPLE_LIBRARY_DIRS}
    ${SDL2_LIBRARY_DIRS}
)

# --- Dear ImGui (lightweight, fetched & pinned) ---
include(FetchContent)
set(IMGUI_TAG v1.91.5)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${IMGUI_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_backends STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(imgui_backends PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${SDL2_INCLUDE_DIRS}
)
target_link_libraries(imgui_backends PUBLIC ${SDL2_LIBRARIES})
target_compile_options(imgui_backends PRIVATE ${UMDPLAY_THIRDPARTY_WARN_FLAGS})   # third-party: silence its warnings

# --- libchdr (Compressed Hunks of Data; fetched & pinned) ---
# libchdr is a C library with vendored lzma/miniz/zstd under deps/. It is
# consumed as a static archive only; its own tests and shared library are
# disabled so nothing else is built. Its warnings are silenced (-w) because the
# project keeps -Werror on its own targets — third-party code must not fail the
# build. This has to be set before the subdirectory is added (its targets do not
# accept -w via target_compile_options once created with CMAKE_C_FLAGS unset).
set(CHDR_TAG v0.3.0)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ZLIB OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ZSTD OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    chdr
    GIT_REPOSITORY https://github.com/rtissera/libchdr.git
    GIT_TAG        ${CHDR_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(chdr)

# Silence third-party warnings and keep it out of sanitizer instrumentation, so
# a project-wide -Werror / ASan+UBSan run is never failed by vendored code.
# libchdr's reference bitstream code does a benign signed left shift that UBSan
# flags; excluding it here keeps the project's own sources the only ones under
# test. Both flags are harmless in a normal (non-sanitizer) build.
foreach(_chdr_tgt chdr-static miniz zstd chdr-lzma)
    if(TARGET ${_chdr_tgt})
        target_compile_options(${_chdr_tgt} PRIVATE ${UMDPLAY_THIRDPARTY_WARN_FLAGS})
        if(NOT MSVC)
            # Keep vendored code out of sanitizer instrumentation (a benign
            # signed left shift in its bitstream code trips UBSan).
            target_compile_options(${_chdr_tgt} PRIVATE -fno-sanitize=undefined,address)
        endif()
    endif()
endforeach()

# --- h264dec (vendored H.264 decoder, MIT; see third_party/h264dec) ---
# Pure C, standard library only. Compiled as its own static library with its
# warnings silenced so vendored code never fails the project's -Werror.
add_library(h264dec STATIC
    third_party/h264dec/h264dec.c
    third_party/h264dec/h264dec_slice.c
    third_party/h264dec/h264dec_recon.c
    third_party/h264dec/h264dec_deblock.c
    third_party/h264dec/h264_cabac_tables.c
    third_party/h264dec/h264.c
)
set_target_properties(h264dec PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_include_directories(h264dec PUBLIC ${PROJECT_SOURCE_DIR}/third_party/h264dec)
target_compile_options(h264dec PRIVATE ${UMDPLAY_THIRDPARTY_WARN_FLAGS})

# stb single-header libs (implementation lives in src/stb_impl.cpp)
add_library(stb_image STATIC src/stb_impl.cpp)
target_include_directories(stb_image PUBLIC ${PROJECT_SOURCE_DIR}/third_party)

# --- Official logo (embedded PNG) ------------------------------------------
# Bake assets/umdplay_logo.png into a static library so the WMC header mark and
# the SDL window icon work from a bare release binary (the bundles ship no
# assets/ directory). cmake/EmbedAsset.cmake turns the PNG into a byte array.
set(UMDPLAY_LOGO_PNG  ${PROJECT_SOURCE_DIR}/assets/umdplay_logo.png)
set(UMDPLAY_LOGO_DATA ${CMAKE_BINARY_DIR}/generated/umdplay_logo_data.cpp)
add_custom_command(
    OUTPUT  ${UMDPLAY_LOGO_DATA}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${UMDPLAY_LOGO_PNG}
            -DOUTPUT=${UMDPLAY_LOGO_DATA}
            -DSYMBOL=umdplay_logo_png
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedAsset.cmake
    DEPENDS ${UMDPLAY_LOGO_PNG} ${PROJECT_SOURCE_DIR}/cmake/EmbedAsset.cmake
    COMMENT "Embedding assets/umdplay_logo.png"
    VERBATIM
)
add_library(umdplay_logo STATIC ${UMDPLAY_LOGO_DATA})
target_include_directories(umdplay_logo PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_options(umdplay_logo PRIVATE ${UMDPLAY_WARN_FLAGS})

# --- UMDPlay core (decoder + ISO reader + SFO parser), shared by app & tests ---
add_library(umdplay_core STATIC
    src/disc_reader.cpp
    src/iso_reader.cpp
    src/param_sfo.cpp
    src/psmf_demuxer.cpp
    src/decoder.cpp
    src/h264_video_decoder.cpp
    src/deinterlace.cpp
    src/av_sync.cpp
    src/yuv_convert.cpp
    src/atrac3p_decoder.cpp
    src/atrac3p/atrac3p_parse.cpp
    src/atrac3p/atrac3p_dsp.cpp
    src/atrac3p/atrac3p_mdct.cpp
    src/atrac3p/atrac3p_tables.cpp
    src/recent.cpp
    src/gesture.cpp
    src/playlist_parser.cpp
    src/rco_parser.cpp
    src/vsmx_vm.cpp
    src/rco_menu.cpp
)

# Optional FFmpeg backends (A/B builds only; never part of the default build).
if(UMDPLAY_USE_FFMPEG)
    target_compile_definitions(umdplay_core PUBLIC UMDPLAY_FFMPEG_BACKENDS=1)
    if(NOT UMDPLAY_NATIVE_VIDEO)
        target_sources(umdplay_core PRIVATE src/ffmpeg_video_decoder.cpp)
    endif()
    if(NOT UMDPLAY_NATIVE_ATRAC3P)
        target_sources(umdplay_core PRIVATE src/ffmpeg_audio_decoder.cpp)
    endif()
endif()
target_include_directories(umdplay_core PUBLIC
    ${chdr_SOURCE_DIR}/include
)
# Playback demuxing is native (PsmfDemuxer over DiscReader) and, by default,
# so is everything else: H.264 comes from third_party/h264dec and ATRAC3+ from
# src/atrac3p/, so umdplay_core links no FFmpeg library at all. The libav*
# backends exist only for A/B builds (-DUMDPLAY_NATIVE_VIDEO=OFF /
# -DUMDPLAY_NATIVE_ATRAC3P=OFF) and are linked in only then.
target_link_libraries(umdplay_core PUBLIC chdr-static h264dec)
if(UMDPLAY_NATIVE_ATRAC3P)
    target_compile_definitions(umdplay_core PUBLIC UMDPLAY_NATIVE_ATRAC3P=1)
endif()
if(UMDPLAY_NATIVE_VIDEO)
    target_compile_definitions(umdplay_core PUBLIC UMDPLAY_NATIVE_VIDEO=1)
endif()
if(UMDPLAY_USE_FFMPEG)
    target_link_libraries(umdplay_core PUBLIC ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    if(NOT UMDPLAY_NATIVE_ATRAC3P)
        target_link_libraries(umdplay_core PUBLIC ${SWRESAMPLE_LIBRARIES})
    endif()
    message(STATUS "UMDPlay: FFmpeg A/B backends enabled (libavcodec)")
else()
    message(STATUS "UMDPlay: native H.264 + ATRAC3+ decoders — zero FFmpeg libraries")
endif()
target_compile_options(umdplay_core PRIVATE ${UMDPLAY_WARN_FLAGS})

# --- Procedural WMC UI sound effects (SDL2 audio; no bundled samples) ---
# Split into its own small library so both the player and its unit test link it
# without dragging in the whole decoder/ImGui stack.
add_library(umdplay_ui_sfx STATIC src/ui_sfx.cpp)
target_include_directories(umdplay_ui_sfx PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(umdplay_ui_sfx PUBLIC ${SDL2_LIBRARIES})
target_compile_options(umdplay_ui_sfx PRIVATE ${UMDPLAY_WARN_FLAGS})

# --- Player executable ---
add_executable(umdplay
    src/main.cpp
    src/ui_manager.cpp
    src/wmc_ui.cpp
    src/clip_export.cpp
    src/rco_texture.cpp
)
target_link_libraries(umdplay PRIVATE
    umdplay_core umdplay_ui_sfx imgui_backends stb_image umdplay_logo
    ${SDL2_LIBRARIES}
)
target_compile_options(umdplay PRIVATE ${UMDPLAY_WARN_FLAGS})

# Windows: attach the official icon to umdplay.exe via a resource script. The
# RC compiler resolves "umdplay.ico" relative to assets/, next to the .rc. Other
# platforms use the runtime window icon (UiManager::set_window_icon).
if(WIN32)
    enable_language(RC)
    target_sources(umdplay PRIVATE assets/umdplay.rc)
    set_source_files_properties(assets/umdplay.rc PROPERTIES
        COMPILE_FLAGS "-I\"${PROJECT_SOURCE_DIR}/assets\"")
endif()

# --- Tests ---
enable_testing()

add_executable(test_audio_decoder tests/test_audio_decoder.cpp)
target_link_libraries(test_audio_decoder PRIVATE umdplay_core)
target_compile_options(test_audio_decoder PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_audio_decoder COMMAND test_audio_decoder)

# Live playback tweaks: speed ladder, audio-sync nudge, A-B loop state machine,
# latency-corrected audio clock, pitch-preserving time stretch, and the 4:3
# default for square-flagged 720x480 UMD video.
add_executable(test_tuning tests/test_tuning.cpp)
target_link_libraries(test_tuning PRIVATE umdplay_core)
target_compile_options(test_tuning PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_tuning COMMAND test_tuning)

add_executable(test_param_sfo tests/test_param_sfo.cpp)
target_link_libraries(test_param_sfo PRIVATE umdplay_core)
target_compile_options(test_param_sfo PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_param_sfo COMMAND test_param_sfo)

add_executable(test_recent tests/test_recent.cpp)
target_link_libraries(test_recent PRIVATE umdplay_core)
target_compile_options(test_recent PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_recent COMMAND test_recent)
# Run in an isolated config dir so the real ~/.config/umdplay is never touched.
set_tests_properties(test_recent PROPERTIES
    ENVIRONMENT "UMDPLAY_CONFIG_DIR=${CMAKE_BINARY_DIR}/test_recent_cfg")

add_executable(test_playback tests/test_playback.cpp)
target_link_libraries(test_playback PRIVATE umdplay_core)
target_compile_options(test_playback PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_playback COMMAND test_playback)
# Decodes a real stream when an ISO is reachable; state checks otherwise.
set_tests_properties(test_playback PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ENVIRONMENT "UMD_ISO=${PROJECT_SOURCE_DIR}/test_umd.iso")

# Pure math: the SDL-free gesture recognizer and the aspect/zoom modes.
add_executable(test_gesture tests/test_gesture.cpp)
target_link_libraries(test_gesture PRIVATE umdplay_core)
target_compile_options(test_gesture PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_gesture COMMAND test_gesture)

# Pure parsers: PLAYLIST.UMD (tracks + chapter markers) and .RCO containers.
add_executable(test_playlist tests/test_playlist.cpp)
target_link_libraries(test_playlist PRIVATE umdplay_core)
target_compile_options(test_playlist PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_playlist COMMAND test_playlist)
# When a real ISO is reachable it also parses the disc's PLAYLIST.UMD/.RCO.
set_tests_properties(test_playlist PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ENVIRONMENT "UMD_ISO=${PROJECT_SOURCE_DIR}/test_umd.iso")

# The audio decoder test decodes a real stream when an ISO is reachable
# (UMD_ISO env var or ./test_umd.iso) and degrades to state checks otherwise.
set_tests_properties(test_audio_decoder PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ENVIRONMENT "UMD_ISO=${PROJECT_SOURCE_DIR}/test_umd.iso")

# Native PsmfDemuxer: synthetic MPEG-PS pack/PES parsing (0xE0 video + 0xBD
# ATRAC3+), 33-bit PTS decoding, variable 0F D0 48 <xx> stride and seek. No
# disc needed; a real ISO additionally exercises a live demux when present.
add_executable(test_psmf_demuxer tests/test_psmf_demuxer.cpp)
target_link_libraries(test_psmf_demuxer PRIVATE umdplay_core)
target_compile_options(test_psmf_demuxer PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_psmf_demuxer COMMAND test_psmf_demuxer)
set_tests_properties(test_psmf_demuxer PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ENVIRONMENT "UMD_ISO=${PROJECT_SOURCE_DIR}/test_umd.iso")

# DiscReader (ISO / CHD byte-range abstraction). Uses a committed CHD fixture;
# when a real ISO + chdman are present it also cross-checks IsoReader on a CHD.
add_executable(test_chd tests/test_chd.cpp)
target_link_libraries(test_chd PRIVATE umdplay_core)
target_compile_options(test_chd PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_chd COMMAND test_chd)
set_tests_properties(test_chd PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ENVIRONMENT "UMD_ISO=${PROJECT_SOURCE_DIR}/test_umd.iso;UMD_CHD_FIXTURE=${PROJECT_SOURCE_DIR}/tests/fixtures/pattern.chd")

# IsoReader disc-layout discovery: builds a synthetic ISO whose UMD_VIDEO/ and
# STREAM/ sit away from the legacy fixed LBAs and whose STREAM/ spans several
# sectors. Needs no real disc.
add_executable(test_iso_reader tests/test_iso_reader.cpp)
target_link_libraries(test_iso_reader PRIVATE umdplay_core)
target_compile_options(test_iso_reader PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_iso_reader COMMAND test_iso_reader)

# Procedural UI sound synthesis: asserts every cue is non-silent, finite and
# in-range, and exercises the SDL playback path under the dummy audio driver.
add_executable(test_ui_sfx tests/test_ui_sfx.cpp)
target_link_libraries(test_ui_sfx PRIVATE umdplay_ui_sfx)
target_compile_options(test_ui_sfx PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_ui_sfx COMMAND test_ui_sfx)
set_tests_properties(test_ui_sfx PROPERTIES
    ENVIRONMENT "SDL_AUDIODRIVER=dummy")

# Native H.264 decoder backend, deinterlacer and YUV converter. Decodes a
# committed synthetic Annex-B stream (no disc needed), checks the deinterlacer
# on a synthetic woven frame, and fuzzes truncated/corrupt NAL units.
add_executable(test_video_decoder tests/test_video_decoder.cpp)
target_link_libraries(test_video_decoder PRIVATE umdplay_core)
target_compile_options(test_video_decoder PRIVATE ${UMDPLAY_WARN_FLAGS})
add_test(NAME test_video_decoder COMMAND test_video_decoder)
set_tests_properties(test_video_decoder PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ENVIRONMENT "UMD_H264_FIXTURE=${PROJECT_SOURCE_DIR}/tests/fixtures/h264/testsrc2_720x480.h264")

if(UMDPLAY_AVERA_UI)
    add_subdirectory(libs/avera-ui)
    add_subdirectory(apps/avera-player)
endif()

install(TARGETS umdplay DESTINATION bin)

message(STATUS "UMDPlay ${PROJECT_VERSION} — WMC 10-foot UI (ImGui ${IMGUI_TAG})")
