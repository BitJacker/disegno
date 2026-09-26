cmake_minimum_required(VERSION 3.16)
project(Disegno VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Portable drawing engine (photo -> strokes), shared by the app and the tools.
add_library(dzcore STATIC
  src/core/image.cpp
  src/core/strokes.cpp
  src/core/pipeline.cpp
  src/core/render.cpp)
target_include_directories(dzcore PUBLIC src)

if(WIN32)
  enable_language(RC)

  add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
  target_compile_definitions(sqlite3 PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_THREADSAFE=1)

  add_executable(Disegno WIN32
    src/win/main.cpp
    src/win/mainwin.cpp
    src/win/winutil.cpp
    src/win/library.cpp
    src/win/imageio.cpp
    src/win/mouse.cpp
    src/win/hotkeys.cpp
    src/win/overlay.cpp
    src/win/preview.cpp
    src/win/app.rc)
  target_compile_definitions(Disegno PRIVATE UNICODE _UNICODE NOMINMAX)
  configure_file(src/win/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)
  target_include_directories(Disegno PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_link_libraries(Disegno PRIVATE dzcore sqlite3
    comctl32 gdi32 user32 shell32 ole32 oleaut32 uuid windowscodecs
    winmm wininet comdlg32 uxtheme dwmapi)

  if(MINGW)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/src/win")
    target_link_options(Disegno PRIVATE -municode -static -static-libgcc -static-libstdc++ -s)
  endif()
  if(MSVC)
    target_compile_options(Disegno PRIVATE /utf-8 /W3)
    target_compile_options(dzcore PRIVATE /utf-8)
    set_property(TARGET Disegno dzcore sqlite3 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()

  # Small Win32 canvas used by the automated end-to-end test.
  add_executable(testcanvas WIN32 tools/testcanvas.cpp)
  target_compile_definitions(testcanvas PRIVATE UNICODE _UNICODE)
  if(MINGW)
    target_link_options(testcanvas PRIVATE -municode -static -static-libgcc -static-libstdc++ -s)
  endif()
else()
  # Desktop tools: render what the app would draw into a PNG.
  add_executable(preview_cli tools/preview_cli.cpp)
  target_link_libraries(preview_cli PRIVATE dzcore)

  enable_testing()
  add_executable(core_tests tests/core_tests.cpp)
  target_link_libraries(core_tests PRIVATE dzcore)
  add_test(NAME core_tests COMMAND core_tests)
endif()
