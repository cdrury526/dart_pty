/// dart_pty — Production-grade PTY for Flutter desktop via FFI.
///
/// Provides pseudo-terminal support on macOS (posix_spawn), Linux (fork/exec),
/// and Windows (ConPTY). Native code posts output and exit events to Dart
/// via ReceivePort + Dart_PostCObject.
library dart_pty;

export 'src/dart_pty_bindings_generated.dart' hide PtyLogLevel;
export 'src/pty.dart' show Pty, PtyException, PtyLogCallback, PtyLogLevel;
