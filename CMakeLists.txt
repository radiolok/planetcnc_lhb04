cmake_minimum_required(VERSION 3.22)

project(mpgd
    VERSION 0.1.0
    DESCRIPTION "XHC LHB04 pendant <-> PlanetCNC TNG adapter daemon"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Build a self-contained binary: pull dependencies in statically.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

include(FetchContent)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
    hidapi
    GIT_REPOSITORY https://github.com/libusb/hidapi.git
    GIT_TAG        hidapi-0.14.0
    GIT_SHALLOW    TRUE
)
set(HIDAPI_WITH_HIDRAW OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
    GIT_SHALLOW    TRUE
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE
)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(hidapi yaml-cpp spdlog)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MPGD_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(FILTER MPGD_SOURCES EXCLUDE REGEX "src[\\/]main\\.cpp$")

add_library(mpgd_core STATIC ${MPGD_SOURCES})
add_library(mpgd::core ALIAS mpgd_core)

target_include_directories(mpgd_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(mpgd_core PUBLIC
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
    YAML_CPP_STATIC_DEFINE
)

if(WIN32)
    target_compile_definitions(mpgd_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_compile_options(mpgd_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

target_link_libraries(mpgd_core PUBLIC
    hidapi::hidapi
    yaml-cpp
    spdlog::spdlog
)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(mpgd "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
target_link_libraries(mpgd PRIVATE mpgd::core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(test_packet_parser tests/test_packet_parser.cpp)
target_link_libraries(test_packet_parser PRIVATE mpgd::core)
add_test(NAME test_packet_parser COMMAND test_packet_parser)

add_executable(test_jog_controller tests/test_jog_controller.cpp)
target_link_libraries(test_jog_controller PRIVATE mpgd::core)
add_test(NAME test_jog_controller COMMAND test_jog_controller)

add_executable(test_display_updater tests/test_display_updater.cpp)
target_link_libraries(test_display_updater PRIVATE mpgd::core)
add_test(NAME test_display_updater COMMAND test_display_updater)

add_executable(test_planetcnc tests/test_planetcnc.cpp)
target_link_libraries(test_planetcnc PRIVATE mpgd::core)
add_test(NAME test_planetcnc COMMAND test_planetcnc)

# ---------------------------------------------------------------------------
# GUI test tool (Dear ImGui + Win32 + OpenGL3)
# ---------------------------------------------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.9
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# Dear ImGui v1.90.9 ships no root CMakeLists.txt, so build the core library
# from its sources directly.
add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
)
target_include_directories(imgui PUBLIC "${imgui_SOURCE_DIR}")

add_executable(mpg_gui
    tools/mpg_gui/main.cpp
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
)
target_include_directories(mpg_gui PRIVATE
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(mpg_gui PRIVATE
    mpgd::core
    imgui
    opengl32 user32 gdi32 shell32 dwmapi imm32
)
set_target_properties(mpg_gui PROPERTIES WIN32_EXECUTABLE TRUE)

# ---------------------------------------------------------------------------
# TNG API probe (Stage 3 real-controller validation)
# ---------------------------------------------------------------------------
add_executable(tng_probe tools/tng_probe/main.cpp)
target_link_libraries(tng_probe PRIVATE mpgd::core)
