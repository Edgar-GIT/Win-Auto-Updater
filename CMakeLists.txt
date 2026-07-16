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
    src/restart.cpp
    src/self_delete.cpp
    resources/app.rc
)

target_include_directories(SingleUpdate PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/resources
)

set_source_files_properties(resources/app.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/resources/icon.ico"
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
)

set_target_properties(SingleUpdate PROPERTIES
    OUTPUT_NAME "SingleUpdate"
    WIN32_EXECUTABLE TRUE
)
