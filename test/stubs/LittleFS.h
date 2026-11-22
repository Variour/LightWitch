#pragma once
// Host stand-in for LittleFS: files live in memory, set them with
// LittleFS.put(path, content) before the code under test opens them.
#include <stddef.h>
#include <string.h>

#include <map>
#include <string>

class File {
   public:
    File() = default;
    explicit File(const std::string* data) : _data(data) {}
    explicit operator bool() const { return _data != nullptr; }
    size_t size() const { return _data ? _data->size() : 0; }
    int read() { return _data && _pos < _data->size() ? (unsigned char)(*_data)[_pos++] : -1; }
    size_t readBytes(char* buf, size_t len) {
        if (!_data) return 0;
        size_t n = _data->size() - _pos < len ? _data->size() - _pos : len;
        memcpy(buf, _data->data() + _pos, n);
        _pos += n;
        return n;
    }
    void close() { _data = nullptr; }

   private:
    const std::string* _data = nullptr;
    size_t _pos = 0;
};

class LittleFSClass {
   public:
    File open(const char* path, const char* = "r") {
        auto it = _files.find(path);
        return it == _files.end() ? File() : File(&it->second);
    }
    void put(const char* path, const std::string& content) { _files[path] = content; }
    void clear() { _files.clear(); }

   private:
    std::map<std::string, std::string> _files;
};

inline LittleFSClass LittleFS;
