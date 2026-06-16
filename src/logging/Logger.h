#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

class Logger {
public:
    using Sink = std::function<void(LogLevel, const char*)>;

    static void addSink(Sink s)       { _sinks.push_back(s); }
    static void setLevel(LogLevel lv) { _minLevel = lv; }
    static LogLevel getLevel()        { return _minLevel; }

    static void d(const char* fmt, ...) { va_list a; va_start(a,fmt); _vlog(LogLevel::DEBUG, fmt, a); va_end(a); }
    static void i(const char* fmt, ...) { va_list a; va_start(a,fmt); _vlog(LogLevel::INFO,  fmt, a); va_end(a); }
    static void w(const char* fmt, ...) { va_list a; va_start(a,fmt); _vlog(LogLevel::WARN,  fmt, a); va_end(a); }
    static void e(const char* fmt, ...) { va_list a; va_start(a,fmt); _vlog(LogLevel::ERROR, fmt, a); va_end(a); }

private:
    static std::vector<Sink> _sinks;
    static LogLevel           _minLevel;

    static void _vlog(LogLevel level, const char* fmt, va_list args) {
        if (level < _minLevel) return;
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, args);
        for (auto& s : _sinks) s(level, buf);
    }
};

inline std::vector<Logger::Sink> Logger::_sinks;
inline LogLevel                   Logger::_minLevel = LogLevel::INFO;
