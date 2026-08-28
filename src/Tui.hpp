// Reusable terminal user interface helpers.
//
// Copyright (c) 2026 Johannes Overmann
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <termios.h>

namespace ut1
{
namespace tui
{

/// ANSI SGR escape sequence that resets foreground, background, and attributes.
extern const char* const ansiReset;
extern const char* const ansiBold;

/// Simple foreground colors without background: standard 30-37 colors.
extern const char* const ansiBlack;
extern const char* const ansiRed;
extern const char* const ansiGreen;
extern const char* const ansiYellow;
extern const char* const ansiBlue;
extern const char* const ansiMagenta;
extern const char* const ansiCyan;
extern const char* const ansiWhite;

/// Simple foreground colors without background: bright 90-97 colors.
extern const char* const ansiGray;
extern const char* const ansiBrightRed;
extern const char* const ansiBrightGreen;
extern const char* const ansiBrightYellow;
extern const char* const ansiBrightBlue;
extern const char* const ansiBrightMagenta;
extern const char* const ansiBrightCyan;
extern const char* const ansiBrightWhite;

/// Foreground/background combinations currently used by treeop.
extern const char* const ansiWhiteOnBlue;
extern const char* const ansiWhiteOnRed;
extern const char* const ansiBlackOnCyan;

/// RAII guard that switches a terminal file descriptor into raw input mode.
///
/// Raw mode disables canonical line buffering and echo so interactive tools can
/// react to individual key presses such as cursor keys. The previous terminal
/// state is restored in the destructor. The destructor also shows the cursor,
/// which protects callers that hide the cursor while rendering a full-screen UI.
class TerminalRawMode
{
public:
    /// Enable raw mode on fd. Throws std::runtime_error if fd is not a terminal
    /// or if terminal settings cannot be read or changed.
    explicit TerminalRawMode(int fd);

    /// Restore previous terminal settings and show the cursor.
    ~TerminalRawMode();

    TerminalRawMode(const TerminalRawMode&) = delete;
    TerminalRawMode& operator=(const TerminalRawMode&) = delete;

private:
    int fd = -1;
    termios oldTermios{};
    bool enabled = false;
};

/// Return terminal height in rows, or 24 when it cannot be queried.
size_t terminalHeight();

/// Return terminal width in columns, or 120 when it cannot be queried.
size_t terminalWidth();

/// Truncate a plain, non-ANSI line to fit width columns.
/// Uses "..." when there is room; callers should apply ANSI colors after this.
std::string fitTerminalLine(const std::string& line, size_t width);

/// Read one byte from stdin. Returns -1 on timeout, EOF, or read error.
/// A negative timeout waits indefinitely.
int readStdinByte(int timeoutMs = -1);

/// Decoded special-key values returned by readKey().
constexpr int keyUp   = 0x100;
constexpr int keyDown = 0x101;
constexpr int keyPageUp = 0x102;
constexpr int keyPageDown = 0x103;

/// Read a key and decode common ANSI cursor and page-key sequences.
int readKey(int timeoutMs = -1);

/// Clear the terminal and move the cursor to the upper-left corner.
void clearScreen();

/// Enter/leave the terminal's alternate screen buffer.
void enterAlternateScreen();
void leaveAlternateScreen();

/// Print a matrix of ANSI background/foreground color combinations.
///
/// Each cell is rendered with the color combination it names, e.g. "41;37".
/// maxRows limits output for small terminals; set it to 0 to print no rows.
/// maxWidth limits columns to avoid terminal wrapping; zero means unlimited.
void printAnsiColorMatrix(std::ostream& os, size_t maxRows, size_t maxWidth = 0);

}
}
