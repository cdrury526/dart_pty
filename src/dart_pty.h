/**
 * dart_pty.h — Shared header for dart_pty native FFI plugin.
 *
 * Defines all public structs and function declarations consumed by:
 *   - dart_pty_unix.c (macOS posix_spawn + Linux fork/exec)
 *   - dart_pty_win.c  (Windows ConPTY)
 *   - ffigen          (Dart binding generation)
 *
 * Platform-specific internals (PtyHandle fields) are defined in the
 * respective .c files. This header only forward-declares PtyHandle
 * as an opaque type.
 */

#ifndef DART_PTY_H_
#define DART_PTY_H_

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Platform detection & export macro                                  */
/* ------------------------------------------------------------------ */

#if defined(_WIN32) || defined(_WIN64)
#define DART_PTY_WIN 1
#define DART_PTY_UNIX 0
#else
#define DART_PTY_WIN 0
#define DART_PTY_UNIX 1
#endif

#if DART_PTY_WIN
#define FFI_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FFI_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------ */
/*  Dart API DL — required for Dart_PostCObject_DL / Dart_PostInteger_DL */
/* ------------------------------------------------------------------ */

#include "include/dart_api_dl.h"

/* ------------------------------------------------------------------ */
/*  Log callback — optional structured logging from native code        */
/* ------------------------------------------------------------------ */

/** Log severity levels. */
enum PtyLogLevel {
    PTY_LOG_DEBUG = 0, /** Verbose diagnostic output. */
    PTY_LOG_INFO  = 1, /** Normal lifecycle events (spawn, resize, exit). */
    PTY_LOG_WARN  = 2, /** Recoverable issues (EINTR retries, EIO as EOF). */
    PTY_LOG_ERROR = 3  /** Unrecoverable failures (syscall errors). */
};

/**
 * Log callback signature.
 *
 * @param level     Severity (one of PtyLogLevel).
 * @param component Short tag identifying the subsystem (e.g. "spawn", "read").
 * @param message   Human-readable log message. Caller owns the string.
 */
typedef void (*pty_log_fn)(int level, const char *component, const char *message);

/**
 * Register a log callback. Pass NULL to disable logging.
 * Not thread-safe — call once at startup before any pty_create().
 */
FFI_PLUGIN_EXPORT void pty_set_log_callback(pty_log_fn callback);

/**
 * Set a Dart port for native log forwarding.
 *
 * When set, every native log message is posted to this port as a
 * Dart_CObject list: [int level, String component, String message].
 * This allows Dart to receive native log messages via ReceivePort
 * without needing a native callback.
 *
 * Pass ILLEGAL_PORT (0) to disable.
 *
 * @param port  Dart_Port from a ReceivePort.sendPort.nativePort.
 */
FFI_PLUGIN_EXPORT void pty_set_log_port(Dart_Port port);

/* ------------------------------------------------------------------ */
/*  PtyOptions — parameters for creating a new PTY session             */
/* ------------------------------------------------------------------ */

/**
 * Options passed to pty_create() to configure the child process
 * and initial terminal dimensions.
 */
typedef struct PtyOptions {
    /** Path to the executable to spawn (e.g. "/bin/bash"). */
    const char *executable;

    /**
     * NULL-terminated array of argument strings.
     * argv[0] is typically the program name.
     * May be NULL if no arguments are needed.
     */
    const char **arguments;

    /**
     * NULL-terminated array of "KEY=VALUE" environment strings.
     * May be NULL to inherit the parent environment.
     */
    const char **environment;

    /**
     * Working directory for the child process.
     * May be NULL to inherit the parent's cwd.
     */
    const char *working_directory;

    /** Initial number of terminal rows (height). Must be > 0. */
    uint16_t rows;

    /** Initial number of terminal columns (width). Must be > 0. */
    uint16_t cols;
} PtyOptions;

/* ------------------------------------------------------------------ */
/*  PtySize — terminal dimensions for resize operations                */
/* ------------------------------------------------------------------ */

/**
 * Terminal size descriptor, used by pty_resize().
 * pixel_width/pixel_height are optional hints (set to 0 if unknown).
 */
typedef struct PtySize {
    /** Number of character rows. */
    uint16_t rows;

    /** Number of character columns. */
    uint16_t cols;

    /** Pixel width of the terminal area (0 if unknown). */
    uint16_t pixel_width;

    /** Pixel height of the terminal area (0 if unknown). */
    uint16_t pixel_height;
} PtySize;

/* ------------------------------------------------------------------ */
/*  PtyHandle — opaque handle to a live PTY session                    */
/* ------------------------------------------------------------------ */

/**
 * Opaque handle representing a PTY session.
 *
 * Internals differ per platform:
 *   - Unix:    master fd, child pid, pthread_t for read/wait threads
 *   - Windows: ConPTY HPCON, pipe HANDLEs, HANDLE for read/wait threads
 *
 * Created by pty_create(), destroyed by pty_close().
 * All operations on a PtyHandle are thread-safe.
 */
typedef struct PtyHandle PtyHandle;

/* ------------------------------------------------------------------ */
/*  Dart API initialization                                            */
/* ------------------------------------------------------------------ */

/**
 * Initialize the Dart Native API for dynamic linking.
 *
 * MUST be called once from Dart before any other pty_* function.
 * Passes NativeApi.initializeApiDLData from the Dart side.
 *
 * @param data  Pointer from NativeApi.initializeApiDLData.
 * @return 0 on success, non-zero on failure.
 */
FFI_PLUGIN_EXPORT intptr_t pty_init_dart_api(void *data);

/* ------------------------------------------------------------------ */
/*  Core PTY lifecycle                                                 */
/* ------------------------------------------------------------------ */

/**
 * Create a new PTY and spawn a child process.
 *
 * Allocates a master/slave pair, configures terminal attributes,
 * spawns the child, and starts background read + wait threads that
 * post data to the given Dart ports.
 *
 * On Unix (macOS): uses posix_spawn() with POSIX_SPAWN_SETSID.
 * On Unix (Linux): uses fork() + setsid() + TIOCSCTTY + execvp().
 * On Windows:      uses CreatePseudoConsole + CreateProcessW.
 *
 * @param opts        Spawn options (executable, args, env, cwd, size).
 *                    Caller retains ownership; fields are copied internally.
 * @param output_port Dart ReceivePort for stdout data (Uint8List chunks).
 *                    The read thread posts Dart_CObject typed data here.
 * @param exit_port   Dart ReceivePort for exit code (int).
 *                    The wait thread posts a single integer here on exit.
 * @return Allocated PtyHandle on success, NULL on failure.
 *         On failure, call pty_error() for a description.
 */
FFI_PLUGIN_EXPORT PtyHandle *pty_create(
    const PtyOptions *opts,
    Dart_Port output_port,
    Dart_Port exit_port
);

/**
 * Write data to the PTY master (stdin of the child process).
 *
 * @param handle  Active PTY handle from pty_create().
 * @param data    Buffer of bytes to write.
 * @param len     Number of bytes in data.
 * @return Number of bytes written on success, -1 on error.
 *         On error, call pty_error() for a description.
 */
FFI_PLUGIN_EXPORT int pty_write(PtyHandle *handle, const uint8_t *data, int len);

/**
 * Resize the PTY terminal dimensions.
 *
 * Sends TIOCSWINSZ (Unix) or ResizePseudoConsole (Windows).
 * The child process receives SIGWINCH on Unix.
 *
 * @param handle  Active PTY handle.
 * @param rows    New row count (must be > 0).
 * @param cols    New column count (must be > 0).
 * @return 0 on success, -1 on error.
 *         On error, call pty_error() for a description.
 */
FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols);

/**
 * Get the process ID of the child.
 *
 * @param handle  Active PTY handle.
 * @return Child PID (Unix) or process ID (Windows), or -1 if invalid.
 */
FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle);

/**
 * Send a signal to the child process.
 *
 * On Unix:    calls kill(pid, signal). Common: SIGTERM (15), SIGKILL (9).
 * On Windows: calls TerminateProcess if signal is SIGKILL (9),
 *             otherwise sends Ctrl+C via GenerateConsoleCtrlEvent.
 *
 * @param handle  Active PTY handle.
 * @param signal  Signal number to send.
 * @return 0 on success, -1 on error.
 *         On error, call pty_error() for a description.
 */
FFI_PLUGIN_EXPORT int pty_kill(PtyHandle *handle, int signal);

/**
 * Close the PTY and release all associated resources.
 *
 * Closes master fd (Unix) or ClosePseudoConsole (Windows),
 * joins the read and wait threads, and frees the handle.
 *
 * After this call, the handle pointer is invalid.
 * Safe to call if the child has already exited.
 * Safe to call with NULL (no-op).
 *
 * @param handle  PTY handle to close, or NULL.
 */
FFI_PLUGIN_EXPORT void pty_close(PtyHandle *handle);

/* ------------------------------------------------------------------ */
/*  Error reporting — thread-local last error                          */
/* ------------------------------------------------------------------ */

/**
 * Get the last error message for the calling thread.
 *
 * Returns a pointer to a thread-local string describing the most
 * recent error from any pty_* function. The string is valid until
 * the next pty_* call on the same thread.
 *
 * @return Error message string, or NULL if no error has occurred.
 *         The caller must NOT free this pointer.
 */
FFI_PLUGIN_EXPORT const char *pty_error(void);

#endif /* DART_PTY_H_ */
