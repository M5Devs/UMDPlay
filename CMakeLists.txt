cmake_minimum_required(VERSION 3.16)
project(UMDPlay VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(${PROJECT_SOURCE_DIR}/include)

# البحث عن مكاتب FFmpeg
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)

include_directories(
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
)

# بناء مشغل UMDPlay (Phase 2 Engine)
add_executable(umdplay
    src/main.cpp
    src/iso_reader.cpp
    src/decoder.cpp
)

target_link_libraries(umdplay PRIVATE
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
)

target_compile_options(umdplay PRIVATE -Wall -Wextra -O2)

enable_testing()

add_executable(test_audio_decoder
    tests/test_audio_decoder.cpp
    src/iso_reader.cpp
    src/decoder.cpp
)

target_link_libraries(test_audio_decoder PRIVATE
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
)

target_compile_options(test_audio_decoder PRIVATE -Wall -Wextra -O2)

add_test(NAME test_audio_decoder COMMAND test_audio_decoder)

install(TARGETS umdplay DESTINATION bin)

message(STATUS "UMDPlay ${PROJECT_VERSION} — Ready for ISO playback")
