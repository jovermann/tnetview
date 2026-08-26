// Reusable terminal user interface helpers.
//
// Copyright (c) 2026 Johannes Overmann
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

#include "Tui.hpp"
#include <iomanip>
#include <iostream>
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

std::string fitTerminalLine(const std::string& line, size_t width)
{
    if (width == 0 || line.size() <= width)
    {
        return line;
    }
    if (width <= 3)
    {
        return line.substr(0, width);
    }
    return line.substr(0, width - 3) + "...";
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

void printAnsiColorMatrix(std::ostream& os, size_t maxRows)
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
        for (int fg : foregrounds)
        {
            std::string cell = std::to_string(bg) + ";" + std::to_string(fg);
            os << "\033[" << bg << ";" << fg << "m" << std::setw(8) << cell << ansiReset;
        }
        os << "\n";
        printedRows++;
    }
}

}
}
