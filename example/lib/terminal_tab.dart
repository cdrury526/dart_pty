import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:dart_pty/dart_pty.dart';
import 'package:dart_xterm/dart_xterm.dart';

// ---------------------------------------------------------------------------
//  TerminalTab — one shell session (Terminal + Pty + wiring)
// ---------------------------------------------------------------------------

/// Holds all state for a single terminal tab: the [Terminal] emulator,
/// the native [Pty] process, and the stream subscriptions that wire them
/// together.
///
/// Call [dispose] to tear everything down (kills the PTY, closes streams).
class TerminalTab {
  TerminalTab._({
    required this.id,
    required this.terminal,
    required this.pty,
    required StreamSubscription outputSubscription,
    String title = 'Shell',
  })  : _outputSubscription = outputSubscription,
        _title = title;

  /// Unique identifier for this tab (monotonically increasing).
  final int id;

  /// The terminal emulator that parses escape sequences and holds the buffer.
  final Terminal terminal;

  /// The native PTY process connected to a shell.
  final Pty pty;

  /// Stream subscription forwarding PTY output to the terminal.
  final StreamSubscription _outputSubscription;

  /// Current tab title (updated by the shell via OSC escape sequences).
  String _title;
  String get title => _title;

  /// Whether this tab has been disposed.
  bool _disposed = false;
  bool get isDisposed => _disposed;

  /// Callback invoked when the tab title changes.
  void Function(TerminalTab tab)? onTitleChange;

  // -------------------------------------------------------------------------
  //  Factory — create and wire a new tab
  // -------------------------------------------------------------------------

  /// Global counter for generating unique tab IDs.
  static int _nextId = 1;

  /// Detect the user's shell.
  static String get _shell {
    if (Platform.isWindows) return 'cmd.exe';
    return Platform.environment['SHELL'] ?? '/bin/zsh';
  }

  /// Create a new terminal tab with a running shell process.
  ///
  /// The [columns] and [rows] set the initial terminal dimensions. These are
  /// updated automatically when the [TerminalView] widget resizes (via
  /// [autoResize]).
  static TerminalTab create({
    int columns = 80,
    int rows = 24,
  }) {
    final id = _nextId++;

    final terminal = Terminal(maxLines: 10000);

    final shell = _shell;
    // Inherit current environment and ensure TERM is set for proper
    // escape sequence handling (zsh ZLE needs this).
    final env = Map<String, String>.from(Platform.environment);
    env['TERM'] = 'xterm-256color';
    env['COLORTERM'] = 'truecolor';

    final pty = Pty.start(
      shell,
      columns: columns,
      rows: rows,
      environment: env,
      onLog: (level, component, msg) {
        debugPrint('[PTY:$level] $component: $msg');
      },
    );

    // PTY output (bytes from shell) -> Terminal (escape sequence parsing).
    // Use a streaming UTF-8 decoder to handle multi-byte characters
    // that may be split across read boundaries.
    final utf8Decoder = Utf8Decoder(allowMalformed: true);
    final outputSub = pty.output
        .map((data) => utf8Decoder.convert(data))
        .listen((text) {
      terminal.write(text);
    });

    final tab = TerminalTab._(
      id: id,
      terminal: terminal,
      pty: pty,
      outputSubscription: outputSub,
      title: shell.split('/').last,
    );

    // Terminal keystrokes -> PTY (user types -> shell stdin).
    terminal.onOutput = (data) {
      if (!tab._disposed) {
        final bytes = Uint8List.fromList(data.codeUnits);
        debugPrint('[SEND] ${bytes.length} bytes: ${bytes.map((b) => '0x${b.toRadixString(16).padLeft(2, '0')}').join(' ')}');
        pty.write(bytes);
      }
    };

    // Terminal resize -> PTY (SIGWINCH).
    terminal.onResize = (width, height, pixelWidth, pixelHeight) {
      if (!tab._disposed) {
        pty.resize(height, width);
      }
    };

    // Shell title changes -> tab title.
    terminal.onTitleChange = (title) {
      tab._title = title;
      tab.onTitleChange?.call(tab);
    };

    return tab;
  }

  // -------------------------------------------------------------------------
  //  Lifecycle
  // -------------------------------------------------------------------------

  /// Release all resources: kill the PTY, cancel subscriptions.
  ///
  /// Safe to call multiple times.
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _outputSubscription.cancel();
    terminal.onOutput = null;
    terminal.onResize = null;
    terminal.onTitleChange = null;
    pty.kill();
    pty.dispose();
  }
}
