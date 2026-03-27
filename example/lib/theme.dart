import 'package:flutter/material.dart';
import 'package:dart_xterm/dart_xterm.dart';

// ---------------------------------------------------------------------------
//  Terminal themes — dark and light variants
// ---------------------------------------------------------------------------

/// Dark terminal theme (VS Code-inspired).
const darkTerminalTheme = TerminalTheme(
  cursor: Color(0XAAAEAFAD),
  selection: Color(0X60AEAFAD),
  foreground: Color(0XFFCCCCCC),
  background: Color(0XFF1E1E1E),
  black: Color(0XFF000000),
  red: Color(0XFFCD3131),
  green: Color(0XFF0DBC79),
  yellow: Color(0XFFE5E510),
  blue: Color(0XFF2472C8),
  magenta: Color(0XFFBC3FBC),
  cyan: Color(0XFF11A8CD),
  white: Color(0XFFE5E5E5),
  brightBlack: Color(0XFF666666),
  brightRed: Color(0XFFF14C4C),
  brightGreen: Color(0XFF23D18B),
  brightYellow: Color(0XFFF5F543),
  brightBlue: Color(0XFF3B8EEA),
  brightMagenta: Color(0XFFD670D6),
  brightCyan: Color(0XFF29B8DB),
  brightWhite: Color(0XFFFFFFFF),
  searchHitBackground: Color(0XFFFFFF2B),
  searchHitBackgroundCurrent: Color(0XFF31FF26),
  searchHitForeground: Color(0XFF000000),
);

/// Light terminal theme (Solarized Light-inspired).
const lightTerminalTheme = TerminalTheme(
  cursor: Color(0XAA586E75),
  selection: Color(0X60586E75),
  foreground: Color(0XFF073642),
  background: Color(0XFFFDF6E3),
  black: Color(0XFF073642),
  red: Color(0XFFDC322F),
  green: Color(0XFF859900),
  yellow: Color(0XFFB58900),
  blue: Color(0XFF268BD2),
  magenta: Color(0XFFD33682),
  cyan: Color(0XFF2AA198),
  white: Color(0XFFEEE8D5),
  brightBlack: Color(0XFF586E75),
  brightRed: Color(0XFFCB4B16),
  brightGreen: Color(0XFF586E75),
  brightYellow: Color(0XFF657B83),
  brightBlue: Color(0XFF839496),
  brightMagenta: Color(0XFF6C71C4),
  brightCyan: Color(0XFF93A1A1),
  brightWhite: Color(0XFFFDF6E3),
  searchHitBackground: Color(0XFFFFFF2B),
  searchHitBackgroundCurrent: Color(0XFF31FF26),
  searchHitForeground: Color(0XFF000000),
);

// ---------------------------------------------------------------------------
//  Material theme data — dark and light variants
// ---------------------------------------------------------------------------

/// Dark Material theme for the app chrome.
final darkAppTheme = ThemeData(
  brightness: Brightness.dark,
  colorSchemeSeed: Colors.blue,
  useMaterial3: true,
);

/// Light Material theme for the app chrome.
final lightAppTheme = ThemeData(
  brightness: Brightness.light,
  colorSchemeSeed: Colors.blue,
  useMaterial3: true,
);

/// Returns the [TerminalTheme] matching the current brightness.
TerminalTheme terminalThemeFor(Brightness brightness) {
  return brightness == Brightness.dark
      ? darkTerminalTheme
      : lightTerminalTheme;
}
