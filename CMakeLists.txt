cmake_minimum_required(VERSION 3.16)
project(UMDPlay VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Output directories ────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Include paths ─────────────────────────────────────────────────────────────
include_directories(${PROJECT_SOURCE_DIR}/include)

# ── Phase 1: ISO Parser (no external deps) ───────────────────────────────────
add_executable(umdplay
    src/main.cpp
    src/iso_reader.cpp
)

target_compile_options(umdplay PRIVATE
    -Wall -Wextra -O2
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS umdplay DESTINATION bin)

message(STATUS "UMDPlay ${PROJECT_VERSION} — Phase 1 build")
message(STATUS "  C++ standard : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
