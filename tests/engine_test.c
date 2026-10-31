// Self-check for the engine, run outside Explorer. Uses %TEMP% and, if present, D:\ for cross-volume.
#include <windows.h>
#include <stdio.h>
#include "../src/engine/engine.h"

static int failures;
#define CHECK(c) do { if (!(c)) { wprintf(L"FAIL line %d: %S\n", __LINE__, #c); failures++; } } while (0)

static void WriteFileText(PCWSTR path, const char *text, DWORD extra)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD w;
    WriteFile(h, text, (DWORD)strlen(text), &w, NULL);
    for (DWORD i = 0; i < extra; i += 4096) {
        static char block[4096];
        WriteFile(h, block, sizeof(block), &w, NULL);
    }
    CloseHandle(h);
}

static BOOL SameContent(PCWSTR a, PCWSTR b)
{
    HANDLE ha = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hb = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    BOOL same = ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE;
    static char ba[65536], bb[65536];
    DWORD ra, rb;
    while (same && ReadFile(ha, ba, sizeof(ba), &ra, NULL) && ReadFile(hb, bb, sizeof(bb), &rb, NULL) && (ra || rb))
        same = ra == rb && !memcmp(ba, bb, ra);
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return same;
}

static BOOL Exists(PCWSTR p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static void Path(PWSTR out, PCWSTR root, PCWSTR rel) { swprintf_s(out, MAX_PATH, L"%s\\%s", root, rel); }

static FcProgress *Reset(FcProgress *p) { ZeroMemory(p, sizeof(*p)); return p; }

static void BuildTree(PCWSTR root)
{
    WCHAR p[MAX_PATH];
    Path(p, root, L"src"); CreateDirectoryW(p, NULL);
    Path(p, root, L"src\\tree"); CreateDirectoryW(p, NULL);
    Path(p, root, L"src\\tree\\sub"); CreateDirectoryW(p, NULL);
    Path(p, root, L"src\\tree\\sub\\empty"); CreateDirectoryW(p, NULL);
    for (int i = 0; i < 50; i++) {
        WCHAR name[64];
        swprintf_s(name, 64, L"src\\tree\\f%02d.txt", i);
        Path(p, root, name);
        WriteFileText(p, "small file", (DWORD)i * 1024);
    }
    Path(p, root, L"src\\tree\\sub\\big.bin"); WriteFileText(p, "big", 8 * 1024 * 1024);
    Path(p, root, L"src\\tree\\sub\\ro.txt"); WriteFileText(p, "read only", 0);
    SetFileAttributesW(p, FILE_ATTRIBUTE_READONLY);
    Path(p, root, L"src\\single.txt"); WriteFileText(p, "single", 0);
    Path(p, root, L"dst"); CreateDirectoryW(p, NULL);
}

static void RunSuite(PCWSTR root, PCWSTR otherRoot)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    Path(src, root, L"src\\tree");
    Path(dst, otherRoot, L"dst");
    FcProgress pr;

    // 1. copy a folder tree
    FcItem copy = { FALSE, src, dst, NULL };
    CHECK(FcRun(&copy, 1, Reset(&pr)) == FC_OK);
    CHECK(pr.bytesTotal > 8 * 1024 * 1024 && pr.bytesDone == pr.bytesTotal);
    CHECK(pr.filesTotal == 52 && pr.filesDone == 52);
    Path(a, root, L"src\\tree\\sub\\big.bin"); Path(b, otherRoot, L"dst\\tree\\sub\\big.bin");
    CHECK(SameContent(a, b));
    Path(a, root, L"src\\tree\\f49.txt"); Path(b, otherRoot, L"dst\\tree\\f49.txt");
    CHECK(SameContent(a, b));
    Path(b, otherRoot, L"dst\\tree\\sub\\empty");
    CHECK(Exists(b));
    CHECK(Exists(src));

    // 2. same copy again: target exists, must fall back without writing
    CHECK(FcRun(&copy, 1, Reset(&pr)) == FC_FALLBACK);

    // 3. copy a single file with a new name
    Path(a, root, L"src\\single.txt");
    FcItem renamed = { FALSE, a, dst, L"renamed.txt" };
    CHECK(FcRun(&renamed, 1, Reset(&pr)) == FC_OK);
    Path(b, otherRoot, L"dst\\renamed.txt");
    CHECK(SameContent(a, b));

    // 4. move the tree into dst\moved: sources must disappear, content must arrive
    WCHAR movedDir[MAX_PATH];
    Path(movedDir, otherRoot, L"dst\\moved");
    CreateDirectoryW(movedDir, NULL);
    // 4a. cancelled before starting: nothing written, nothing deleted
    FcItem move = { TRUE, src, movedDir, NULL };
    Reset(&pr)->cancel = 1;
    CHECK(FcRun(&move, 1, &pr) == FC_CANCELLED);
    CHECK(Exists(src));
    Path(b, otherRoot, L"dst\\moved\\tree");
    CHECK(!Exists(b));

    CHECK(FcRun(&move, 1, Reset(&pr)) == FC_OK);
    CHECK(pr.bytesDone == pr.bytesTotal);
    CHECK(!Exists(src));
    Path(b, otherRoot, L"dst\\moved\\tree\\sub\\big.bin");
    Path(a, otherRoot, L"dst\\tree\\sub\\big.bin");
    CHECK(SameContent(a, b));
    Path(b, otherRoot, L"dst\\moved\\tree\\sub\\ro.txt");
    CHECK(Exists(b));

    // 5. folder into itself must fall back
    Path(a, otherRoot, L"dst\\moved");
    Path(b, otherRoot, L"dst\\moved\\tree");
    FcItem self = { FALSE, a, b, NULL };
    CHECK(FcRun(&self, 1, Reset(&pr)) == FC_FALLBACK);
}

static void Nuke(PCWSTR dir)
{
    WCHAR cmd[MAX_PATH + 32];
    swprintf_s(cmd, _countof(cmd), L"cmd /c attrib -r \"%s\\*\" /s /d >nul & rd /s /q \"%s\"", dir, dir);
    _wsystem(cmd);
}

int wmain(void)
{
    WCHAR tmp[MAX_PATH], root[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(root, MAX_PATH, L"%sfc_test_%llu", tmp, GetTickCount64());
    CreateDirectoryW(root, NULL);

    wprintf(L"same volume: %s\n", root);
    BuildTree(root);
    RunSuite(root, root);
    Nuke(root);

    if (GetDriveTypeW(L"D:\\") == DRIVE_FIXED) {
        WCHAR other[MAX_PATH];
        swprintf_s(other, MAX_PATH, L"D:\\fc_test_%llu", GetTickCount64());
        CreateDirectoryW(root, NULL);
        CreateDirectoryW(other, NULL);
        wprintf(L"cross volume: %s -> %s\n", root, other);
        BuildTree(root);
        Path(tmp, other, L"dst"); CreateDirectoryW(tmp, NULL);
        RunSuite(root, other);
        Nuke(root);
        Nuke(other);
    }

    wprintf(failures ? L"%d FAILURES\n" : L"ALL TESTS PASSED\n", failures);
    return failures != 0;
}
