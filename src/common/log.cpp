#include "common/log.h"

#include <cstdarg>
#include <cstdio>

namespace vrlog {

static HANDLE g_file = INVALID_HANDLE_VALUE;
static SRWLOCK g_lock = SRWLOCK_INIT;

void init(HMODULE self)
{
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return;
    // Replace the file name with the log name.
    for (wchar_t* p = path + len; p > path; --p) {
        if (p[-1] == L'\\') { *p = L'\0'; break; }
    }
    lstrcatW(path, L"snowrunner_vr.log");
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void write(const char* fmt, ...)
{
    if (g_file == INVALID_HANDLE_VALUE)
        return;

    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int off = snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u|%5lu] ",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       GetCurrentThreadId());

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line + off, sizeof(line) - off - 2, fmt, args);
    va_end(args);
    if (n < 0)
        return;

    int end = off + ((n < static_cast<int>(sizeof(line)) - off - 2) ? n : static_cast<int>(sizeof(line)) - off - 2);
    line[end++] = '\n';

    AcquireSRWLockExclusive(&g_lock);
    DWORD written;
    WriteFile(g_file, line, static_cast<DWORD>(end), &written, nullptr);
    FlushFileBuffers(g_file);
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace vrlog
