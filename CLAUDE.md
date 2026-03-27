# dart_pty — Production-Grade PTY Package for Flutter Desktop

Native FFI pseudo-terminal package for macOS, Windows, and Linux. Spawns shell processes with real PTY allocation (not dart:io Process.start) so interactive programs (vim, htop, colored ls) work correctly.

## qdexcode

- **Project ID**: `c1308426-62c4-4361-82b4-3e819dd5c96f`
- **GitHub**: https://github.com/cdrury526/dart_pty
- **Development Plan**: `6cfd366b-56f1-48bc-9232-dc7219de1a5c` (shared with dart_xterm)

## Reference Implementations (indexed in qdexcode)

| Repo | Project ID | What to reference |
|---|---|---|
| node-pty (Microsoft/VS Code) | `031c2c4b-5d8a-4a44-9baf-feebae50c1d5` | macOS posix_spawn, signal handling, gold standard |
| wezterm portable-pty | `605be883-a7a7-4e7f-9d69-e29845093679` | Clean trait design, EIO→EOF, Windows ConPTY flags |
| flutter_pty (original) | `0accf70a-5546-4031-aba6-2636034ef622` | Dart FFI plugin structure, ReceivePort pattern |
| creack/pty (Go) | `b00141aa-b61c-45b1-b40c-2d11c254d60b` | Simple API inspiration |

## Architecture

```
src/
  dart_pty.h          — Shared C header (structs, function declarations, log callback)
  dart_pty_unix.c     — macOS (posix_spawn) + Linux (fork) implementation
  dart_pty_win.c      — Windows ConPTY implementation
lib/
  dart_pty.dart       — Barrel export
  src/
    pty.dart                       — Public Dart API (Pty class, PtySize, PtyException)
    dart_pty_bindings_generated.dart — Auto-generated FFI bindings (ffigen)
example/
  lib/                — Tabbed terminal example app (uses dart_pty + dart_xterm)
```

## Public API

```dart
class Pty {
  factory Pty.start(String executable, {
    List<String> arguments,
    String? workingDirectory,
    Map<String, String>? environment,
    PtySize size,
    void Function(PtyLogLevel, String, String)? onLog,
  });

  Stream<Uint8List> get output;
  Future<int> get exitCode;
  int get pid;

  void write(Uint8List data);
  void resize(PtySize size);
  void kill([ProcessSignal signal]);
  void dispose();
}
```

## Platform Implementation

**macOS** — posix_spawn() (node-pty pattern):
- posix_openpt → grantpt → unlockpt → TIOCPTYGNAME (thread-safe)
- posix_spawn with POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID
- More stable than fork() in multi-threaded Flutter processes

**Linux** — fork() + execvp():
- Same master/slave allocation
- Child: setsid(), ioctl(TIOCSCTTY), dup2, close inherited FDs, reset signals, execvp

**Windows** — CreatePseudoConsole (ConPTY):
- CreatePipe x2, CreatePseudoConsole, CreateProcessW with EXTENDED_STARTUPINFO_PRESENT
- MUST call ClosePseudoConsole on cleanup (flutter_pty is missing this)

## Logging & Debugging

### Native C Layer

Log callback passed to `pty_create`:
```c
typedef void (*pty_log_fn)(int level, const char* component, const char* message);
// Levels: PTY_LOG_ERROR=0, PTY_LOG_WARN=1, PTY_LOG_INFO=2, PTY_LOG_DEBUG=3
```

All syscall failures log errno/strerror (Unix) or GetLastError (Windows). Lifecycle events logged at INFO level:
- `pty_create: spawning executable=%s pid=%d master_fd=%d`
- `read_loop: EIO received, treating as EOF`
- `wait_loop: child exited code=%d`
- `pty_close: killing pid=%d signal=%d`

### Dart Layer

```dart
final pty = Pty.start('/bin/zsh',
  onLog: (level, component, msg) {
    print('[$level] $component: $msg');
  },
);
```

- `onLog` callback receives both Dart-side and native-side log messages
- Default: null (zero output, zero overhead)
- `PtyException` includes: message, errno/winError, syscall name

### Debugging a failing PTY

1. Enable logging: pass `onLog` to `Pty.start`
2. Check for `PtyException` — includes which syscall failed and the OS error code
3. Use qdexcode to compare behavior against reference implementations
4. Native C code can also log to stderr for low-level debugging: `#define PTY_DEBUG_STDERR 1`

## Key Gotchas (verified during implementation)

- **EIO on read = EOF** — when the slave PTY closes, the master gets EIO. Treat as clean EOF, not error.
- **EINTR retry** — waitpid and read can be interrupted by signals. Always retry.
- **TIOCSCTTY required** — must set controlling terminal in child or SIGWINCH won't be delivered on resize.
- **Close inherited FDs** — child must close all FDs except 0/1/2 to prevent master fd leak.
- **pty_error() is thread-local** — safe to call from any thread, returns last error for that thread.
- **macOS App Sandbox must be disabled** — PTY allocation (tcsetattr) fails with EPERM under App Sandbox. Set `com.apple.security.app-sandbox` to `false` in both DebugProfile.entitlements and Release.entitlements.
- **DynamicLibrary.process() on macOS** — CocoaPods compiles native plugins as frameworks linked into the app. Use `DynamicLibrary.process()` not `DynamicLibrary.open('libdart_pty.dylib')`. Linux/Windows still use `DynamicLibrary.open()`.
- **TERM=xterm-256color required** — zsh's line editor (ZLE) needs TERM set correctly or backspace/cursor movement breaks. Callers MUST pass `environment: {'TERM': 'xterm-256color', ...inherited}` to `Pty.start()`.
- **UTF-8 decoding** — PTY output is raw bytes. Use `Utf8Decoder(allowMalformed: true)` to convert to String before passing to Terminal.write(). Do NOT use `String.fromCharCodes()` — it treats each byte as a code point, breaking multi-byte characters (box drawing, emoji, CJK).
- **No file over 600 lines** — split into focused, composable files.

## Build & Test

```bash
# Generate FFI bindings
dart run ffigen

# Run example app (macOS)
cd example && flutter run -d macos

# Build release
cd example && flutter build macos

# Analyze
flutter analyze
```

## Known Issues (to fix)

- **Box-drawing characters render as thick bars** — dart_xterm renderer may be treating U+2500 range as double-width. Needs investigation in dart_xterm's painter.
- **OSC title sequences leak `t:` to screen** — the `\x1b]0;title\x07` (set window title) handler may not be consuming the full sequence.
- **Some text appears underlined** — SGR escape sequence interpretation issue in dart_xterm.

## Companion Package

**dart_xterm** (`/Users/chrisdrury/projects/dart_xterm`, qdexcode: `e0273d34-522c-454c-af6b-031f04a9e77d`) — the terminal widget that renders PTY output. The example app uses both packages together.
