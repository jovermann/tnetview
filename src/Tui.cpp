// Reusable terminal user interface helpers.
//
// Copyright (c) 2026 Johannes Overmann
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

#include "Tui.hpp"
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <cwchar>
#include <locale.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace ut1
{
namespace tui
{

const char* const ansiReset = "\033[0m";
const char* const ansiBold = "\033[1m";
const char* const ansiBlack = "\033[30m";
const char* const ansiRed = "\033[31m";
const char* const ansiGreen = "\033[32m";
const char* const ansiYellow = "\033[33m";
const char* const ansiBlue = "\033[34m";
const char* const ansiMagenta = "\033[35m";
const char* const ansiCyan = "\033[36m";
const char* const ansiWhite = "\033[37m";
const char* const ansiGray = "\033[90m";
const char* const ansiBrightRed = "\033[91m";
const char* const ansiBrightGreen = "\033[92m";
const char* const ansiBrightYellow = "\033[93m";
const char* const ansiBrightBlue = "\033[94m";
const char* const ansiBrightMagenta = "\033[95m";
const char* const ansiBrightCyan = "\033[96m";
const char* const ansiBrightWhite = "\033[97m";
const char* const ansiWhiteOnBlue = "\033[44;37m";
const char* const ansiWhiteOnRed = "\033[41;37m";
const char* const ansiBlackOnCyan = "\033[46;30m";

TerminalRawMode::TerminalRawMode(int fd_)
    : TerminalRawMode(fd_, false)
{
}

TerminalRawMode::TerminalRawMode(int fd_, bool disableSignals)
    : fd(fd_)
{
    if (!isatty(fd))
    {
        throw std::runtime_error("--interactive requires a terminal.");
    }
    if (tcgetattr(fd, &oldTermios) != 0)
    {
        throw std::runtime_error("Failed to read terminal settings.");
    }
    termios raw = oldTermios;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    // Let callers handle Ctrl-C as an input key and unwind their terminal guards.
    if (disableSignals) raw.c_lflag &= static_cast<tcflag_t>(~ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSAFLUSH, &raw) != 0)
    {
        throw std::runtime_error("Failed to enable terminal raw mode.");
    }
    enabled = true;
}

TerminalRawMode::~TerminalRawMode()
{
    std::cout << "\033[?25h";
    std::cout.flush();
    if (enabled)
    {
        tcsetattr(fd, TCSAFLUSH, &oldTermios);
    }
}

size_t terminalHeight()
{
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
    {
        return ws.ws_row;
    }
    return 24;
}

size_t terminalWidth()
{
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    {
        return ws.ws_col;
    }
    return 120;
}

namespace
{
struct WidthLocale
{
    locale_t previous{};
    WidthLocale()
    {
        static locale_t utf8 = []
        {
            for (const char* name : {"C.UTF-8", "en_US.UTF-8", "UTF-8"})
                if (locale_t candidate = newlocale(LC_CTYPE_MASK, name, nullptr)) return candidate;
            return locale_t{};
        }();
        if (utf8) previous = uselocale(utf8);
    }
    ~WidthLocale() { if (previous) uselocale(previous); }
};

struct Glyph { size_t bytes, columns; };
Glyph nextGlyph(const std::string& text, size_t offset)
{
    unsigned char first = text[offset];
    size_t length = first < 128 ? 1 : first >= 0xc2 && first <= 0xdf ? 2
        : first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 1;
    if (offset + length > text.size()) return {1, 1};
    uint32_t codepoint = first & (length == 1 ? 0x7f : length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07);
    for (size_t i = 1; i < length; i++)
    {
        unsigned char byte = text[offset + i];
        if ((byte & 0xc0) != 0x80) return {1, 1};
        codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    int columns = wcwidth(static_cast<wchar_t>(codepoint));
    return {length, columns < 0 ? 1 : static_cast<size_t>(columns)};
}
}

size_t terminalTextWidth(const std::string& text)
{
    WidthLocale locale;
    size_t columns = 0;
    for (size_t offset = 0; offset < text.size();)
    {
        auto glyph = nextGlyph(text, offset);
        offset += glyph.bytes;
        columns += glyph.columns;
    }
    return columns;
}

std::string fitTerminalLine(const std::string& line, size_t width)
{
    if (width == 0 || terminalTextWidth(line) <= width) return line;
    WidthLocale locale;
    size_t budget = width > 3 ? width - 3 : width;
    size_t offset = 0, columns = 0;
    while (offset < line.size())
    {
        auto glyph = nextGlyph(line, offset);
        if (columns + glyph.columns > budget) break;
        columns += glyph.columns;
        offset += glyph.bytes;
    }
    return line.substr(0, offset) + (width > 3 ? "..." : "");
}

int readStdinByte(int timeoutMs)
{
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, timeoutMs) <= 0)
    {
        return -1;
    }
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1)
    {
        return -1;
    }
    return static_cast<unsigned char>(c);
}

int readKey(int timeoutMs)
{
    int key = readStdinByte(timeoutMs);
    if (key != 27)
    {
        return key;
    }
    int second = readStdinByte(20);
    if ((second != '[') && (second != 'O'))
    {
        return 27;
    }
    int third = readStdinByte(20);
    if (third == 'A')
    {
        return keyUp;
    }
    if (third == 'B')
    {
        return keyDown;
    }
    if (third == 'C')
    {
        return keyRight;
    }
    if (third == 'D')
    {
        return keyLeft;
    }
    if (third == 'H') return keyHome;
    if (third == 'F') return keyEnd;
    if (third == '1' || third == '4' || third == '7' || third == '8'
        || third == '5' || third == '6')
    {
        int fourth = readStdinByte(20);
        if (fourth == '~')
        {
            if (third == '5') return keyPageUp;
            if (third == '6') return keyPageDown;
            return third == '1' || third == '7' ? keyHome : keyEnd;
        }
    }
    return 27;
}

void clearScreen()
{
    std::cout << "\033[H\033[2J";
}

void enterAlternateScreen()
{
    std::cout << "\033[?1049h\033[?25l" << std::flush;
}

void leaveAlternateScreen()
{
    std::cout << "\033[?1049l\033[?25h" << std::flush;
}

void printAnsiColorMatrix(std::ostream& os, size_t maxRows, size_t maxWidth)
{
    const std::vector<int> foregrounds = {30, 31, 32, 33, 34, 35, 36, 37, 90, 91, 92, 93, 94, 95, 96, 97};
    const std::vector<int> backgrounds = {40, 41, 42, 43, 44, 45, 46, 47, 100, 101, 102, 103, 104, 105, 106, 107};
    size_t printedRows = 0;
    for (int bg : backgrounds)
    {
        if (printedRows >= maxRows)
        {
            break;
        }
        os << ansiGray << "bg " << bg << " " << ansiReset;
        size_t printedWidth = std::string("bg ").size() + std::to_string(bg).size() + 1;
        for (int fg : foregrounds)
        {
            if (maxWidth && printedWidth + 8 > maxWidth)
            {
                break;
            }
            std::string cell = std::to_string(bg) + ";" + std::to_string(fg);
            os << "\033[" << bg << ";" << fg << "m" << std::setw(8) << cell << ansiReset;
            printedWidth += 8;
        }
        os << "\n";
        printedRows++;
    }
}

}
}
