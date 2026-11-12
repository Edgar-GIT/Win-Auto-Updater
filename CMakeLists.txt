cmake_minimum_required(VERSION 3.20)

project(SingleUpdate VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(SingleUpdate WIN32
    src/main.cpp
    src/app.cpp
    src/logger.cpp
    src/scheduler.cpp
    src/update.cpp
    src/updatepicker.cpp
    src/restart.cpp
    src/cleanup.cpp
    src/state.cpp
    src/display.cpp
    src/driverpolicy.cpp
    src/drivers.cpp
    src/stress.cpp
    src/macro.cpp
    src/ui.cpp
    # Native test bench (beta) — self-contained, see native_tests/README.md
    native_tests/nt_core.cpp
    native_tests/ram_test.cpp
    native_tests/cpu_burner.cpp
    native_tests/disk_test.cpp
    native_tests/gpu_test.cpp
    native_tests/bench_window.cpp
    native_tests/nt_dev.cpp
    resources/app.rc
)

target_include_directories(SingleUpdate PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/resources
    ${CMAKE_SOURCE_DIR}/native_tests
)

set_source_files_properties(resources/app.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/resources/icon.ico;${CMAKE_SOURCE_DIR}/resources/app.manifest"
)

target_compile_definitions(SingleUpdate PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

target_compile_options(SingleUpdate PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -O2
)

target_link_options(SingleUpdate PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    -mwindows
    -municode
    -s
)

target_link_libraries(SingleUpdate PRIVATE
    ole32
    oleaut32
    uuid
    taskschd
    wininet
    advapi32
    shell32
    comctl32
    user32
    gdi32
    version
    secur32
    dwmapi
    gdiplus
    d3d11
    dxgi
    powrprof
)

# Developer aid: "SingleUpdate.exe --ui-snapshot <dir>" renders every screen to PNG.
option(SU_UI_SNAPSHOT "Build the UI snapshot renderer (dev only)" OFF)
if(SU_UI_SNAPSHOT)
    target_compile_definitions(SingleUpdate PRIVATE SU_UI_SNAPSHOT)
endif()

set_target_properties(SingleUpdate PROPERTIES
    OUTPUT_NAME "SingleUpdate"
    WIN32_EXECUTABLE TRUE
)
