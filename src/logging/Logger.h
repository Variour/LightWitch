#pragma once
#include <Arduino.h>

#include <functional>
#include <vector>

// Values are the persisted/wire representation (Config::logLevel, web UI select) and must not
// be renumbered — VERBOSE was appended after ERROR to keep existing saved settings meaning the
// same. Severity ordering for filtering is handled separately by _rank().
enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3, VERBOSE = 4 };

class Logger {
   public:
    using Sink = std::function<void(LogLevel, const char*)>;

    static void addSink(Sink s) { _sinks.push_back(s); }
    static void setLevel(LogLevel lv) { _minLevel = lv; }
    static LogLevel getLevel() { return _minLevel; }

    static void v(const char* fmt, ...) {
        va_list a;
        va_start(a, fmt);
        _vlog(LogLevel::VERBOSE, fmt, a);
        va_end(a);
    }
    static void d(const char* fmt, ...) {
        va_list a;
        va_start(a, fmt);
        _vlog(LogLevel::DEBUG, fmt, a);
        va_end(a);
    }
    static void i(const char* fmt, ...) {
        va_list a;
        va_start(a, fmt);
        _vlog(LogLevel::INFO, fmt, a);
        va_end(a);
    }
    static void w(const char* fmt, ...) {
        va_list a;
        va_start(a, fmt);
        _vlog(LogLevel::WARN, fmt, a);
        va_end(a);
    }
    static void e(const char* fmt, ...) {
        va_list a;
        va_start(a, fmt);
        _vlog(LogLevel::ERROR, fmt, a);
        va_end(a);
    }

   private:
    static std::vector<Sink> _sinks;
    static LogLevel _minLevel;

    // Severity rank, least to most severe: VERBOSE < DEBUG < INFO < WARN < ERROR.
    static uint8_t _rank(LogLevel lv) {
        switch (lv) {
            case LogLevel::VERBOSE:
                return 0;
            case LogLevel::DEBUG:
                return 1;
            case LogLevel::INFO:
                return 2;
            case LogLevel::WARN:
                return 3;
            case LogLevel::ERROR:
                return 4;
        }
        return 1;
    }

    static void _vlog(LogLevel level, const char* fmt, va_list args) {
        if (_rank(level) < _rank(_minLevel)) return;
        char msg[256];
        vsnprintf(msg, sizeof(msg), fmt, args);
        char buf[272];
        snprintf(buf, sizeof(buf), "[%lu] %s", millis(), msg);
        for (auto& s : _sinks) s(level, buf);
    }
};

inline std::vector<Logger::Sink> Logger::_sinks;
inline LogLevel Logger::_minLevel = LogLevel::INFO;
