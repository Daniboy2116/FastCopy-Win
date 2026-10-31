// Minimal copy/move engine. Plans everything first (no writes), so any unsupported case can
// fall back to Windows untouched; then executes: renames, folders, parallel file copies
// (moves verify each copy byte by byte), and only then deletes the sources of cross-volume moves.
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wctype.h>
#include "engine.h"

#define BIG_FILE (256ull * 1024 * 1024)  // above this, copy without the system cache
#define MAX_WORKERS 8
#define VERIFY_BLOCK (1024 * 1024)

enum { J_RENAME, J_MKDIR, J_COPY };

typedef struct {
    int kind;
    BOOL fromMove;
    BOOL done;  // copied (and verified, for moves)
    PWSTR src, dst;
    ULONGLONG size;
} Job;

typedef struct {
    Job *v;
    int n, cap;
} Plan;

static PWSTR Join(PCWSTR a, PCWSTR b)
{
    size_t la = wcslen(a), cap = la + wcslen(b) + 2;
    PWSTR r = malloc(cap * sizeof(WCHAR));
    wcscpy_s(r, cap, a);
    if (la && a[la - 1] != L'\\')
        wcscat_s(r, cap, L"\\");
    wcscat_s(r, cap, b);
    return r;
}

static BOOL Push(Plan *p, int kind, BOOL fromMove, PCWSTR src, PCWSTR dst, ULONGLONG size)
{
    // shortcut: paths >= MAX_PATH fall back to Windows; add \\?\ handling if that shows up in real use.
    if (wcslen(src) >= MAX_PATH || wcslen(dst) >= MAX_PATH) {
        FcLog(L"fallback: path too long %s", src);
        return FALSE;
    }
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->v = realloc(p->v, p->cap * sizeof(Job));
    }
    Job *j = &p->v[p->n++];
    j->kind = kind;
    j->fromMove = fromMove;
    j->done = FALSE;
    j->src = _wcsdup(src);
    j->dst = _wcsdup(dst);
    j->size = size;
    return TRUE;
}

static BOOL PlanDir(Plan *p, PCWSTR src, PCWSTR dst, BOOL move, FcProgress *prog)
{
    PWSTR pattern = Join(src, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        FcLog(L"fallback: cannot list %s (%lu)", src, GetLastError());
        return FALSE;
    }
    BOOL ok = TRUE;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            FcLog(L"fallback: reparse point %s\\%s", src, fd.cFileName);
            ok = FALSE;
            break;
        }
        PWSTR s = Join(src, fd.cFileName), d = Join(dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = Push(p, J_MKDIR, move, s, d, 0) && PlanDir(p, s, d, move, prog);
        else
            ok = Push(p, J_COPY, move, s, d, ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        free(s);
        free(d);
    } while (ok && !prog->cancel && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static BOOL IsDrivePath(PCWSTR p)
{
    return iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\';
}

static BOOL SameVolume(PCWSTR a, PCWSTR b)
{
    WCHAR va[MAX_PATH], vb[MAX_PATH];
    return GetVolumePathNameW(a, va, MAX_PATH) && GetVolumePathNameW(b, vb, MAX_PATH) && !_wcsicmp(va, vb);
}

static BOOL PlanItem(Plan *p, const FcItem *it, FcProgress *prog)
{
    if (!IsDrivePath(it->src) || !IsDrivePath(it->dstDir)) {
        FcLog(L"fallback: not a local drive path %s -> %s", it->src, it->dstDir);
        return FALSE;
    }
    PCWSTR name = it->newName && *it->newName ? it->newName : PathFindFileNameW(it->src);
    PWSTR dst = Join(it->dstDir, name);
    DWORD attr = GetFileAttributesW(it->src);
    size_t ls = wcslen(it->src);
    BOOL ok = FALSE;

    if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
        FcLog(L"fallback: target exists %s", dst);  // Windows shows its replace/skip dialog
    } else if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        FcLog(L"fallback: source missing or reparse point %s", it->src);
    } else if ((attr & FILE_ATTRIBUTE_DIRECTORY) && !_wcsnicmp(dst, it->src, ls) && dst[ls] == L'\\') {
        FcLog(L"fallback: destination inside source %s", dst);
    } else if (it->move && SameVolume(it->src, it->dstDir)) {
        ok = Push(p, J_RENAME, TRUE, it->src, dst, 0);
    } else if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        ok = Push(p, J_MKDIR, it->move, it->src, dst, 0) && PlanDir(p, it->src, dst, it->move, prog);
    } else {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        ok = GetFileAttributesExW(it->src, GetFileExInfoStandard, &fa) &&
             Push(p, J_COPY, it->move, it->src, dst, ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
    }
    free(dst);
    return ok;
}

// Blocks while paused; FALSE once cancelled.
static BOOL WaitIfPaused(FcProgress *prog)
{
    while (prog->paused && !prog->cancel)
        Sleep(100);
    return !prog->cancel;
}

static void SetCurrent(FcProgress *prog, PCWSTR path)
{
    AcquireSRWLockExclusive(&prog->lock);
    wcscpy_s(prog->current, MAX_PATH, path);
    ReleaseSRWLockExclusive(&prog->lock);
}

typedef struct {
    FcProgress *prog;
    ULONGLONG reported;
} ChunkCtx;

static COPYFILE2_MESSAGE_ACTION CALLBACK OnChunk(const COPYFILE2_MESSAGE *m, PVOID arg)
{
    ChunkCtx *cc = arg;
    if (m->Type == COPYFILE2_CALLBACK_CHUNK_FINISHED) {
        ULONGLONG total = m->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart;
        InterlockedAdd64(&cc->prog->bytesDone, (LONG64)(total - cc->reported));
        cc->reported = total;
    }
    return WaitIfPaused(cc->prog) ? COPYFILE2_PROGRESS_CONTINUE : COPYFILE2_PROGRESS_CANCEL;
}

// Byte-by-byte comparison, used before deleting the source of a move.
static BOOL SameContent(PCWSTR a, PCWSTR b, FcProgress *prog)
{
    HANDLE ha = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    HANDLE hb = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    char *ba = malloc(VERIFY_BLOCK), *bb = malloc(VERIFY_BLOCK);
    BOOL same = ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE && ba && bb;
    DWORD ra = 0, rb = 0;
    while (same && WaitIfPaused(prog)) {
        same = ReadFile(ha, ba, VERIFY_BLOCK, &ra, NULL) && ReadFile(hb, bb, VERIFY_BLOCK, &rb, NULL) &&
               ra == rb && !memcmp(ba, bb, ra);
        InterlockedAdd64(&prog->bytesDone, ra);
        if (!ra)
            break;
    }
    if (prog->cancel)
        same = FALSE;
    free(ba);
    free(bb);
    if (ha != INVALID_HANDLE_VALUE)
        CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE)
        CloseHandle(hb);
    return same;
}

typedef struct {
    Job *v;
    int *idx;
    LONG n;
    FcProgress *prog;
    volatile LONG next;
    volatile LONG err;
} CopyCtx;

static DWORD WINAPI CopyWorker(LPVOID arg)
{
    CopyCtx *c = arg;
    for (;;) {
        LONG i = InterlockedIncrement(&c->next) - 1;
        if (i >= c->n || c->err || c->prog->cancel)
            return 0;
        Job *j = &c->v[c->idx[i]];
        SetCurrent(c->prog, j->src);

        ChunkCtx cc = { c->prog, 0 };
        COPYFILE2_EXTENDED_PARAMETERS ep = { sizeof(ep) };
        ep.dwCopyFlags = COPY_FILE_FAIL_IF_EXISTS | (j->size >= BIG_FILE ? COPY_FILE_NO_BUFFERING : 0);
        ep.pProgressRoutine = OnChunk;
        ep.pvCallbackContext = &cc;
        HRESULT hr = CopyFile2(j->src, j->dst, &ep);
        if (FAILED(hr)) {
            if (!c->prog->cancel) {
                FcLog(L"copy failed hr=0x%08lX %s", hr, j->src);
                InterlockedCompareExchange(&c->err, hr, 0);
            }
            continue;
        }
        if (cc.reported < j->size)  // small files may finish without a chunk callback
            InterlockedAdd64(&c->prog->bytesDone, (LONG64)(j->size - cc.reported));

        if (j->fromMove && !SameContent(j->src, j->dst, c->prog)) {
            if (!c->prog->cancel) {
                FcLog(L"verify failed, keeping source %s", j->src);
                InterlockedCompareExchange(&c->err, E_FAIL, 0);
            }
            continue;
        }
        j->done = TRUE;
        InterlockedIncrement(&c->prog->filesDone);
    }
}

static BOOL RunCopies(Plan *p, int ncopy, FcProgress *prog)
{
    CopyCtx c = { p->v, malloc(ncopy * sizeof(int)), ncopy, prog, 0, 0 };
    for (int i = 0, k = 0; i < p->n; i++)
        if (p->v[i].kind == J_COPY)
            c.idx[k++] = i;

    HANDLE th[MAX_WORKERS];
    int nt = 0;
    while (nt < MAX_WORKERS && nt < ncopy && (th[nt] = CreateThread(NULL, 0, CopyWorker, &c, 0, NULL)))
        nt++;
    if (nt)
        WaitForMultipleObjects(nt, th, TRUE, INFINITE);
    else
        CopyWorker(&c);
    for (int t = 0; t < nt; t++)
        CloseHandle(th[t]);
    free(c.idx);
    return c.err == 0;
}

static BOOL ForceDelete(PCWSTR path, BOOL dir)
{
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);  // read-only sources would otherwise refuse
    return dir ? RemoveDirectoryW(path) : DeleteFileW(path);
}

// Removes the sources of verified moved files, then the source folders left empty.
static BOOL DeleteMovedSources(Plan *p, BOOL quietDirs)
{
    BOOL ok = TRUE;
    for (int i = 0; i < p->n; i++)
        if (p->v[i].kind == J_COPY && p->v[i].fromMove && p->v[i].done && !ForceDelete(p->v[i].src, FALSE)) {
            FcLog(L"delete source failed (%lu) %s", GetLastError(), p->v[i].src);
            ok = FALSE;
        }
    for (int i = p->n - 1; i >= 0; i--)
        if (p->v[i].kind == J_MKDIR && p->v[i].fromMove && !ForceDelete(p->v[i].src, TRUE) && !quietDirs) {
            FcLog(L"remove source dir failed (%lu) %s", GetLastError(), p->v[i].src);
            ok = FALSE;
        }
    return ok;
}

static void Notify(const FcItem *items, int n)
{
    for (int i = 0; i < n; i++) {
        if (i == 0 || _wcsicmp(items[i].dstDir, items[i - 1].dstDir))
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, items[i].dstDir, NULL);
        if (items[i].move) {
            WCHAR parent[MAX_PATH];
            wcscpy_s(parent, MAX_PATH, items[i].src);
            PathRemoveFileSpecW(parent);
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, parent, NULL);
        }
    }
}

FcResult FcRun(const FcItem *items, int n, FcProgress *prog)
{
    Plan p = { 0 };
    FcResult res = FC_FALLBACK;
    ULONGLONG bytes = 0;
    int i, ncopy = 0;

    prog->phase = FC_PHASE_PLAN;
    for (i = 0; i < n; i++) {
        BOOL ok = PlanItem(&p, &items[i], prog);
        if (prog->cancel) {
            res = FC_CANCELLED;
            goto out;
        }
        if (!ok)
            goto out;
    }
    for (i = 0; i < p.n; i++)
        if (p.v[i].kind == J_COPY) {
            bytes += p.v[i].size;
            ncopy++;
        }
    // shortcut: assumes all items share one destination volume (true for Explorer drops/pastes).
    ULARGE_INTEGER freeBytes;
    if (bytes && (!GetDiskFreeSpaceExW(items[0].dstDir, &freeBytes, NULL, NULL) || freeBytes.QuadPart < bytes)) {
        FcLog(L"fallback: not enough free space for %llu bytes", bytes);
        goto out;
    }
    LONG64 total = 0;
    for (i = 0; i < p.n; i++)
        if (p.v[i].kind == J_COPY)
            total += p.v[i].fromMove ? 2 * p.v[i].size : p.v[i].size;
    prog->bytesTotal = total;
    prog->filesTotal = ncopy;
    prog->phase = FC_PHASE_COPY;

    // From here on we write: failures are FC_ERROR and move sources stay in place.
    res = FC_ERROR;
    for (i = 0; i < p.n; i++) {
        Job *j = &p.v[i];
        if (prog->cancel) {
            res = FC_CANCELLED;
            goto out;
        }
        if (j->kind == J_RENAME && !MoveFileExW(j->src, j->dst, 0)) {
            FcLog(L"rename failed (%lu) %s", GetLastError(), j->src);
            goto out;
        }
        if (j->kind == J_MKDIR && !CreateDirectoryExW(j->src, j->dst, NULL)) {
            FcLog(L"mkdir failed (%lu) %s", GetLastError(), j->dst);
            goto out;
        }
    }
    BOOL copied = !ncopy || RunCopies(&p, ncopy, prog);
    prog->phase = FC_PHASE_FINISH;
    if (prog->cancel) {
        // A move cancelled mid-verify leaves a full but unverified copy: drop it, the source is intact.
        for (i = 0; i < p.n; i++)
            if (p.v[i].kind == J_COPY && p.v[i].fromMove && !p.v[i].done)
                ForceDelete(p.v[i].dst, FALSE);
        DeleteMovedSources(&p, TRUE);  // only verified files; folders still holding the rest stay
        res = FC_CANCELLED;
        goto out;
    }
    if (!copied)
        goto out;
    res = DeleteMovedSources(&p, FALSE) ? FC_OK : FC_ERROR;

out:
    if (res != FC_FALLBACK)
        Notify(items, n);
    for (i = 0; i < p.n; i++) {
        free(p.v[i].src);
        free(p.v[i].dst);
    }
    free(p.v);
    return res;
}
