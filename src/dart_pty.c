/**
 * dart_pty.c -- Unity build file for dart_pty native FFI plugin.
 *
 * Includes dart_api_dl.c (Dart FFI support) and the platform-specific
 * PTY implementation. CMake and podspec both compile only this file.
 */
#include "dart_pty.h"

#include "include/dart_api_dl.c"

#if defined(_WIN32) || defined(_WIN64)
#include "dart_pty_win.c"
#else
#include "dart_pty_unix.c"
#endif
