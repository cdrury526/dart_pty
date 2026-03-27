/**
 * dart_pty_unix.c — Unix PTY implementation (macOS + Linux).
 *
 * macOS: posix_spawn() with POSIX_SPAWN_SETSID (node-pty pattern).
 * Linux: fork() + setsid() + TIOCSCTTY + execvp().
 * Both paths share read/wait threads, write, resize, kill, close.
 */
#if !defined(_WIN32) && !defined(_WIN64)

#include "dart_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <spawn.h>
#include <util.h>
#else
#include <pty.h>
#endif

/* --- PtyHandle: internal state for a live PTY session --- */

struct PtyHandle {
    int master_fd;
    pid_t child_pid;
    pthread_t read_tid;
    pthread_t wait_tid;
    Dart_Port output_port;
    Dart_Port exit_port;
    volatile int closed;
};

/* --- Thread-local error reporting --- */

static __thread char tls_error_buf[512];
static __thread int tls_error_set = 0;

/** Set thread-local error with printf formatting. */
static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tls_error_buf, sizeof(tls_error_buf), fmt, ap);
    va_end(ap);
    tls_error_set = 1;
}

/** Set thread-local error with errno context. */
static void set_error_errno(const char *func) {
    int saved = errno;
    snprintf(tls_error_buf, sizeof(tls_error_buf), "%s: %s (errno=%d)",
             func, strerror(saved), saved);
    tls_error_set = 1;
}

FFI_PLUGIN_EXPORT const char *pty_error(void) {
    return tls_error_set ? tls_error_buf : NULL;
}

/* --- Logging --- */

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

/** Log a formatted message via callback and/or Dart port. */
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

static int env_key_matches(const char *entry, const char *key) {
    const size_t key_len = strlen(key);
    return strncmp(entry, key, key_len) == 0 && entry[key_len] == '=';
}

static void log_spawn_environment(char *const *envp) {
    if (!envp) {
        pty_log(PTY_LOG_INFO, "spawn", "Environment: inheriting parent");
        return;
    }

    static const char *keys[] = {
        "TERM",
        "COLORTERM",
        "TERM_PROGRAM",
        "TERM_PROGRAM_VERSION",
        "FORCE_HYPERLINK",
        "COLORFGBG",
        "TERM_PROGRAM_BACKGROUND",
        "LANG",
        "LC_ALL",
        "CI",
        "TEAMCITY_VERSION",
        "VTE_VERSION",
    };

    int entry_count = 0;
    for (char *const *cursor = envp; *cursor; cursor++) {
        entry_count++;
    }
    pty_log(PTY_LOG_INFO, "spawn", "Environment entries=%d", entry_count);

    for (char *const *cursor = envp; *cursor; cursor++) {
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (env_key_matches(*cursor, keys[i])) {
                pty_log(PTY_LOG_INFO, "spawn", "env %s", *cursor);
                break;
            }
        }
    }
}

/* --- Dart API initialization --- */

FFI_PLUGIN_EXPORT intptr_t pty_init_dart_api(void *data) {
    intptr_t result = Dart_InitializeApiDL(data);
    if (result == 0)
        pty_log(PTY_LOG_INFO, "init", "Dart API DL initialized");
    else
        pty_log(PTY_LOG_ERROR, "init", "Dart_InitializeApiDL failed: %d",
                (int)result);
    return result;
}

/* --- Terminal configuration (node-pty style defaults) --- */

/** Configure termios to match a standard interactive terminal. */
static void configure_termios(struct termios *t) {
    memset(t, 0, sizeof(*t));
    t->c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT;
#ifdef IUTF8
    t->c_iflag |= IUTF8;
#endif
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CREAD | CS8 | HUPCL;
    t->c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOK
               | ECHOKE | ECHOCTL;
    t->c_cc[VEOF] = 4;  t->c_cc[VEOL] = 255;  t->c_cc[VEOL2] = 255;
    t->c_cc[VERASE] = 0x7f;  t->c_cc[VWERASE] = 23;  t->c_cc[VKILL] = 21;
    t->c_cc[VREPRINT] = 18;  t->c_cc[VINTR] = 3;  t->c_cc[VQUIT] = 0x1c;
    t->c_cc[VSUSP] = 26;  t->c_cc[VSTART] = 17;  t->c_cc[VSTOP] = 19;
    t->c_cc[VLNEXT] = 22;  t->c_cc[VDISCARD] = 15;
    t->c_cc[VMIN] = 1;  t->c_cc[VTIME] = 0;
#ifdef __APPLE__
    t->c_cc[VDSUSP] = 25;  t->c_cc[VSTATUS] = 20;
#endif
    cfsetispeed(t, B38400);
    cfsetospeed(t, B38400);
}

/* --- Read thread: posts bytes to Dart via Dart_PostCObject_DL --- */

typedef struct {
    int fd;
    Dart_Port port;
    volatile int *closed_flag;
} ReadThreadCtx;

/** Read loop: reads from master fd, posts Uint8List chunks to Dart. */
static void *read_thread_fn(void *arg) {
    ReadThreadCtx *ctx = (ReadThreadCtx *)arg;
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = read(ctx->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            /* EIO on master = slave closed (wezterm pattern). Treat as EOF. */
            if (errno == EIO) {
                pty_log(PTY_LOG_WARN, "read",
                        "EIO on fd %d — treating as EOF", ctx->fd);
                break;
            }
            pty_log(PTY_LOG_ERROR, "read", "read(fd=%d): %s (errno=%d)",
                    ctx->fd, strerror(errno), errno);
            break;
        }
        if (n == 0) break;
        Dart_CObject cobject;
        cobject.type = Dart_CObject_kTypedData;
        cobject.value.as_typed_data.type = Dart_TypedData_kUint8;
        cobject.value.as_typed_data.length = n;
        cobject.value.as_typed_data.values = buf;
        Dart_PostCObject_DL(ctx->port, &cobject);
    }
    pty_log(PTY_LOG_INFO, "read", "Read thread exiting for fd %d", ctx->fd);
    free(ctx);
    return NULL;
}

/* --- Wait thread: posts exit code to Dart via Dart_PostInteger_DL --- */

typedef struct {
    pid_t pid;
    Dart_Port port;
} WaitThreadCtx;

/** Wait for child exit and post exit code to Dart. */
static void *wait_thread_fn(void *arg) {
    WaitThreadCtx *ctx = (WaitThreadCtx *)arg;
    int status;
    pid_t result;
    do {
        result = waitpid(ctx->pid, &status, 0);
    } while (result == -1 && errno == EINTR);

    int exit_code;
    if (result == -1) {
        pty_log(PTY_LOG_ERROR, "wait", "waitpid(%d): %s (errno=%d)",
                ctx->pid, strerror(errno), errno);
        exit_code = -1;
    } else if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
        pty_log(PTY_LOG_INFO, "wait", "Child %d exited normally, code=%d",
                ctx->pid, exit_code);
    } else if (WIFSIGNALED(status)) {
        exit_code = -WTERMSIG(status); /* Negative = signal death */
        pty_log(PTY_LOG_INFO, "wait",
                "Child %d killed by signal %d (status=0x%x)",
                ctx->pid, WTERMSIG(status), status);
    } else {
        exit_code = -1;
        pty_log(PTY_LOG_WARN, "wait",
                "Child %d: unexpected status 0x%x", ctx->pid, status);
    }
    Dart_PostInteger_DL(ctx->port, exit_code);
    free(ctx);
    return NULL;
}

/* --- PTY allocation: master/slave pair with terminal attrs set --- */

/** Allocate master/slave PTY pair. Returns 0 on success, -1 on failure. */
static int allocate_pty(int *master_out, int *slave_out,
                        const struct termios *termp,
                        const struct winsize *winp) {
    int master = posix_openpt(O_RDWR);
    if (master == -1) {
        set_error_errno("posix_openpt");
        pty_log(PTY_LOG_ERROR, "pty", "posix_openpt: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }
    if (grantpt(master) == -1) {
        set_error_errno("grantpt");
        pty_log(PTY_LOG_ERROR, "pty", "grantpt(fd=%d): %s (errno=%d)", master, strerror(errno), errno);
        close(master); return -1;
    }
    if (unlockpt(master) == -1) {
        set_error_errno("unlockpt");
        pty_log(PTY_LOG_ERROR, "pty", "unlockpt(fd=%d): %s (errno=%d)", master, strerror(errno), errno);
        close(master); return -1;
    }

    /* Get slave name: TIOCPTYGNAME (thread-safe) on macOS, ptsname on Linux. */
    char slave_name[256];
#ifdef __APPLE__
    if (ioctl(master, TIOCPTYGNAME, slave_name) == -1) {
        set_error_errno("ioctl(TIOCPTYGNAME)");
        pty_log(PTY_LOG_ERROR, "pty", "TIOCPTYGNAME(fd=%d): %s (errno=%d)", master, strerror(errno), errno);
        close(master); return -1;
    }
#else
    {
        char *name = ptsname(master);
        if (!name) {
            set_error_errno("ptsname");
            pty_log(PTY_LOG_ERROR, "pty", "ptsname(fd=%d): %s (errno=%d)", master, strerror(errno), errno);
            close(master); return -1;
        }
        strncpy(slave_name, name, sizeof(slave_name) - 1);
        slave_name[sizeof(slave_name) - 1] = '\0';
    }
#endif

    pty_log(PTY_LOG_DEBUG, "pty", "Allocated PTY: master=%d slave=%s",
            master, slave_name);

    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave == -1) {
        set_error_errno("open(slave)");
        pty_log(PTY_LOG_ERROR, "pty", "open(%s): %s (errno=%d)", slave_name, strerror(errno), errno);
        close(master); return -1;
    }
    if (termp && tcsetattr(slave, TCSANOW, termp) == -1) {
        set_error_errno("tcsetattr");
        pty_log(PTY_LOG_ERROR, "pty", "tcsetattr(fd=%d): %s (errno=%d)", slave, strerror(errno), errno);
        close(slave); close(master); return -1;
    }
    if (winp && ioctl(slave, TIOCSWINSZ, winp) == -1) {
        set_error_errno("ioctl(TIOCSWINSZ)");
        pty_log(PTY_LOG_ERROR, "pty", "TIOCSWINSZ(fd=%d): %s (errno=%d)", slave, strerror(errno), errno);
        close(slave); close(master); return -1;
    }
    *master_out = master;
    *slave_out = slave;
    return 0;
}

/* --- macOS: posix_spawn path --- */

#ifdef __APPLE__

/**
 * Spawn child via posix_spawn (macOS preferred).
 * POSIX_SPAWN_CLOEXEC_DEFAULT closes all fds except wired ones.
 * POSIX_SPAWN_SETSID creates a new session.
 */
static int spawn_macos(const PtyOptions *opts, int master, int slave,
                       pid_t *pid_out) {
    int err;
    posix_spawn_file_actions_t acts;
    posix_spawnattr_t attrs;

    posix_spawn_file_actions_init(&acts);
    posix_spawnattr_init(&attrs);

    /* Wire slave to stdin/stdout/stderr, close both fds. */
    posix_spawn_file_actions_adddup2(&acts, slave, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&acts, slave, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&acts, slave, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&acts, slave);
    posix_spawn_file_actions_addclose(&acts, master);

    if (opts->working_directory && opts->working_directory[0] != '\0') {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        posix_spawn_file_actions_addchdir_np(&acts, opts->working_directory);
#pragma clang diagnostic pop
    }

    short flags = POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGDEF
                | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSID;
    err = posix_spawnattr_setflags(&attrs, flags);
    if (err != 0) { set_error("posix_spawnattr_setflags: %s", strerror(err)); goto done; }

    /* Reset all signals to default in child (node-pty pattern). */
    sigset_t sig_set;
    sigfillset(&sig_set);
    err = posix_spawnattr_setsigdefault(&attrs, &sig_set);
    if (err != 0) { set_error("posix_spawnattr_setsigdefault: %s", strerror(err)); goto done; }

    /* Clear signal mask in child. */
    sigemptyset(&sig_set);
    err = posix_spawnattr_setsigmask(&attrs, &sig_set);
    if (err != 0) { set_error("posix_spawnattr_setsigmask: %s", strerror(err)); goto done; }

    /* Retry on EINTR (node-pty pattern). */
    log_spawn_environment((char *const *)opts->environment);
    do {
        err = posix_spawn(pid_out, opts->executable, &acts, &attrs,
                          (char *const *)opts->arguments,
                          (char *const *)opts->environment);
    } while (err == EINTR);
    if (err != 0) {
        set_error("posix_spawn(%s): %s (errno=%d)", opts->executable,
                  strerror(err), err);
        pty_log(PTY_LOG_ERROR, "spawn", "posix_spawn(%s): %s (errno=%d)",
                opts->executable, strerror(err), err);
    }

done:
    posix_spawn_file_actions_destroy(&acts);
    posix_spawnattr_destroy(&attrs);
    return (err == 0) ? 0 : -1;
}

#endif /* __APPLE__ */

/* --- Linux: fork + execvp path --- */

#ifndef __APPLE__

/** Close all fds above stderr in child (wezterm pattern). */
static void close_fds_above_stderr(void) {
    int max_fd = (int)sysconf(_SC_OPEN_MAX);
    if (max_fd < 0) max_fd = 1024;
    for (int fd = STDERR_FILENO + 1; fd < max_fd; fd++) close(fd);
}

/** Reset all signal dispositions to SIG_DFL (node-pty pattern). */
static void reset_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    for (int i = 1; i < NSIG; i++) sigaction(i, &sa, NULL);
}

/** Spawn child via fork + execvp. Block signals before fork (node-pty). */
static int spawn_linux(const PtyOptions *opts, int master, int slave,
                       pid_t *pid_out) {
    sigset_t all_sigs, old_mask;
    sigfillset(&all_sigs);
    pthread_sigmask(SIG_SETMASK, &all_sigs, &old_mask);

    pid_t pid = fork();
    if (pid < 0) {
        set_error_errno("fork");
        pty_log(PTY_LOG_ERROR, "spawn", "fork: %s (errno=%d)",
                strerror(errno), errno);
        pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        return -1;
    }

    if (pid == 0) {
        /* --- Child --- */
        if (setsid() == -1) { perror("setsid"); _exit(127); }
        /* TIOCSCTTY required for SIGWINCH delivery on resize. */
        if (ioctl(slave, TIOCSCTTY, 0) == -1) { perror("TIOCSCTTY"); _exit(127); }
        if (dup2(slave, STDIN_FILENO) == -1)  _exit(127);
        if (dup2(slave, STDOUT_FILENO) == -1) _exit(127);
        if (dup2(slave, STDERR_FILENO) == -1) _exit(127);
        close_fds_above_stderr();
        reset_signals();
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        if (opts->working_directory && opts->working_directory[0] != '\0') {
            if (chdir(opts->working_directory) == -1) { perror("chdir"); _exit(127); }
        }
        if (opts->environment) {
            extern char **environ;
            environ = (char **)opts->environment;
        }
        execvp(opts->executable, (char *const *)opts->arguments);
        perror("execvp");
        _exit(127);
    }

    /* --- Parent --- */
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    *pid_out = pid;
    return 0;
}

#endif /* !__APPLE__ */

/* --- pty_create: allocate PTY, spawn child, start threads --- */

FFI_PLUGIN_EXPORT PtyHandle *pty_create(const PtyOptions *opts,
                                        Dart_Port output_port,
                                        Dart_Port exit_port) {
    if (!opts || !opts->executable) {
        set_error("pty_create: opts and executable must not be NULL");
        return NULL;
    }
    if (opts->rows == 0 || opts->cols == 0) {
        set_error("pty_create: rows and cols must be > 0");
        return NULL;
    }
    tls_error_set = 0;

    struct termios term;
    configure_termios(&term);
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = opts->rows;
    ws.ws_col = opts->cols;

    int master, slave;
    if (allocate_pty(&master, &slave, &term, &ws) == -1) return NULL;

    pty_log(PTY_LOG_INFO, "spawn", "Spawning %s (%dx%d)",
            opts->executable, opts->cols, opts->rows);
    log_spawn_environment((char *const *)opts->environment);

    pid_t pid;
#ifdef __APPLE__
    int rc = spawn_macos(opts, master, slave, &pid);
#else
    int rc = spawn_linux(opts, master, slave, &pid);
#endif
    close(slave); /* Only child needs the slave fd. */
    if (rc == -1) { close(master); return NULL; }

    pty_log(PTY_LOG_INFO, "spawn", "Child PID %d, master fd %d", pid, master);

    PtyHandle *h = (PtyHandle *)calloc(1, sizeof(PtyHandle));
    if (!h) {
        set_error("pty_create: calloc failed");
        close(master); kill(pid, SIGKILL); return NULL;
    }
    h->master_fd = master;
    h->child_pid = pid;
    h->output_port = output_port;
    h->exit_port = exit_port;

    /* Start read thread. */
    ReadThreadCtx *rctx = (ReadThreadCtx *)malloc(sizeof(ReadThreadCtx));
    if (!rctx) {
        set_error("malloc(ReadThreadCtx) failed");
        close(master); kill(pid, SIGKILL); free(h); return NULL;
    }
    rctx->fd = master;
    rctx->port = output_port;
    rctx->closed_flag = &h->closed;
    pty_log(PTY_LOG_INFO, "spawn", "Starting read thread for fd %d", master);
    if (pthread_create(&h->read_tid, NULL, read_thread_fn, rctx) != 0) {
        set_error_errno("pthread_create(read)");
        free(rctx); close(master); kill(pid, SIGKILL); free(h); return NULL;
    }

    /* Start wait thread. */
    WaitThreadCtx *wctx = (WaitThreadCtx *)malloc(sizeof(WaitThreadCtx));
    if (!wctx) {
        set_error("malloc(WaitThreadCtx) failed");
        close(master); kill(pid, SIGKILL);
        pthread_join(h->read_tid, NULL); free(h); return NULL;
    }
    wctx->pid = pid;
    wctx->port = exit_port;
    pty_log(PTY_LOG_INFO, "spawn", "Starting wait thread for pid %d", pid);
    if (pthread_create(&h->wait_tid, NULL, wait_thread_fn, wctx) != 0) {
        set_error_errno("pthread_create(wait)");
        free(wctx); close(master); kill(pid, SIGKILL);
        pthread_join(h->read_tid, NULL); free(h); return NULL;
    }
    return h;
}

/* --- pty_write: write data to PTY master (stdin of child) --- */

FFI_PLUGIN_EXPORT int pty_write(PtyHandle *handle, const uint8_t *data, int len) {
    if (!handle || handle->closed) { set_error("pty_write: invalid handle"); return -1; }
    int total = 0;
    while (total < len) {
        ssize_t w = write(handle->master_fd, data + total, (size_t)(len - total));
        if (w < 0) {
            if (errno == EINTR) continue;
            set_error_errno("write(master_fd)");
            pty_log(PTY_LOG_ERROR, "write",
                    "write(fd=%d, %d bytes): %s (errno=%d)",
                    handle->master_fd, len - total, strerror(errno), errno);
            return -1;
        }
        total += (int)w;
    }
    return total;
}

/* --- pty_resize: change terminal dimensions --- */

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols) {
    if (!handle || handle->closed) { set_error("pty_resize: invalid handle"); return -1; }
    if (rows <= 0 || cols <= 0) { set_error("pty_resize: rows/cols must be > 0"); return -1; }
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    if (ioctl(handle->master_fd, TIOCSWINSZ, &ws) == -1) {
        set_error_errno("ioctl(TIOCSWINSZ)");
        pty_log(PTY_LOG_ERROR, "resize",
                "TIOCSWINSZ(fd=%d): %s (errno=%d)",
                handle->master_fd, strerror(errno), errno);
        return -1;
    }
    pty_log(PTY_LOG_INFO, "resize", "Resized to %dx%d", cols, rows);
    return 0;
}

/* --- pty_getpid: get child process ID --- */

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle) {
    return handle ? (int)handle->child_pid : -1;
}

/* --- pty_kill: send signal to child process --- */

FFI_PLUGIN_EXPORT int pty_kill(PtyHandle *handle, int signal) {
    if (!handle) { set_error("pty_kill: invalid handle"); return -1; }
    if (kill(handle->child_pid, signal) == -1) {
        set_error_errno("kill");
        pty_log(PTY_LOG_ERROR, "kill",
                "kill(pid=%d, sig=%d): %s (errno=%d)",
                handle->child_pid, signal, strerror(errno), errno);
        return -1;
    }
    pty_log(PTY_LOG_INFO, "kill", "Signal %d to PID %d", signal, handle->child_pid);
    return 0;
}

/* --- pty_close: clean up all PTY resources --- */

FFI_PLUGIN_EXPORT void pty_close(PtyHandle *handle) {
    if (!handle || handle->closed) return;
    handle->closed = 1;
    pty_log(PTY_LOG_INFO, "close", "Closing PTY for PID %d", handle->child_pid);
    kill(handle->child_pid, SIGHUP);
    if (handle->master_fd >= 0) { close(handle->master_fd); handle->master_fd = -1; }
    pthread_join(handle->read_tid, NULL);
    pthread_join(handle->wait_tid, NULL);
    pty_log(PTY_LOG_INFO, "close", "PTY closed for PID %d", handle->child_pid);
    free(handle);
}

#endif /* !_WIN32 && !_WIN64 */
