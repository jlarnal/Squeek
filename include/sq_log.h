#ifndef SQ_LOG_H
#define SQ_LOG_H

#include <Arduino.h>
#include <esp_log.h>
#include <cstdarg>

class SqLogClass {
public:
    static void init() {
        s_defaultVprintf = esp_log_set_vprintf(quietVprintf);
    }

    static void setQuiet(bool q) { s_quiet = q; }
    static bool isQuiet() { return s_quiet; }

    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (s_quiet) return;
        va_list args;
        va_start(args, fmt);
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Serial.print(buf);
    }

    void print(const char* s) {
        if (s_quiet) return;
        Serial.print(s);
    }

    void print(char c) {
        if (s_quiet) return;
        Serial.print(c);
    }

    void println(const char* s = "") {
        if (s_quiet) return;
        Serial.println(s);
    }

    void println(int v) {
        if (s_quiet) return;
        Serial.println(v);
    }

    void flush() {
        Serial.flush();
    }

private:
    static bool s_quiet;
    static vprintf_like_t s_defaultVprintf;

    static int quietVprintf(const char* fmt, va_list args) {
        if (s_quiet) return 0;
        if (s_defaultVprintf) return s_defaultVprintf(fmt, args);
        return vprintf(fmt, args);
    }
};

inline bool SqLogClass::s_quiet = false;
inline vprintf_like_t SqLogClass::s_defaultVprintf = nullptr;

inline SqLogClass SqLog;

#endif // SQ_LOG_H
