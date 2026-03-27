import 'package:flutter/material.dart';

import 'terminal_tab.dart';

// ---------------------------------------------------------------------------
//  TerminalTabBar — horizontal scrollable tab strip
// ---------------------------------------------------------------------------

/// A custom tab bar with close buttons per tab and a trailing "+" button
/// to create new tabs. Tabs scroll horizontally when they overflow.
class TerminalTabBar extends StatelessWidget {
  const TerminalTabBar({
    super.key,
    required this.tabs,
    required this.activeIndex,
    required this.onSelect,
    required this.onClose,
    required this.onNewTab,
  });

  /// All open terminal tabs.
  final List<TerminalTab> tabs;

  /// Index of the currently active tab.
  final int activeIndex;

  /// Called when a tab is tapped.
  final ValueChanged<int> onSelect;

  /// Called when a tab's close button is pressed.
  final ValueChanged<int> onClose;

  /// Called when the "+" new-tab button is pressed.
  final VoidCallback onNewTab;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;

    return Container(
      height: 38,
      color: colorScheme.surfaceContainerHighest,
      child: Row(
        children: [
          // Scrollable tab list.
          Expanded(
            child: ListView.builder(
              scrollDirection: Axis.horizontal,
              itemCount: tabs.length,
              itemBuilder: (context, index) {
                return _TabChip(
                  tab: tabs[index],
                  isActive: index == activeIndex,
                  onTap: () => onSelect(index),
                  onClose: () => onClose(index),
                );
              },
            ),
          ),

          // New tab button.
          _NewTabButton(onPressed: onNewTab),
        ],
      ),
    );
  }
}

// ---------------------------------------------------------------------------
//  _TabChip — individual tab in the bar
// ---------------------------------------------------------------------------

class _TabChip extends StatelessWidget {
  const _TabChip({
    required this.tab,
    required this.isActive,
    required this.onTap,
    required this.onClose,
  });

  final TerminalTab tab;
  final bool isActive;
  final VoidCallback onTap;
  final VoidCallback onClose;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;

    final bgColor = isActive
        ? colorScheme.surface
        : colorScheme.surfaceContainerHighest;
    final fgColor = isActive
        ? colorScheme.onSurface
        : colorScheme.onSurfaceVariant;
    final borderColor = isActive
        ? colorScheme.primary
        : Colors.transparent;

    return GestureDetector(
      onTap: onTap,
      child: Container(
        constraints: const BoxConstraints(
          minWidth: 100,
          maxWidth: 200,
        ),
        decoration: BoxDecoration(
          color: bgColor,
          border: Border(
            bottom: BorderSide(
              color: borderColor,
              width: 2,
            ),
          ),
        ),
        padding: const EdgeInsets.symmetric(horizontal: 12),
        alignment: Alignment.center,
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            // Tab title.
            Flexible(
              child: Text(
                tab.title,
                style: TextStyle(
                  color: fgColor,
                  fontSize: 13,
                  fontWeight: isActive ? FontWeight.w600 : FontWeight.normal,
                ),
                overflow: TextOverflow.ellipsis,
                maxLines: 1,
              ),
            ),

            const SizedBox(width: 6),

            // Close button.
            SizedBox(
              width: 18,
              height: 18,
              child: IconButton(
                padding: EdgeInsets.zero,
                iconSize: 14,
                splashRadius: 10,
                onPressed: onClose,
                icon: Icon(
                  Icons.close,
                  size: 14,
                  color: fgColor.withValues(alpha: 0.6),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

// ---------------------------------------------------------------------------
//  _NewTabButton — the "+" button at the end of the tab bar
// ---------------------------------------------------------------------------

class _NewTabButton extends StatelessWidget {
  const _NewTabButton({required this.onPressed});

  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return SizedBox(
      width: 38,
      height: 38,
      child: IconButton(
        padding: EdgeInsets.zero,
        iconSize: 20,
        onPressed: onPressed,
        tooltip: 'New Tab',
        icon: Icon(
          Icons.add,
          color: colorScheme.onSurfaceVariant,
        ),
      ),
    );
  }
}
