# ZX-6R — Telemetría OBD2 (versión C++)
#   cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release && cmake --build cpp/build -j
# Raspberry Pi OS: sudo apt install build-essential cmake git libsqlite3-dev
#   libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev
cmake_minimum_required(VERSION 3.20)
project(zx6r LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo de build" FORCE)
endif()

# ── raylib 5.5 (se baja y se compila con el proyecto) ──
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
# Raspberry Pi: Mesa (V3D) no da OpenGL 3.3 core, que es el default de raylib
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64)")
  set(OPENGL_VERSION "2.1" CACHE STRING "Versión de OpenGL para raylib")
endif()
FetchContent_Declare(raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(raylib)

find_package(SQLite3 REQUIRED)   # macOS: viene en el SDK; Raspberry Pi: libsqlite3-dev
find_package(Threads REQUIRED)
if(TARGET SQLite3::SQLite3)      # CMake >= 4.3
  set(ZX6R_SQLITE SQLite3::SQLite3)
else()
  set(ZX6R_SQLITE SQLite::SQLite3)
endif()

# ── visor web embebido: web/index.html -> header con un raw string literal ──
set(ZX6R_WEB_INDEX "${CMAKE_CURRENT_SOURCE_DIR}/../web/index.html")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ZX6R_WEB_INDEX}")
file(READ "${ZX6R_WEB_INDEX}" ZX6R_INDEX_HTML)
configure_file(src/web_index.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/web_index.hpp" @ONLY)

add_executable(zx6r
  src/main.cpp
  src/obd.cpp
  src/store.cpp
  src/web.cpp
  src/ui.cpp)
target_include_directories(zx6r PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(zx6r PRIVATE raylib ${ZX6R_SQLITE} Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(zx6r PRIVATE -Wall -Wextra)
endif()
