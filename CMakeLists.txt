cmake_minimum_required(VERSION 3.16)
project(UMDPlay VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/third_party)

# --- FFmpeg + SDL2 (system) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
# libavfilter is optional: it powers yadif deinterlacing of 480i content. When
# absent the decoder passes interlaced frames through unchanged.
pkg_check_modules(AVFILTER libavfilter)
pkg_check_modules(SDL2 REQUIRED sdl2)

include_directories(
    ${AVFORMAT_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS} ${SWRESAMPLE_INCLUDE_DIRS} ${AVFILTER_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
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
target_compile_options(imgui_backends PRIVATE -w)   # third-party: silence its warnings

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
        target_compile_options(${_chdr_tgt} PRIVATE -w
            -fno-sanitize=undefined,address)
    endif()
endforeach()

# stb single-header libs (implementation lives in src/stb_impl.cpp)
add_library(stb_image STATIC src/stb_impl.cpp)
target_include_directories(stb_image PUBLIC ${PROJECT_SOURCE_DIR}/third_party)

# --- UMDPlay core (decoder + ISO reader + SFO parser), shared by app & tests ---
add_library(umdplay_core STATIC
    src/disc_reader.cpp
    src/iso_reader.cpp
    src/param_sfo.cpp
    src/decoder.cpp
    src/recent.cpp
    src/gesture.cpp
    src/aspect.cpp
    src/playlist_parser.cpp
    src/rco_parser.cpp
    src/vsmx_vm.cpp
    src/rco_menu.cpp
)
target_include_directories(umdplay_core PUBLIC
    ${chdr_SOURCE_DIR}/include
)
target_link_libraries(umdplay_core PUBLIC
    chdr-static
    ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES} ${SWRESAMPLE_LIBRARIES} ${AVFILTER_LIBRARIES}
)
if(AVFILTER_FOUND)
    target_compile_definitions(umdplay_core PUBLIC UMDPLAY_HAVE_AVFILTER=1)
    message(STATUS "UMDPlay: libavfilter found — yadif deinterlacing enabled")
else()
    message(STATUS "UMDPlay: libavfilter not found — interlaced frames pass through")
endif()
target_compile_options(umdplay_core PRIVATE -Wall -Wextra -Wpedantic)

# --- Player executable ---
add_executable(umdplay
    src/main.cpp
    src/ui_manager.cpp
    src/wmc_ui.cpp
    src/clip_export.cpp
    src/rco_texture.cpp
)
target_link_libraries(umdplay PRIVATE
    umdplay_core imgui_backends stb_image
    ${SDL2_LIBRARIES}
)
target_compile_options(umdplay PRIVATE -Wall -Wextra -Wpedantic)

# --- Tests ---
enable_testing()

add_executable(test_audio_decoder tests/test_audio_decoder.cpp)
target_link_libraries(test_audio_decoder PRIVATE umdplay_core)
target_compile_options(test_audio_decoder PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME test_audio_decoder COMMAND test_audio_decoder)

add_executable(test_param_sfo tests/test_param_sfo.cpp)
target_link_libraries(test_param_sfo PRIVATE umdplay_core)
target_compile_options(test_param_sfo PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME test_param_sfo COMMAND test_param_sfo)

add_executable(test_recent tests/test_recent.cpp)
target_link_libraries(test_recent PRIVATE umdplay_core)
target_compile_options(test_recent PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME test_recent COMMAND test_recent)
# Run in an isolated config dir so the real ~/.config/umdplay is never touched.
set_tests_properties(test_recent PROPERTIES
    ENVIRONMENT "UMDPLAY_CONFIG_DIR=${CMAKE_BINARY_DIR}/test_recent_cfg")

add_executable(test_playback tests/test_playback.cpp)
target_link_libraries(test_playback PRIVATE umdplay_core)
target_compile_options(test_playback PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME test_playback COMMAND test_playback)
# Decodes a real stream when an ISO is reachable; state checks otherwise.
set_tests_properties(test_playback PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ENVIRONMENT "UMD_ISO=${PROJECT_SOURCE_DIR}/test_umd.iso")

# Pure math: the SDL-free gesture recognizer and the aspect/zoom modes.
add_executable(test_gesture tests/test_gesture.cpp)
target_link_libraries(test_gesture PRIVATE umdplay_core)
target_compile_options(test_gesture PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME test_gesture COMMAND test_gesture)

# Pure parsers: PLAYLIST.UMD (tracks + chapter markers) and .RCO containers.
add_executable(test_playlist tests/test_playlist.cpp)
target_link_libraries(test_playlist PRIVATE umdplay_core)
target_compile_options(test_playlist PRIVATE -Wall -Wextra -Wpedantic)
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

# DiscReader (ISO / CHD byte-range abstraction). Uses a committed CHD fixture;
# when a real ISO + chdman are present it also cross-checks IsoReader on a CHD.
add_executable(test_chd tests/test_chd.cpp)
target_link_libraries(test_chd PRIVATE umdplay_core)
target_compile_options(test_chd PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME test_chd COMMAND test_chd)
set_tests_properties(test_chd PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ENVIRONMENT "UMD_ISO=${PROJECT_SOURCE_DIR}/test_umd.iso;UMD_CHD_FIXTURE=${PROJECT_SOURCE_DIR}/tests/fixtures/pattern.chd")

install(TARGETS umdplay DESTINATION bin)

message(STATUS "UMDPlay ${PROJECT_VERSION} — WMC 10-foot UI (ImGui ${IMGUI_TAG})")
