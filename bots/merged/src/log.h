// log.h - cheap logging. std::cout costs about 500 fuel per character; this builds a line in a plain buffer and writes it in one go.
//
// Reads/writes: stdout (one line per flush). Edit here to change: the line buffer size (600 characters - a longer line is cut), the number format.
// Fuel: about 25 per character written, so a 300-character line costs ~7,500 fuel: log little, and never on a turn that is already heavy.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

class LogLine {
public:
    LogLine& s(const char* text) {
        const std::size_t n = std::strlen(text);
        if (len_ + n < sizeof(buf_) - 2) {
            std::memcpy(buf_ + len_, text, n);
            len_ += n;
        }
        return *this;
    }
    LogLine& n(long long v) {
        char tmp[24];
        int count = 0;
        const bool neg = v < 0;
        unsigned long long u = neg ? 0ULL - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
        do {
            tmp[count++] = static_cast<char>('0' + u % 10);
            u /= 10;
        } while (u);
        if (neg)
            tmp[count++] = '-';
        char out[24];
        for (int i = 0; i < count; ++i)
            out[i] = tmp[count - 1 - i];
        out[count] = 0;
        return s(out);
    }
    LogLine& xy(int x, int y) { return s("(").n(x).s(",").n(y).s(")"); }
    // Writes the line (with a newline) and flushes so nothing is lost if the turn or the game ends.
    void flush() {
        buf_[len_++] = '\n';
        std::fwrite(buf_, 1, len_, stdout);
        std::fflush(stdout);
        len_ = 0;
    }

private:
    char buf_[600];
    std::size_t len_ = 0;
};
