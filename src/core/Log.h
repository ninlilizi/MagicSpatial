#pragma once

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <string>

namespace MagicSpatial {

// Simple file logger. The log lives beside the DLL as spatial.log so that the
// audio engine's instance (audiodg runs as LOCAL SERVICE, whose roaming
// profile the user cannot read) and the editor's instance write to one file
// the user can open. %APPDATA%\MagicSpatial\spatial.log is the fallback if
// the DLL's folder is not writable.
inline FILE* GetLogFile() {
    static FILE* s_file = nullptr;
    if (!s_file) {
        HMODULE self = nullptr;
        wchar_t modulePath[MAX_PATH];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&GetLogFile), &self) &&
            GetModuleFileNameW(self, modulePath, MAX_PATH) > 0) {
            std::wstring path(modulePath);
            size_t slash = path.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                path = path.substr(0, slash + 1) + L"spatial.log";
                s_file = _wfopen(path.c_str(), L"a");
            }
        }
        if (!s_file) {
            wchar_t appdata[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
                std::wstring dir = std::wstring(appdata) + L"\\MagicSpatial";
                CreateDirectoryW(dir.c_str(), nullptr);
                std::wstring path = dir + L"\\spatial.log";
                s_file = _wfopen(path.c_str(), L"a");
            }
        }
    }
    return s_file;
}

inline void LogMsg(const char* fmt, ...) {
    FILE* f = GetLogFile();
    if (!f) return;
    // Prefix every line with the process ID so we can tell whether E-APO's
    // multiple plugin instances share a process (audiodg) or are split across
    // the editor GUI and the audio engine — which decides whether a single
    // shared ISpatialAudioClient is even possible.
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(f, "%02u:%02u:%02u.%03u [pid %lu] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 static_cast<unsigned long>(GetCurrentProcessId()));
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fflush(f);
}

} // namespace MagicSpatial
