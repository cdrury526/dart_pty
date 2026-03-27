import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:dart_xterm/dart_xterm.dart';

import 'terminal_tab.dart';
import 'theme.dart';

// ---------------------------------------------------------------------------
//  TerminalPage — renders a single terminal tab
// ---------------------------------------------------------------------------

/// A full-size [TerminalView] for one [TerminalTab], with focus management
/// and a right-click context menu (copy / paste / clear).
class TerminalPage extends StatefulWidget {
  const TerminalPage({
    super.key,
    required this.tab,
    required this.brightness,
  });

  /// The terminal tab to render.
  final TerminalTab tab;

  /// Current theme brightness (dark or light).
  final Brightness brightness;

  @override
  State<TerminalPage> createState() => _TerminalPageState();
}

class _TerminalPageState extends State<TerminalPage>
    with AutomaticKeepAliveClientMixin {
  late final TerminalController _controller;
  late final FocusNode _focusNode;

  @override
  bool get wantKeepAlive => true;

  @override
  void initState() {
    super.initState();
    _controller = TerminalController();
    _focusNode = FocusNode(debugLabel: 'terminal-${widget.tab.id}');
  }

  @override
  void dispose() {
    _controller.dispose();
    _focusNode.dispose();
    super.dispose();
  }

  /// Request focus for this terminal (called when the tab becomes active).
  void requestFocus() {
    if (_focusNode.canRequestFocus) {
      _focusNode.requestFocus();
    }
  }

  @override
  Widget build(BuildContext context) {
    super.build(context);
    // Request focus after the frame so the terminal receives keyboard input.
    WidgetsBinding.instance.addPostFrameCallback((_) => requestFocus());

    return TerminalView(
      widget.tab.terminal,
      controller: _controller,
      theme: terminalThemeFor(widget.brightness),
      textStyle: const TerminalStyle(
        fontFamily: 'Menlo',
        fontSize: 14,
      ),
      autoResize: true,
      focusNode: _focusNode,
      autofocus: true,
      hardwareKeyboardOnly: true,
      onSecondaryTapDown: (details, offset) {
        _showContextMenu(context, details.globalPosition);
      },
    );
  }

  // -------------------------------------------------------------------------
  //  Context menu
  // -------------------------------------------------------------------------

  void _showContextMenu(BuildContext context, Offset position) {
    final overlay =
        Overlay.of(context).context.findRenderObject() as RenderBox?;
    if (overlay == null) return;

    showMenu<String>(
      context: context,
      position: RelativeRect.fromLTRB(
        position.dx,
        position.dy,
        position.dx,
        position.dy,
      ),
      items: [
        const PopupMenuItem(value: 'copy', child: Text('Copy')),
        const PopupMenuItem(value: 'paste', child: Text('Paste')),
        const PopupMenuDivider(),
        const PopupMenuItem(value: 'clear', child: Text('Clear')),
      ],
    ).then((value) {
      if (value == null) return;
      switch (value) {
        case 'copy':
          _handleCopy();
        case 'paste':
          _handlePaste();
        case 'clear':
          _handleClear();
      }
    });
  }

  void _handleCopy() {
    final selection = _controller.selection;
    if (selection != null) {
      final text = widget.tab.terminal.buffer.getText(selection);
      Clipboard.setData(ClipboardData(text: text));
    }
  }

  void _handlePaste() async {
    final data = await Clipboard.getData(Clipboard.kTextPlain);
    if (data?.text != null) {
      widget.tab.terminal.paste(data!.text!);
    }
  }

  void _handleClear() {
    // Send clear command to the shell.
    widget.tab.terminal.textInput('clear\n');
  }
}
