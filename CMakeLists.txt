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

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BUILD_TESTING "Build the unit tests" ON)
option(MPGD_BUILD_TOOLS "Build the diagnostic tools (tng_probe, mpg_gui)" ON)
# mpg_gui uses the Win32 + OpenGL3 ImGui backends, so it only builds on Windows.
include(CMakeDependentOption)
cmake_dependent_option(MPGD_BUILD_GUI "Build the mpg_gui pendant test tool"
    ON "MPGD_BUILD_TOOLS;WIN32" OFF)
option(MPGD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (used by CI)" OFF)
option(MPGD_USE_SYSTEM_HIDAPI "Use an installed hidapi instead of fetching it" OFF)
# On Linux hidapi has two backends: libusb (talks to the device through
# libusb-1.0) and hidraw (the kernel hidraw driver, needs libudev).
set(MPGD_HIDAPI_BACKEND "libusb" CACHE STRING "hidapi backend on Linux: libusb or hidraw")
set_property(CACHE MPGD_HIDAPI_BACKEND PROPERTY STRINGS libusb hidraw)
if(NOT MPGD_HIDAPI_BACKEND MATCHES "^(libusb|hidraw)$")
    message(FATAL_ERROR "MPGD_HIDAPI_BACKEND must be libusb or hidraw, got '${MPGD_HIDAPI_BACKEND}'")
endif()

# Sanitizers for GCC/Clang, e.g. "address,undefined" or "thread". Applied to
# every target, dependencies included, which TSan needs to avoid false reports.
set(MPGD_SANITIZERS "" CACHE STRING "Comma-separated -fsanitize= list (GCC/Clang only)")
if(MPGD_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "MPGD_SANITIZERS is only supported with GCC or Clang")
    endif()
    add_compile_options(-fsanitize=${MPGD_SANITIZERS} -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${MPGD_SANITIZERS})
endif()

# Build a self-contained binary: pull dependencies in statically.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

include(FetchContent)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
set(FETCHCONTENT_QUIET OFF)
# hidapi 0.14 and yaml-cpp 0.8 declare cmake_minimum_required below 3.5,
# which CMake 4 rejects unless a policy floor is given.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

set(MPGD_FETCH_DEPS)
if(MPGD_USE_SYSTEM_HIDAPI)
    find_package(hidapi 0.14 REQUIRED)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The system package exports each backend as its own target.
        set(MPGD_HIDAPI_TARGET hidapi::${MPGD_HIDAPI_BACKEND})
    else()
        set(MPGD_HIDAPI_TARGET hidapi::hidapi)
    endif()
else()
    FetchContent_Declare(
        hidapi
        GIT_REPOSITORY https://github.com/libusb/hidapi.git
        GIT_TAG        hidapi-0.14.0
        GIT_SHALLOW    TRUE
    )
    if(MPGD_HIDAPI_BACKEND STREQUAL "hidraw")
        set(HIDAPI_WITH_HIDRAW ON CACHE BOOL "" FORCE)
        set(HIDAPI_WITH_LIBUSB OFF CACHE BOOL "" FORCE)
    else()
        set(HIDAPI_WITH_HIDRAW OFF CACHE BOOL "" FORCE)
        set(HIDAPI_WITH_LIBUSB ON CACHE BOOL "" FORCE)
    endif()
    list(APPEND MPGD_FETCH_DEPS hidapi)
    set(MPGD_HIDAPI_TARGET hidapi::hidapi)
endif()

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

FetchContent_MakeAvailable(${MPGD_FETCH_DEPS} yaml-cpp spdlog)

# Treat third-party headers as system headers so our stricter warning flags
# (and warnings-as-errors) do not fire inside them. FetchContent's SYSTEM
# option needs CMake 3.25, so do it by hand.
function(mpgd_mark_system_includes target)
    get_target_property(real ${target} ALIASED_TARGET)
    if(NOT real)
        set(real ${target})
    endif()
    get_target_property(dirs ${real} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
        set_target_properties(${real} PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
    endif()
endfunction()

foreach(dep ${MPGD_HIDAPI_TARGET} yaml-cpp spdlog::spdlog)
    mpgd_mark_system_includes(${dep})
endforeach()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
# Explicit source list: a new file has to be added here on purpose.
set(MPGD_SOURCES
    src/config/ConfigManager.cpp
    src/logic/ButtonHandler.cpp
    src/logic/DisplayUpdater.cpp
    src/logic/JogController.cpp
    src/planetcnc/StateReader.cpp
    src/planetcnc/TngApi.cpp
    src/threads/ButtonThread.cpp
    src/threads/DisplayThread.cpp
    src/threads/JogThread.cpp
    src/threads/UsbPollThread.cpp
    src/usb/HidDevice.cpp
    src/usb/PacketParser.cpp
    src/usb/XhcPendant.cpp
    src/utils/Daemon.cpp
    src/utils/Logger.cpp
    src/utils/Utf8.cpp
)

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

# Warning flags for every target built from this project's own sources.
function(mpgd_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(MPGD_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
        if(MPGD_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

mpgd_set_warnings(mpgd_core)

target_link_libraries(mpgd_core PUBLIC
    ${MPGD_HIDAPI_TARGET}
    yaml-cpp
    spdlog::spdlog
)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(mpgd "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
target_link_libraries(mpgd PRIVATE mpgd::core)
mpgd_set_warnings(mpgd)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
    enable_testing()

    function(mpgd_add_test name)
        add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE mpgd::core)
        mpgd_set_warnings(${name})
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    mpgd_add_test(test_packet_parser)
    mpgd_add_test(test_jog_controller)
    mpgd_add_test(test_display_updater)
    mpgd_add_test(test_config)
    mpgd_add_test(test_planetcnc)
    mpgd_add_test(test_xhc_pendant)
    mpgd_add_test(test_utf8)
    mpgd_add_test(test_usb_poll)

    target_compile_definitions(test_config PRIVATE
        MPGD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# ---------------------------------------------------------------------------
# TNG API probe (Stage 3 real-controller validation)
# ---------------------------------------------------------------------------
if(MPGD_BUILD_TOOLS)
    add_executable(tng_probe tools/tng_probe/main.cpp)
    target_link_libraries(tng_probe PRIVATE mpgd::core)
    mpgd_set_warnings(tng_probe)
endif()

# ---------------------------------------------------------------------------
# GUI test tool (Dear ImGui + Win32 + OpenGL3), Windows only
# ---------------------------------------------------------------------------
if(MPGD_BUILD_GUI)
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
    target_include_directories(imgui PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
    )

    # Third-party backends live in the imgui library so that our warning
    # flags (and warnings-as-errors) only apply to mpg_gui's own code.
    target_sources(imgui PRIVATE
        "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
    )

    add_executable(mpg_gui tools/mpg_gui/main.cpp)
    target_link_libraries(mpg_gui PRIVATE
        mpgd::core
        imgui
        opengl32 user32 gdi32 shell32 dwmapi imm32
    )
    set_target_properties(mpg_gui PROPERTIES WIN32_EXECUTABLE TRUE)
    mpgd_set_warnings(mpg_gui)
endif()
