/// Dart API for dart_pty — production-grade PTY for Flutter desktop.
///
/// Wraps the native FFI bindings with a clean, stream-based Dart API.
/// Handles native library loading, ReceivePort wiring, and log forwarding.
library;

import 'dart:async';
import 'dart:ffi';
import 'dart:io' show Platform;
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'dart_pty_bindings_generated.dart';

// ---------------------------------------------------------------------------
//  PtyLogLevel — mirrors native PtyLogLevel enum
// ---------------------------------------------------------------------------

/// Log severity levels matching the native C enum.
enum PtyLogLevel {
  /// Verbose diagnostic output.
  debug,

  /// Normal lifecycle events (spawn, resize, exit).
  info,

  /// Recoverable issues (EINTR retries, EIO as EOF).
  warn,

  /// Unrecoverable failures (syscall errors).
  error;

  /// Convert from native integer level.
  static PtyLogLevel fromNative(int value) {
    return switch (value) {
      0 => PtyLogLevel.debug,
      1 => PtyLogLevel.info,
      2 => PtyLogLevel.warn,
      3 => PtyLogLevel.error,
      _ => PtyLogLevel.debug,
    };
  }
}

/// Signature for the log callback.
typedef PtyLogCallback =
    void Function(PtyLogLevel level, String component, String message);

const _spawnDebugEnvKeys = <String>{
  'TERM',
  'COLORTERM',
  'TERM_PROGRAM',
  'TERM_PROGRAM_VERSION',
  'FORCE_HYPERLINK',
  'COLORFGBG',
  'TERM_PROGRAM_BACKGROUND',
  'LANG',
  'LC_ALL',
  'CI',
  'TEAMCITY_VERSION',
  'VTE_VERSION',
};

// ---------------------------------------------------------------------------
//  PtyException — structured error from native PTY operations
// ---------------------------------------------------------------------------

/// Exception thrown by PTY operations when a native call fails.
///
/// Contains the error message, the syscall that failed, and the
/// platform-specific error code (errno on Unix, GetLastError on Windows).
class PtyException implements Exception {
  /// Human-readable error description.
  final String message;

  /// The native syscall that failed (e.g. "posix_openpt", "CreateProcessW").
  final String? syscall;

  /// Unix errno value, if available.
  final int? errno;

  /// Windows GetLastError value, if available.
  final int? winError;

  /// Create a PtyException.
  const PtyException(this.message, {this.syscall, this.errno, this.winError});

  @override
  String toString() {
    final parts = <String>['PtyException: $message'];
    if (syscall != null) parts.add('syscall=$syscall');
    if (errno != null) parts.add('errno=$errno');
    if (winError != null) parts.add('winError=$winError');
    return parts.join(', ');
  }
}

// ---------------------------------------------------------------------------
//  Native library loader
// ---------------------------------------------------------------------------

/// Load the native library for the current platform.
///
/// On macOS, CocoaPods compiles the plugin as a framework that's linked into
/// the app — symbols are available via DynamicLibrary.process().
/// On Linux/Windows, the shared library is loaded by name.
DynamicLibrary _openNativeLibrary() {
  if (Platform.isMacOS) return DynamicLibrary.process();
  if (Platform.isLinux) return DynamicLibrary.open('libdart_pty.so');
  if (Platform.isWindows) return DynamicLibrary.open('dart_pty.dll');
  throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
}

/// Lazily loaded native bindings.
DartPtyBindings? _cachedBindings;
bool _dartApiInitialized = false;

DartPtyBindings _loadBindings(PtyLogCallback? onLog) {
  if (_cachedBindings != null) return _cachedBindings!;
  onLog?.call(PtyLogLevel.info, 'dart', 'Loading native library');
  final lib = _openNativeLibrary();
  _cachedBindings = DartPtyBindings(lib);
  return _cachedBindings!;
}

void _ensureDartApiInitialized(
  DartPtyBindings bindings,
  PtyLogCallback? onLog,
) {
  if (_dartApiInitialized) return;
  onLog?.call(PtyLogLevel.info, 'dart', 'Initializing Dart API DL');
  final result = bindings.pty_init_dart_api(NativeApi.initializeApiDLData);
  if (result != 0) {
    throw PtyException(
      'Dart_InitializeApiDL failed with code $result',
      syscall: 'Dart_InitializeApiDL',
    );
  }
  _dartApiInitialized = true;
}

// ---------------------------------------------------------------------------
//  Pty — the public API
// ---------------------------------------------------------------------------

/// A pseudo-terminal session.
///
/// Create with [Pty.start], which spawns a child process and returns
/// a [Pty] instance. Output from the child is available as a [Stream]
/// via [output]. The exit code is available via [exitCode].
///
/// Example:
/// ```dart
/// final pty = Pty.start(
///   '/bin/bash',
///   columns: 80,
///   rows: 24,
///   onLog: (level, component, message) {
///     print('[$level] $component: $message');
///   },
/// );
/// pty.output.listen((data) => stdout.add(data));
/// final code = await pty.exitCode;
/// pty.dispose();
/// ```
class Pty {
  final DartPtyBindings _bindings;
  final Pointer<PtyHandle> _handle;
  final ReceivePort _outputPort;
  final ReceivePort _exitPort;
  final ReceivePort? _logPort;
  final PtyLogCallback? _onLog;
  final StreamController<Uint8List> _outputController;
  final Completer<int> _exitCompleter = Completer<int>();
  bool _disposed = false;

  Pty._({
    required DartPtyBindings bindings,
    required Pointer<PtyHandle> handle,
    required ReceivePort outputPort,
    required ReceivePort exitPort,
    required ReceivePort? logPort,
    required PtyLogCallback? onLog,
    required StreamController<Uint8List> outputController,
  }) : _bindings = bindings,
       _handle = handle,
       _outputPort = outputPort,
       _exitPort = exitPort,
       _logPort = logPort,
       _onLog = onLog,
       _outputController = outputController;

  /// Spawn a child process in a new pseudo-terminal.
  ///
  /// [executable] is the path to the program to run.
  /// [arguments] are the command-line arguments (argv). Defaults to
  /// `[executable]` if not specified.
  /// [environment] is a map of "KEY=VALUE" pairs. Pass null to inherit.
  /// [workingDirectory] is the cwd for the child. Pass null to inherit.
  /// [columns] and [rows] set the initial terminal size.
  /// [onLog] receives structured log messages from both native and Dart layers.
  static Pty start(
    String executable, {
    List<String>? arguments,
    Map<String, String>? environment,
    String? workingDirectory,
    int columns = 80,
    int rows = 24,
    PtyLogCallback? onLog,
  }) {
    final bindings = _loadBindings(onLog);
    _ensureDartApiInitialized(bindings, onLog);

    // Set up the native log port if onLog is provided.
    ReceivePort? logPort;
    if (onLog != null) {
      logPort = ReceivePort('dart_pty.log');
      bindings.pty_set_log_port(logPort.sendPort.nativePort);
      logPort.listen((message) {
        if (message is List && message.length == 3) {
          final level = PtyLogLevel.fromNative(message[0] as int);
          final component = message[1] as String;
          final msg = message[2] as String;
          onLog(level, component, msg);
        }
      });
    }

    onLog?.call(PtyLogLevel.info, 'dart', 'Calling pty_create for $executable');

    // Build native options.
    final opts = calloc<PtyOptions>();
    final execPtr = executable.toNativeUtf8();
    opts.ref.executable = execPtr.cast();
    opts.ref.rows = rows;
    opts.ref.cols = columns;

    // Build argv: [executable, ...arguments, null].
    final args = arguments ?? [executable];
    final argvPtr = calloc<Pointer<Char>>(args.length + 1);
    final argPtrs = <Pointer<Utf8>>[];
    for (var i = 0; i < args.length; i++) {
      final p = args[i].toNativeUtf8();
      argPtrs.add(p);
      argvPtr[i] = p.cast();
    }
    argvPtr[args.length] = nullptr;
    opts.ref.arguments = argvPtr.cast();

    // Build envp: ["KEY=VALUE", ..., null] or null.
    Pointer<Pointer<Char>>? envpPtr;
    final envPtrs = <Pointer<Utf8>>[];
    if (environment != null) {
      final entries = environment.entries
          .map((e) => '${e.key}=${e.value}')
          .toList();
      onLog?.call(
        PtyLogLevel.info,
        'dart',
        'Preparing ${entries.length} environment entries for pty_create',
      );
      for (final entry in entries) {
        final separator = entry.indexOf('=');
        final key = separator == -1 ? entry : entry.substring(0, separator);
        if (_spawnDebugEnvKeys.contains(key)) {
          onLog?.call(PtyLogLevel.info, 'dart', 'env $entry');
        }
      }
      envpPtr = calloc<Pointer<Char>>(entries.length + 1);
      for (var i = 0; i < entries.length; i++) {
        final p = entries[i].toNativeUtf8();
        envPtrs.add(p);
        envpPtr[i] = p.cast();
      }
      envpPtr[entries.length] = nullptr;
      opts.ref.environment = envpPtr.cast();
    }

    // Working directory.
    Pointer<Utf8>? cwdPtr;
    if (workingDirectory != null) {
      cwdPtr = workingDirectory.toNativeUtf8();
      opts.ref.working_directory = cwdPtr.cast();
    }

    // Create receive ports for output and exit.
    final outputPort = ReceivePort('dart_pty.output');
    final exitPort = ReceivePort('dart_pty.exit');

    // Call native pty_create.
    final handle = bindings.pty_create(
      opts,
      outputPort.sendPort.nativePort,
      exitPort.sendPort.nativePort,
    );

    // Free native strings.
    calloc.free(execPtr);
    for (final p in argPtrs) {
      calloc.free(p);
    }
    calloc.free(argvPtr);
    if (envpPtr != null) {
      for (final p in envPtrs) {
        calloc.free(p);
      }
      calloc.free(envpPtr);
    }
    if (cwdPtr != null) calloc.free(cwdPtr);
    calloc.free(opts);

    if (handle == nullptr) {
      outputPort.close();
      exitPort.close();
      logPort?.close();
      final errPtr = bindings.pty_error();
      final errMsg = errPtr == nullptr
          ? 'pty_create failed (unknown error)'
          : errPtr.cast<Utf8>().toDartString();
      throw PtyException(errMsg, syscall: 'pty_create');
    }

    final pid = bindings.pty_getpid(handle);
    onLog?.call(
      PtyLogLevel.info,
      'dart',
      'pty_create success: pid=$pid, '
          'outputPort=${outputPort.sendPort.nativePort}, '
          'exitPort=${exitPort.sendPort.nativePort}',
    );

    // Wire up output stream.
    final outputController = StreamController<Uint8List>.broadcast();
    outputPort.listen(
      (message) {
        if (message is Uint8List) {
          outputController.add(message);
        }
      },
      onError: (Object error) {
        onLog?.call(PtyLogLevel.error, 'dart', 'Output stream error: $error');
      },
    );

    // Wire up exit code.
    final pty = Pty._(
      bindings: bindings,
      handle: handle,
      outputPort: outputPort,
      exitPort: exitPort,
      logPort: logPort,
      onLog: onLog,
      outputController: outputController,
    );

    exitPort.listen((message) {
      if (message is int && !pty._exitCompleter.isCompleted) {
        onLog?.call(PtyLogLevel.info, 'dart', 'Exit code received: $message');
        pty._exitCompleter.complete(message);
      }
    });

    return pty;
  }

  /// Stream of output bytes from the child process.
  Stream<Uint8List> get output => _outputController.stream;

  /// Future that completes with the child's exit code.
  Future<int> get exitCode => _exitCompleter.future;

  /// The process ID of the child.
  int get pid => _bindings.pty_getpid(_handle);

  /// Write data to the PTY (stdin of the child).
  void write(Uint8List data) {
    _ensureNotDisposed();
    _onLog?.call(PtyLogLevel.debug, 'dart', 'write: ${data.length} bytes');
    final ptr = calloc<Uint8>(data.length);
    ptr.asTypedList(data.length).setAll(0, data);
    final result = _bindings.pty_write(_handle, ptr, data.length);
    calloc.free(ptr);
    if (result < 0) {
      final errPtr = _bindings.pty_error();
      final errMsg = errPtr == nullptr
          ? 'pty_write failed'
          : errPtr.cast<Utf8>().toDartString();
      throw PtyException(errMsg, syscall: 'write');
    }
  }

  /// Resize the terminal to [rows] x [columns].
  void resize(int rows, int columns) {
    _ensureNotDisposed();
    _onLog?.call(PtyLogLevel.info, 'dart', 'resize: ${columns}x$rows');
    final result = _bindings.pty_resize(_handle, rows, columns);
    if (result < 0) {
      final errPtr = _bindings.pty_error();
      final errMsg = errPtr == nullptr
          ? 'pty_resize failed'
          : errPtr.cast<Utf8>().toDartString();
      throw PtyException(errMsg, syscall: 'pty_resize');
    }
  }

  /// Send a signal to the child process.
  ///
  /// Common values: 15 (SIGTERM), 9 (SIGKILL).
  /// On Windows, 9 calls TerminateProcess; others send Ctrl+C.
  void kill([int signal = 15]) {
    _ensureNotDisposed();
    _onLog?.call(PtyLogLevel.info, 'dart', 'kill: signal=$signal');
    final result = _bindings.pty_kill(_handle, signal);
    if (result < 0) {
      final errPtr = _bindings.pty_error();
      final errMsg = errPtr == nullptr
          ? 'pty_kill failed'
          : errPtr.cast<Utf8>().toDartString();
      throw PtyException(errMsg, syscall: 'kill');
    }
  }

  /// Release all native resources.
  ///
  /// Closes the PTY master, joins background threads, and frees memory.
  /// Safe to call multiple times.
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _onLog?.call(PtyLogLevel.info, 'dart', 'dispose: closing PTY');
    _bindings.pty_close(_handle);
    _outputPort.close();
    _exitPort.close();
    _outputController.close();
    // Disable native log port forwarding.
    if (_logPort != null) {
      _bindings.pty_set_log_port(0);
      _logPort.close();
    }
    _onLog?.call(PtyLogLevel.info, 'dart', 'dispose: complete');
  }

  void _ensureNotDisposed() {
    if (_disposed) {
      throw PtyException('PTY has been disposed');
    }
  }
}
