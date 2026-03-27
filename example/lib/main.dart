import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'tab_bar_widget.dart';
import 'terminal_page.dart';
import 'terminal_tab.dart';
import 'theme.dart';

// ---------------------------------------------------------------------------
//  App entry point
// ---------------------------------------------------------------------------

void main() {
  runApp(const TerminalApp());
}

// ---------------------------------------------------------------------------
//  TerminalApp — root widget
// ---------------------------------------------------------------------------

class TerminalApp extends StatefulWidget {
  const TerminalApp({super.key});

  @override
  State<TerminalApp> createState() => _TerminalAppState();
}

class _TerminalAppState extends State<TerminalApp> {
  ThemeMode _themeMode = ThemeMode.dark;

  void _toggleTheme() {
    setState(() {
      _themeMode =
          _themeMode == ThemeMode.dark ? ThemeMode.light : ThemeMode.dark;
    });
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'dart_pty Terminal',
      debugShowCheckedModeBanner: false,
      theme: lightAppTheme,
      darkTheme: darkAppTheme,
      themeMode: _themeMode,
      home: TerminalHome(
        themeMode: _themeMode,
        onToggleTheme: _toggleTheme,
      ),
    );
  }
}

// ---------------------------------------------------------------------------
//  TerminalHome — scaffold with tab management
// ---------------------------------------------------------------------------

class TerminalHome extends StatefulWidget {
  const TerminalHome({
    super.key,
    required this.themeMode,
    required this.onToggleTheme,
  });

  final ThemeMode themeMode;
  final VoidCallback onToggleTheme;

  @override
  State<TerminalHome> createState() => _TerminalHomeState();
}

class _TerminalHomeState extends State<TerminalHome> {
  final List<TerminalTab> _tabs = [];
  int _activeIndex = 0;

  @override
  void initState() {
    super.initState();
    // Start with one tab.
    _addTab();
  }

  @override
  void dispose() {
    _disposeAllTabs();
    super.dispose();
  }

  // -------------------------------------------------------------------------
  //  Tab management
  // -------------------------------------------------------------------------

  void _addTab() {
    final tab = TerminalTab.create();
    tab.onTitleChange = (_) {
      if (mounted) setState(() {});
    };
    setState(() {
      _tabs.add(tab);
      _activeIndex = _tabs.length - 1;
    });
  }

  void _closeTab(int index) {
    if (index < 0 || index >= _tabs.length) return;

    final tab = _tabs[index];
    setState(() {
      _tabs.removeAt(index);
      tab.dispose();

      if (_tabs.isEmpty) {
        // Last tab closed — create a fresh one.
        _addTab();
        return;
      }
      // Adjust active index.
      if (_activeIndex >= _tabs.length) {
        _activeIndex = _tabs.length - 1;
      } else if (_activeIndex > index) {
        _activeIndex--;
      }
    });
  }

  void _selectTab(int index) {
    if (index < 0 || index >= _tabs.length) return;
    setState(() => _activeIndex = index);
  }

  void _disposeAllTabs() {
    for (final tab in _tabs) {
      tab.dispose();
    }
    _tabs.clear();
  }

  // -------------------------------------------------------------------------
  //  Keyboard shortcuts
  // -------------------------------------------------------------------------

  /// The modifier key: Cmd on macOS, Ctrl elsewhere.
  static final bool _isMacOS = Platform.isMacOS;

  KeyEventResult _handleKeyEvent(FocusNode node, KeyEvent event) {
    if (event is! KeyDownEvent && event is! KeyRepeatEvent) {
      return KeyEventResult.ignored;
    }

    final isModifier = _isMacOS
        ? HardwareKeyboard.instance.isMetaPressed
        : HardwareKeyboard.instance.isControlPressed;

    if (!isModifier) return KeyEventResult.ignored;

    // Cmd+T / Ctrl+T — new tab.
    if (event.logicalKey == LogicalKeyboardKey.keyT) {
      _addTab();
      return KeyEventResult.handled;
    }

    // Cmd+W / Ctrl+W — close current tab.
    if (event.logicalKey == LogicalKeyboardKey.keyW) {
      _closeTab(_activeIndex);
      return KeyEventResult.handled;
    }

    // Cmd+1-9 / Ctrl+1-9 — switch to tab N.
    final digit = _digitFromKey(event.logicalKey);
    if (digit != null && digit >= 1 && digit <= 9) {
      final target = digit - 1;
      if (target < _tabs.length) {
        _selectTab(target);
      }
      return KeyEventResult.handled;
    }

    return KeyEventResult.ignored;
  }

  /// Extract digit 1-9 from a logical key, or null.
  static int? _digitFromKey(LogicalKeyboardKey key) {
    return switch (key) {
      LogicalKeyboardKey.digit1 => 1,
      LogicalKeyboardKey.digit2 => 2,
      LogicalKeyboardKey.digit3 => 3,
      LogicalKeyboardKey.digit4 => 4,
      LogicalKeyboardKey.digit5 => 5,
      LogicalKeyboardKey.digit6 => 6,
      LogicalKeyboardKey.digit7 => 7,
      LogicalKeyboardKey.digit8 => 8,
      LogicalKeyboardKey.digit9 => 9,
      _ => null,
    };
  }

  // -------------------------------------------------------------------------
  //  Build
  // -------------------------------------------------------------------------

  Brightness get _brightness =>
      widget.themeMode == ThemeMode.dark ? Brightness.dark : Brightness.light;

  @override
  Widget build(BuildContext context) {
    return Focus(
      onKeyEvent: _handleKeyEvent,
      child: Scaffold(
        appBar: _buildAppBar(),
        body: Column(
          children: [
            // Tab bar.
            TerminalTabBar(
              tabs: _tabs,
              activeIndex: _activeIndex,
              onSelect: _selectTab,
              onClose: _closeTab,
              onNewTab: _addTab,
            ),

            // Terminal content area.
            Expanded(
              child: IndexedStack(
                index: _activeIndex,
                children: [
                  for (var i = 0; i < _tabs.length; i++)
                    TerminalPage(
                      key: ValueKey(_tabs[i].id),
                      tab: _tabs[i],
                      brightness: _brightness,
                    ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  PreferredSizeWidget _buildAppBar() {
    return AppBar(
      title: const Text('dart_pty Terminal'),
      titleTextStyle: Theme.of(context).textTheme.titleMedium,
      toolbarHeight: 40,
      actions: [
        // Active session count.
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 8),
          child: Center(
            child: Text(
              '${_tabs.length} session${_tabs.length == 1 ? '' : 's'}',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ),
        ),

        // Theme toggle.
        IconButton(
          icon: Icon(
            widget.themeMode == ThemeMode.dark
                ? Icons.light_mode
                : Icons.dark_mode,
          ),
          tooltip: 'Toggle theme',
          onPressed: widget.onToggleTheme,
        ),

        const SizedBox(width: 8),
      ],
    );
  }
}
