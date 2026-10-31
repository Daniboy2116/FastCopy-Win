#include <windows.h>
#include <stdio.h>
#include "engine.h"

static SRWLOCK gLogLock = SRWLOCK_INIT;

// Appends one UTF-8 line to %TEMP%\fastcopy.log.
void FcLog(const wchar_t *fmt, ...)
{
    wchar_t msg[2048], path[MAX_PATH], line[2200];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = swprintf_s(line, _countof(line), L"%02d:%02d:%02d.%03d tid=%lu %s\r\n",
                       t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    char utf8[6600];
    int len = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof(utf8), NULL, NULL);
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, MAX_PATH, L"fastcopy.log");

    AcquireSRWLockExclusive(&gLogLock);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, utf8, len, &w, NULL);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&gLogLock);
}
