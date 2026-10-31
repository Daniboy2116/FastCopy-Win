// FastCopy agent: runs in the user's session (started at logon from HKCU\...\Run) and keeps
// fc_hook.dll (next to this exe) loaded in every explorer.exe of the session, including
// Explorer restarts. Runs with the user's rights on purpose: no SYSTEM process loads our DLL.
//   fc_agent.exe          watch and inject (single instance)
//   fc_agent.exe --once   inject into current explorer processes and exit
#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include "../engine/engine.h"

#define HOOK_DLL L"fc_hook.dll"
#define MAX_KNOWN 64
#define MIN_AGE_MS 2000  // let a fresh explorer.exe finish starting before injecting

static BOOL HasModule(DWORD pid, PCWSTR name)
{
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (s == INVALID_HANDLE_VALUE)
        return FALSE;  // injecting twice is harmless: LoadLibrary only bumps the refcount
    MODULEENTRY32W me = { sizeof(me) };
    BOOL found = FALSE;
    for (BOOL m = Module32FirstW(s, &me); m && !found; m = Module32NextW(s, &me))
        found = !_wcsicmp(me.szModule, name);
    CloseHandle(s);
    return found;
}

static ULONGLONG AgeMs(HANDLE proc)
{
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(proc, &created, &exited, &kernel, &user))
        return 0;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER c = { created.dwLowDateTime, created.dwHighDateTime }, n = { now.dwLowDateTime, now.dwHighDateTime };
    return (n.QuadPart - c.QuadPart) / 10000;
}

static BOOL InjectInto(HANDLE proc, DWORD pid, PCWSTR dll)
{
    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BOOL ok = FALSE;
    if (remote && WriteProcessMemory(proc, remote, dll, bytes, NULL)) {
        // kernel32 is mapped at the same address in every process of a boot session.
        LPTHREAD_START_ROUTINE loadLib =
            (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE th = CreateRemoteThread(proc, NULL, 0, loadLib, remote, 0, NULL);
        if (th) {
            WaitForSingleObject(th, 10000);
            DWORD code = 0;
            GetExitCodeThread(th, &code);
            ok = code != 0;  // truncated HMODULE; 0 means LoadLibraryW failed
            CloseHandle(th);
        }
    }
    FcLog(L"agent: inject pid=%lu %s (%lu)", pid, ok ? L"OK" : L"FAILED", ok ? 0 : GetLastError());
    if (remote)
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

// Injects into explorer.exe processes of this session not seen before. `known` holds the pids
// already handled and is replaced with the pids alive now.
static void Sweep(PCWSTR dll, DWORD *known, int *nknown)
{
    DWORD mySession = 0, alive[MAX_KNOWN];
    int nalive = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        DWORD pid = pe.th32ProcessID, session;
        if (_wcsicmp(pe.szExeFile, L"explorer.exe") || !ProcessIdToSessionId(pid, &session) || session != mySession)
            continue;
        BOOL seen = FALSE;
        for (int i = 0; i < *nknown && !seen; i++)
            seen = known[i] == pid;
        if (!seen) {
            HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                      PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
            if (!proc)
                continue;  // not ours (e.g. elevated Explorer); retried next sweep
            if (AgeMs(proc) < MIN_AGE_MS) {
                CloseHandle(proc);
                continue;
            }
            if (!HasModule(pid, HOOK_DLL))
                InjectInto(proc, pid, dll);
            CloseHandle(proc);
        }
        if (nalive < MAX_KNOWN)
            alive[nalive++] = pid;
    }
    CloseHandle(snap);
    memcpy(known, alive, nalive * sizeof(DWORD));
    *nknown = nalive;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)inst; (void)prev; (void)show;
    WCHAR dll[MAX_PATH];
    GetModuleFileNameW(NULL, dll, MAX_PATH);
    PathRemoveFileSpecW(dll);
    PathAppendW(dll, HOOK_DLL);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        FcLog(L"agent: %s not found", dll);
        return 1;
    }

    DWORD known[MAX_KNOWN];
    int nknown = 0;
    if (wcsstr(cmd, L"--once")) {
        Sweep(dll, known, &nknown);
        return 0;
    }

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\FastCopyAgent");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    FcLog(L"agent: started, dll=%s", dll);
    // shortcut: 1 s polling; switch to a process-start event (WMI/ETW) if the delay ever matters.
    for (;;) {
        Sweep(dll, known, &nknown);
        Sleep(1000);
    }
    (void)mutex;
}
