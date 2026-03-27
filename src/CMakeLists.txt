# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

project(dart_pty_library VERSION 0.0.1 LANGUAGES C)

# Unity build: dart_pty.c #includes dart_api_dl.c + platform .c files.
add_library(dart_pty SHARED
  "dart_pty.c"
)

set_target_properties(dart_pty PROPERTIES
  PUBLIC_HEADER dart_pty.h
  OUTPUT_NAME "dart_pty"
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
)

target_compile_definitions(dart_pty PUBLIC DART_SHARED_LIB)

# Include path for dart_api_dl.h and other Dart SDK headers in src/include/.
target_include_directories(dart_pty PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Platform-specific compiler and linker flags.
if(WIN32)
  # Windows: /W4 warning level.
  target_compile_options(dart_pty PRIVATE /W4)
else()
  # Unix (macOS + Linux): strict warnings, link pthread.
  target_compile_options(dart_pty PRIVATE -Wall -Wextra)
  target_link_libraries(dart_pty PRIVATE pthread)
endif()
