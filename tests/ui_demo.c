// Runs the real progress window outside Explorer, one copy per file, all at once, to try the UI
// without restarting Explorer. Usage: ui_demo.exe <dst folder> <file> [<file> ...]
// A number N in %TEMP%\fastcopy_slow throttles each file to N MB/s so the window stays up.
#include <windows.h>
#include "../src/hook/ui.h"
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")  // comctl32 v6, as in Explorer

HINSTANCE gInst;
static PCWSTR gDst;

static DWORD WINAPI Copy(LPVOID arg)
{
    FcItem item = { FALSE, (PWSTR)arg, (PWSTR)gDst, NULL };
    FcProgress prog = { 0 };
    InitializeSRWLock(&prog.lock);
    RunWithProgress(&item, 1, &prog);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    HANDLE th[32];
    int n = 0;
    gInst = GetModuleHandleW(NULL);
    gDst = argc > 1 ? argv[1] : L"";
    for (int i = 2; i < argc && n < 32; i++)
        th[n++] = CreateThread(NULL, 0, Copy, argv[i], 0, NULL);
    if (n)
        WaitForMultipleObjects(n, th, TRUE, INFINITE);
    return 0;
}
