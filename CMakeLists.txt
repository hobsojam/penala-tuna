cmake_minimum_required(VERSION 3.20)
project(penala-tuna LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /W3)
else()
    # Ensure UTF-8 source and execution charset so note-name strings (♭ ♯) encode correctly
    add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# Main executable — WIN32 suppresses the console window (uses WinMain)
add_executable(penala-tuna WIN32
    src/main.cpp
    src/tone.cpp
    src/capture.cpp
    src/pitch.cpp
    src/notes.cpp
)
target_link_libraries(penala-tuna PRIVATE winmm gdi32 user32)
target_include_directories(penala-tuna PRIVATE src)

# Tests (console executables, not WIN32)
enable_testing()

add_executable(test_notes tests/test_notes.cpp src/notes.cpp)
target_include_directories(test_notes PRIVATE src)
add_test(NAME notes COMMAND test_notes)

add_executable(test_pitch tests/test_pitch.cpp src/pitch.cpp)
target_include_directories(test_pitch PRIVATE src)
add_test(NAME pitch COMMAND test_pitch)
