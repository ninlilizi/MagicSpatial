#pragma once

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <string>

namespace MagicSpatial {

// Simple file logger to %APPDATA%\MagicSpatial\spatial.log
inline FILE* GetLogFile() {
    static FILE* s_file = nullptr;
    if (!s_file) {
        wchar_t appdata[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
            std::wstring dir = std::wstring(appdata) + L"\\MagicSpatial";
            CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring path = dir + L"\\spatial.log";
            s_file = _wfopen(path.c_str(), L"a");
        }
    }
    return s_file;
}

inline void LogMsg(const char* fmt, ...) {
    FILE* f = GetLogFile();
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fflush(f);
}

} // namespace MagicSpatial
