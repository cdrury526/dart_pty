/**
 * dart_pty_win.c -- Windows ConPTY implementation for dart_pty.
 *
 * Uses CreatePseudoConsole (Windows 10 17763+) to spawn child processes
 * with a pseudo-terminal. Posts output/exit to Dart via Dart_PostCObject_DL.
 *
 * Key fixes vs flutter_pty: ClosePseudoConsole in pty_close, thread join,
 * pipe cleanup on all paths, thread-local errors, proper UTF-8->wide.
 */

#ifdef _WIN32

#include "dart_pty.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>

/* Fallback for older SDKs (< Windows SDK 17134). */
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
    ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
typedef VOID *HPCON;
#endif

/* ---- Logging --------------------------------------------------------- */

static pty_log_fn g_log_callback = NULL;
static Dart_Port g_log_port = 0; /* ILLEGAL_PORT */

FFI_PLUGIN_EXPORT void pty_set_log_callback(pty_log_fn callback) {
    g_log_callback = callback;
}

FFI_PLUGIN_EXPORT void pty_set_log_port(Dart_Port port) {
    g_log_port = port;
}

/** Post a log message to the Dart log port as [level, component, message]. */
static void post_log_to_dart(int level, const char *component, const char *msg) {
    if (g_log_port == 0) return;

    Dart_CObject c_level;
    c_level.type = Dart_CObject_kInt64;
    c_level.value.as_int64 = level;

    Dart_CObject c_comp;
    c_comp.type = Dart_CObject_kString;
    c_comp.value.as_string = (char *)component;

    Dart_CObject c_msg;
    c_msg.type = Dart_CObject_kString;
    c_msg.value.as_string = (char *)msg;

    Dart_CObject *elements[3] = { &c_level, &c_comp, &c_msg };
    Dart_CObject list;
    list.type = Dart_CObject_kArray;
    list.value.as_array.length = 3;
    list.value.as_array.values = elements;

    Dart_PostCObject_DL(g_log_port, &list);
}

static void pty_log(int level, const char *component, const char *fmt, ...) {
    if (!g_log_callback && g_log_port == 0) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log_callback) g_log_callback(level, component, buf);
    post_log_to_dart(level, component, buf);
}

/* ---- Thread-local error reporting ------------------------------------ */

static __declspec(thread) char tls_error[256] = {0};

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tls_error, sizeof(tls_error), fmt, ap);
    va_end(ap);
}

FFI_PLUGIN_EXPORT const char *pty_error(void) {
    return tls_error[0] ? tls_error : NULL;
}

/* ---- Dart API DL ----------------------------------------------------- */

FFI_PLUGIN_EXPORT intptr_t pty_init_dart_api(void *data) {
    intptr_t result = Dart_InitializeApiDL(data);
    if (result == 0)
        pty_log(PTY_LOG_INFO, "init", "Dart API DL initialized");
    else
        pty_log(PTY_LOG_ERROR, "init", "Dart_InitializeApiDL failed: %d",
                (int)result);
    return result;
}

/* ---- PtyHandle (Windows-specific) ------------------------------------ */

struct PtyHandle {
    HPCON hpc;            /* ConPTY pseudo-console handle. */
    HANDLE input_write;   /* Write end of input pipe (child stdin). */
    HANDLE output_read;   /* Read end of output pipe (child stdout). */
    HANDLE process;       /* Child process handle. */
    HANDLE thread;        /* Child main thread handle. */
    DWORD pid;            /* Child process ID. */
    HANDLE read_thread;   /* Background output reader thread. */
    HANDLE wait_thread;   /* Background exit-wait thread. */
    volatile LONG closed; /* Double-close guard. */
};

/* ---- Wide-string helpers --------------------------------------------- */

/** Convert UTF-8 to wide string. Caller must free. Returns NULL on failure. */
static LPWSTR utf8_to_wide(const char *utf8) {
    if (!utf8) return NULL;
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (needed <= 0) return NULL;
    LPWSTR wide = (LPWSTR)malloc(needed * sizeof(WCHAR));
    if (!wide) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, needed);
    return wide;
}

/** Build wide command line: "executable arg1 arg2". Caller must free. */
static LPWSTR build_cmdline(const char *executable, const char **arguments) {
    size_t total = strlen(executable) + 3;
    if (arguments) {
        for (int i = 0; arguments[i]; i++)
            total += strlen(arguments[i]) + 3;
    }

    char *cmd = (char *)malloc(total + 1);
    if (!cmd) return NULL;

    char *p = cmd;
    p += sprintf(p, "%s", executable);
    if (arguments) {
        for (int i = 0; arguments[i]; i++)
            p += sprintf(p, " %s", arguments[i]);
    }

    LPWSTR wide = utf8_to_wide(cmd);
    free(cmd);
    return wide;
}

/** Build wide environment block ("K=V\0K=V\0\0"). Caller must free. */
static LPWSTR build_env_block(const char **environment) {
    if (!environment) return NULL;

    size_t total = 1; /* final null terminator */
    for (int i = 0; environment[i]; i++)
        total += MultiByteToWideChar(CP_UTF8, 0, environment[i], -1, NULL, 0);

    LPWSTR block = (LPWSTR)malloc(total * sizeof(WCHAR));
    if (!block) return NULL;

    WCHAR *p = block;
    for (int i = 0; environment[i]; i++) {
        int len = MultiByteToWideChar(
            CP_UTF8, 0, environment[i], -1, p, (int)(total - (p - block)));
        p += len;
    }
    *p = L'\0';
    return block;
}

/* ---- Read thread ----------------------------------------------------- */

typedef struct ReadThreadCtx {
    HANDLE output_read;
    Dart_Port output_port;
    volatile LONG *closed_flag;
} ReadThreadCtx;

/** Reads from output pipe in a loop, posts Uint8List chunks to Dart. */
static DWORD WINAPI read_thread_proc(LPVOID arg) {
    ReadThreadCtx *ctx = (ReadThreadCtx *)arg;
    char buf[4096];
    DWORD bytes_read;

    pty_log(PTY_LOG_INFO, "read", "read thread started");

    while (!InterlockedCompareExchange(ctx->closed_flag, 0, 0)) {
        BOOL ok = ReadFile(ctx->output_read, buf, sizeof(buf),
                           &bytes_read, NULL);
        if (!ok || bytes_read == 0) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA)
                pty_log(PTY_LOG_INFO, "read", "pipe EOF (error %lu)", err);
            else if (!InterlockedCompareExchange(ctx->closed_flag, 0, 0))
                pty_log(PTY_LOG_WARN, "read", "ReadFile failed: %lu", err);
            break;
        }

        Dart_CObject obj;
        obj.type = Dart_CObject_kTypedData;
        obj.value.as_typed_data.type = Dart_TypedData_kUint8;
        obj.value.as_typed_data.length = bytes_read;
        obj.value.as_typed_data.values = (uint8_t *)buf;

        if (!Dart_PostCObject_DL(ctx->output_port, &obj)) {
            pty_log(PTY_LOG_WARN, "read", "Dart_PostCObject_DL failed");
            break;
        }
    }

    pty_log(PTY_LOG_INFO, "read", "read thread exiting");
    free(ctx);
    return 0;
}

/* ---- Wait thread ----------------------------------------------------- */

typedef struct WaitThreadCtx {
    HANDLE process;
    Dart_Port exit_port;
} WaitThreadCtx;

/** Waits for child exit, posts exit code integer to Dart. */
static DWORD WINAPI wait_thread_proc(LPVOID arg) {
    WaitThreadCtx *ctx = (WaitThreadCtx *)arg;
    pty_log(PTY_LOG_INFO, "wait", "wait thread started");

    WaitForSingleObject(ctx->process, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(ctx->process, &exit_code);
    pty_log(PTY_LOG_INFO, "wait", "child exited with code %lu", exit_code);

    Dart_PostInteger_DL(ctx->exit_port, (int64_t)exit_code);
    free(ctx);
    return 0;
}

/* ---- pty_create ------------------------------------------------------ */

FFI_PLUGIN_EXPORT PtyHandle *pty_create(
    const PtyOptions *opts, Dart_Port output_port, Dart_Port exit_port)
{
    if (!opts || !opts->executable) {
        set_error("pty_create: opts and executable are required");
        return NULL;
    }
    if (opts->rows == 0 || opts->cols == 0) {
        set_error("pty_create: rows and cols must be > 0");
        return NULL;
    }

    pty_log(PTY_LOG_INFO, "spawn", "creating ConPTY for '%s' (%dx%d)",
            opts->executable, opts->cols, opts->rows);

    HANDLE input_read = NULL, input_write = NULL;
    HANDLE output_read = NULL, output_write = NULL;
    HPCON hpc = NULL;
    LPWSTR cmdline = NULL, env_block = NULL, cwd_wide = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attr_list = NULL;

    /* 1. Create pipes for ConPTY I/O. */
    if (!CreatePipe(&input_read, &input_write, NULL, 0)) {
        set_error("CreatePipe (input) failed: %lu", GetLastError());
        pty_log(PTY_LOG_ERROR, "spawn", "%s", tls_error);
        goto fail;
    }
    if (!CreatePipe(&output_read, &output_write, NULL, 0)) {
        set_error("CreatePipe (output) failed: %lu", GetLastError());
        pty_log(PTY_LOG_ERROR, "spawn", "%s", tls_error);
        goto fail;
    }

    /* 2. Create the pseudo-console. */
    COORD size = { (SHORT)opts->cols, (SHORT)opts->rows };
    HRESULT hr = CreatePseudoConsole(size, input_read, output_write, 0, &hpc);
    if (FAILED(hr)) {
        set_error("CreatePseudoConsole failed: HRESULT 0x%08lX", hr);
        pty_log(PTY_LOG_ERROR, "spawn", "%s", tls_error);
        goto fail;
    }

    /* 3. Close pipe ends now owned by ConPTY. */
    CloseHandle(input_read);   input_read = NULL;
    CloseHandle(output_write); output_write = NULL;

    /* 4. Set up proc thread attribute list with ConPTY handle. */
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    attr_list = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(
        GetProcessHeap(), 0, attr_size);
    if (!attr_list) {
        set_error("HeapAlloc for attribute list failed");
        pty_log(PTY_LOG_ERROR, "spawn", "HeapAlloc for attribute list failed");
        goto fail;
    }
    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
        DWORD err = GetLastError();
        set_error("InitializeProcThreadAttributeList failed: %lu", err);
        pty_log(PTY_LOG_ERROR, "spawn",
                "InitializeProcThreadAttributeList: error %lu", err);
        goto fail;
    }
    if (!UpdateProcThreadAttribute(attr_list, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            hpc, sizeof(HPCON), NULL, NULL)) {
        DWORD err = GetLastError();
        set_error("UpdateProcThreadAttribute failed: %lu", err);
        pty_log(PTY_LOG_ERROR, "spawn",
                "UpdateProcThreadAttribute: error %lu", err);
        goto fail;
    }

    /* 5. Build wide strings for command, env, cwd. */
    cmdline = build_cmdline(opts->executable, opts->arguments);
    if (!cmdline) { set_error("Failed to build command line"); goto fail; }
    env_block = build_env_block(opts->environment);
    if (opts->working_directory)
        cwd_wide = utf8_to_wide(opts->working_directory);

    /* 6. Create the child process. */
    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    si.lpAttributeList = attr_list;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    DWORD flags = EXTENDED_STARTUPINFO_PRESENT;
    if (env_block) flags |= CREATE_UNICODE_ENVIRONMENT;

    BOOL ok = CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, flags,
                             env_block, cwd_wide, &si.StartupInfo, &pi);
    free(cmdline);   cmdline = NULL;
    free(env_block); env_block = NULL;
    free(cwd_wide);  cwd_wide = NULL;

    if (!ok) {
        set_error("CreateProcessW failed: %lu", GetLastError());
        pty_log(PTY_LOG_ERROR, "spawn", "%s", tls_error);
        goto fail;
    }

    pty_log(PTY_LOG_INFO, "spawn", "child PID %lu", pi.dwProcessId);

    /* 7. Clean up attribute list. */
    DeleteProcThreadAttributeList(attr_list);
    HeapFree(GetProcessHeap(), 0, attr_list);
    attr_list = NULL;

    /* 8. Allocate PtyHandle. */
    PtyHandle *handle = (PtyHandle *)calloc(1, sizeof(PtyHandle));
    if (!handle) {
        set_error("Failed to allocate PtyHandle");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ClosePseudoConsole(hpc);
        CloseHandle(input_write);
        CloseHandle(output_read);
        return NULL;
    }
    handle->hpc = hpc;
    handle->input_write = input_write;
    handle->output_read = output_read;
    handle->process = pi.hProcess;
    handle->thread = pi.hThread;
    handle->pid = pi.dwProcessId;

    /* 9. Start read thread. */
    ReadThreadCtx *rctx = (ReadThreadCtx *)malloc(sizeof(ReadThreadCtx));
    if (!rctx) {
        set_error("Failed to allocate ReadThreadCtx");
        pty_close(handle);
        return NULL;
    }
    rctx->output_read = output_read;
    rctx->output_port = output_port;
    rctx->closed_flag = &handle->closed;
    handle->read_thread = CreateThread(NULL, 0, read_thread_proc, rctx, 0, NULL);
    if (!handle->read_thread) {
        set_error("CreateThread (read) failed: %lu", GetLastError());
        free(rctx);
        pty_close(handle);
        return NULL;
    }

    /* 10. Start wait thread. */
    WaitThreadCtx *wctx = (WaitThreadCtx *)malloc(sizeof(WaitThreadCtx));
    if (!wctx) {
        set_error("Failed to allocate WaitThreadCtx");
        pty_close(handle);
        return NULL;
    }
    wctx->process = pi.hProcess;
    wctx->exit_port = exit_port;
    handle->wait_thread = CreateThread(NULL, 0, wait_thread_proc, wctx, 0, NULL);
    if (!handle->wait_thread) {
        set_error("CreateThread (wait) failed: %lu", GetLastError());
        free(wctx);
        pty_close(handle);
        return NULL;
    }

    pty_log(PTY_LOG_INFO, "spawn", "ConPTY session ready (PID %lu)", handle->pid);
    return handle;

fail:
    if (attr_list) {
        DeleteProcThreadAttributeList(attr_list);
        HeapFree(GetProcessHeap(), 0, attr_list);
    }
    if (hpc) ClosePseudoConsole(hpc);
    if (input_read) CloseHandle(input_read);
    if (input_write) CloseHandle(input_write);
    if (output_read) CloseHandle(output_read);
    if (output_write) CloseHandle(output_write);
    free(cmdline);
    free(env_block);
    free(cwd_wide);
    return NULL;
}

/* ---- pty_write ------------------------------------------------------- */

FFI_PLUGIN_EXPORT int pty_write(PtyHandle *handle, const uint8_t *data,
                                int len) {
    if (!handle || !data || len <= 0) {
        set_error("pty_write: invalid arguments");
        return -1;
    }
    DWORD written = 0;
    if (!WriteFile(handle->input_write, data, (DWORD)len, &written, NULL)) {
        DWORD err = GetLastError();
        set_error("pty_write: WriteFile failed: %lu", err);
        pty_log(PTY_LOG_ERROR, "write", "WriteFile failed: %lu", err);
        return -1;
    }
    return (int)written;
}

/* ---- pty_resize ------------------------------------------------------ */

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols) {
    if (!handle) { set_error("pty_resize: handle is NULL"); return -1; }
    if (rows <= 0 || cols <= 0) {
        set_error("pty_resize: rows and cols must be > 0");
        return -1;
    }
    COORD sz = { (SHORT)cols, (SHORT)rows };
    HRESULT hr = ResizePseudoConsole(handle->hpc, sz);
    if (FAILED(hr)) {
        set_error("ResizePseudoConsole failed: HRESULT 0x%08lX", hr);
        pty_log(PTY_LOG_ERROR, "resize", "%s", tls_error);
        return -1;
    }
    pty_log(PTY_LOG_INFO, "resize", "resized to %dx%d", cols, rows);
    return 0;
}

/* ---- pty_getpid ------------------------------------------------------ */

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle) {
    if (!handle) return -1;
    return (int)handle->pid;
}

/* ---- pty_kill -------------------------------------------------------- */

FFI_PLUGIN_EXPORT int pty_kill(PtyHandle *handle, int signal) {
    if (!handle) { set_error("pty_kill: handle is NULL"); return -1; }

    /* SIGKILL (9) -> TerminateProcess; others -> Ctrl+C via input pipe. */
    if (signal == 9) {
        if (!TerminateProcess(handle->process, 1)) {
            DWORD err = GetLastError();
            set_error("TerminateProcess failed: %lu", err);
            pty_log(PTY_LOG_ERROR, "kill", "%s", tls_error);
            return -1;
        }
        pty_log(PTY_LOG_INFO, "kill", "TerminateProcess PID %lu", handle->pid);
    } else {
        const char ctrl_c = '\x03';
        DWORD written;
        if (!WriteFile(handle->input_write, &ctrl_c, 1, &written, NULL)) {
            DWORD err = GetLastError();
            set_error("pty_kill: Ctrl+C write failed: %lu", err);
            pty_log(PTY_LOG_WARN, "kill", "%s", tls_error);
            return -1;
        }
        pty_log(PTY_LOG_INFO, "kill", "Ctrl+C to PID %lu (sig %d)",
                handle->pid, signal);
    }
    return 0;
}

/* ---- pty_close ------------------------------------------------------- */

FFI_PLUGIN_EXPORT void pty_close(PtyHandle *handle) {
    if (!handle) return;
    if (InterlockedCompareExchange(&handle->closed, 1, 0) != 0) return;

    pty_log(PTY_LOG_INFO, "close", "closing ConPTY (PID %lu)", handle->pid);

    /*
     * CRITICAL: ClosePseudoConsole FIRST. This breaks the output pipe
     * (unblocking the read thread) and signals the child to exit.
     * flutter_pty omits this entirely -- resource leak.
     * wezterm does this in PsuedoCon::drop.
     */
    if (handle->hpc) {
        ClosePseudoConsole(handle->hpc);
        handle->hpc = NULL;
    }

    /* Close input pipe to unblock pending writes. */
    if (handle->input_write) {
        CloseHandle(handle->input_write);
        handle->input_write = NULL;
    }

    /* Join read thread (exits on broken pipe from ClosePseudoConsole). */
    if (handle->read_thread) {
        WaitForSingleObject(handle->read_thread, 5000);
        CloseHandle(handle->read_thread);
        handle->read_thread = NULL;
    }

    /* Close output pipe after read thread is done. */
    if (handle->output_read) {
        CloseHandle(handle->output_read);
        handle->output_read = NULL;
    }

    /* Join wait thread (unblocks when child exits). */
    if (handle->wait_thread) {
        WaitForSingleObject(handle->wait_thread, 5000);
        CloseHandle(handle->wait_thread);
        handle->wait_thread = NULL;
    }

    /* Close process and thread handles. */
    if (handle->process) { CloseHandle(handle->process); handle->process = NULL; }
    if (handle->thread)  { CloseHandle(handle->thread);  handle->thread = NULL; }

    pty_log(PTY_LOG_INFO, "close", "ConPTY session closed");
    free(handle);
}

#endif /* _WIN32 */
