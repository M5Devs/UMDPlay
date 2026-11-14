cmake_minimum_required(VERSION 3.16)
project(UMDPlay VERSION 0.4.0 LANGUAGES CXX)

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

# stb single-header libs (implementation lives in src/stb_impl.cpp)
add_library(stb_image STATIC src/stb_impl.cpp)
target_include_directories(stb_image PUBLIC ${PROJECT_SOURCE_DIR}/third_party)

# --- UMDPlay core (decoder + ISO reader + SFO parser), shared by app & tests ---
add_library(umdplay_core STATIC
    src/iso_reader.cpp
    src/param_sfo.cpp
    src/decoder.cpp
    src/recent.cpp
    src/gesture.cpp
    src/aspect.cpp
    src/playlist_parser.cpp
    src/rco_parser.cpp
    src/vsmx_vm.cpp
)
target_link_libraries(umdplay_core PUBLIC
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

install(TARGETS umdplay DESTINATION bin)

message(STATUS "UMDPlay ${PROJECT_VERSION} — WMC 10-foot UI (ImGui ${IMGUI_TAG})")
