// Injected into explorer.exe. Records what Explorer queues on IFileOperation and, at
// PerformOperations, runs our engine instead of Windows' copy. Anything we don't fully
// understand falls back to the original PerformOperations untouched.
// Kill switch: create %TEMP%\fastcopy_off to make every operation native.
#define _WIN32_WINNT 0x0A00
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include "MinHook.h"
#include "ui.h"

HINSTANCE gInst;

typedef struct {
    IFileOperation *op;
    FcItem *items;
    int n, cap;
    BOOL unsupported;
} OpEntry;

#define MAX_OPS 64
static OpEntry gOps[MAX_OPS];
static SRWLOCK gOpsLock = SRWLOCK_INIT;

// Caller holds gOpsLock.
static OpEntry *FindEntry(IFileOperation *op, BOOL create)
{
    OpEntry *empty = NULL;
    for (int i = 0; i < MAX_OPS; i++) {
        if (gOps[i].op == op)
            return &gOps[i];
        if (!gOps[i].op && !empty)
            empty = &gOps[i];
    }
    if (create && empty)
        empty->op = op;
    return create ? empty : NULL;
}

static void FreeEntry(OpEntry *e)
{
    for (int i = 0; i < e->n; i++) {
        free(e->items[i].src);
        free(e->items[i].dstDir);
        free(e->items[i].newName);
    }
    free(e->items);
    ZeroMemory(e, sizeof(*e));
}

static void MarkUnsupported(IFileOperation *op, PCWSTR why)
{
    AcquireSRWLockExclusive(&gOpsLock);
    OpEntry *e = FindEntry(op, TRUE);
    if (e)
        e->unsupported = TRUE;
    ReleaseSRWLockExclusive(&gOpsLock);
    FcLog(L"op=%p unsupported: %s", op, why);
}

static PWSTR FsPath(IShellItem *si)
{
    PWSTR p = NULL;
    if (!si || FAILED(IShellItem_GetDisplayName(si, SIGDN_FILESYSPATH, &p)))
        return NULL;
    PWSTR r = _wcsdup(p);
    CoTaskMemFree(p);
    return r;
}

static void AddItem(IFileOperation *op, BOOL move, IShellItem *src, IShellItem *dest, LPCWSTR newName)
{
    PWSTR s = FsPath(src), d = FsPath(dest);
    if (!s || !d) {
        free(s);
        free(d);
        MarkUnsupported(op, L"item is not a file system path");
        return;
    }
    AcquireSRWLockExclusive(&gOpsLock);
    OpEntry *e = FindEntry(op, TRUE);
    if (e) {
        if (e->n == e->cap) {
            e->cap = e->cap ? e->cap * 2 : 16;
            e->items = realloc(e->items, e->cap * sizeof(FcItem));
        }
        FcItem it = { move, s, d, newName ? _wcsdup(newName) : NULL };
        e->items[e->n++] = it;
    }
    ReleaseSRWLockExclusive(&gOpsLock);
    if (!e) {
        free(s);
        free(d);
        FcLog(L"op table full, op=%p stays native", op);
    }
}

// Explorer passes IPersistIDList (one call per item); other callers may pass arrays or data objects.
static void AddItems(IFileOperation *op, BOOL move, IUnknown *punk, IShellItem *dest)
{
    IPersistIDList *pl = NULL;
    IShellItem *one = NULL;
    IShellItemArray *arr = NULL;
    IDataObject *dobj = NULL;

    if (SUCCEEDED(IUnknown_QueryInterface(punk, &IID_IPersistIDList, (void **)&pl))) {
        PIDLIST_ABSOLUTE id = NULL;
        if (SUCCEEDED(IPersistIDList_GetIDList(pl, &id)) &&
            SUCCEEDED(SHCreateItemFromIDList(id, &IID_IShellItem, (void **)&one))) {
            AddItem(op, move, one, dest, NULL);
            IShellItem_Release(one);
        } else {
            MarkUnsupported(op, L"IPersistIDList without item");
        }
        CoTaskMemFree(id);
        IPersistIDList_Release(pl);
        return;
    }
    if (SUCCEEDED(IUnknown_QueryInterface(punk, &IID_IShellItem, (void **)&one))) {
        AddItem(op, move, one, dest, NULL);
        IShellItem_Release(one);
        return;
    }
    if (FAILED(IUnknown_QueryInterface(punk, &IID_IShellItemArray, (void **)&arr)) &&
        SUCCEEDED(IUnknown_QueryInterface(punk, &IID_IDataObject, (void **)&dobj))) {
        SHCreateShellItemArrayFromDataObject(dobj, &IID_IShellItemArray, (void **)&arr);
        IDataObject_Release(dobj);
    }
    if (!arr) {
        MarkUnsupported(op, L"unknown item container");
        return;
    }
    DWORD count = 0;
    IShellItemArray_GetCount(arr, &count);
    for (DWORD i = 0; i < count; i++) {
        if (SUCCEEDED(IShellItemArray_GetItemAt(arr, i, &one))) {
            AddItem(op, move, one, dest, NULL);
            IShellItem_Release(one);
        }
    }
    IShellItemArray_Release(arr);
}

static BOOL Disabled(void)
{
    WCHAR p[MAX_PATH];
    GetTempPathW(MAX_PATH, p);
    wcscat_s(p, MAX_PATH, L"fastcopy_off");
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

typedef HRESULT (STDMETHODCALLTYPE *ItemsDest_t)(IFileOperation *, IUnknown *, IShellItem *);
typedef HRESULT (STDMETHODCALLTYPE *ItemDest_t)(IFileOperation *, IShellItem *, IShellItem *, LPCWSTR, IFileOperationProgressSink *);
typedef HRESULT (STDMETHODCALLTYPE *NoArgs_t)(IFileOperation *);
typedef HRESULT (STDMETHODCALLTYPE *SetFlags_t)(IFileOperation *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *OneItem_t)(IFileOperation *, IShellItem *);
typedef HRESULT (STDMETHODCALLTYPE *OneUnk_t)(IFileOperation *, IUnknown *);
typedef HRESULT (STDMETHODCALLTYPE *RenameItem_t)(IFileOperation *, IShellItem *, LPCWSTR, IFileOperationProgressSink *);
typedef HRESULT (STDMETHODCALLTYPE *RenameItems_t)(IFileOperation *, IUnknown *, LPCWSTR);
typedef HRESULT (STDMETHODCALLTYPE *DeleteItem_t)(IFileOperation *, IShellItem *, IFileOperationProgressSink *);
typedef HRESULT (STDMETHODCALLTYPE *NewItem_t)(IFileOperation *, IShellItem *, DWORD, LPCWSTR, LPCWSTR, IFileOperationProgressSink *);

static ItemsDest_t oCopyItems, oMoveItems;
static ItemDest_t oCopyItem, oMoveItem;
static NoArgs_t oPerform;
static SetFlags_t oSetFlags;
static OneItem_t oApplyPropsItem;
static OneUnk_t oApplyPropsItems, oDeleteItems;
static RenameItem_t oRenameItem;
static RenameItems_t oRenameItems;
static DeleteItem_t oDeleteItem;
static NewItem_t oNewItem;

static HRESULT STDMETHODCALLTYPE hkSetFlags(IFileOperation *op, DWORD flags)
{
    // Explorer calls this first on every new operation. shortcut: also used to drop stale entries
    // of a released op whose address got reused; hook Release if another caller skips this.
    AcquireSRWLockExclusive(&gOpsLock);
    OpEntry *e = FindEntry(op, FALSE);
    if (e && !e->n && !e->unsupported)
        e = NULL;
    if (e)
        FreeEntry(e);
    ReleaseSRWLockExclusive(&gOpsLock);
    return oSetFlags(op, flags);
}

static HRESULT STDMETHODCALLTYPE hkCopyItems(IFileOperation *op, IUnknown *items, IShellItem *dest)
{
    AddItems(op, FALSE, items, dest);
    return oCopyItems(op, items, dest);  // still queue it natively so fallback stays possible
}

static HRESULT STDMETHODCALLTYPE hkMoveItems(IFileOperation *op, IUnknown *items, IShellItem *dest)
{
    AddItems(op, TRUE, items, dest);
    return oMoveItems(op, items, dest);
}

static HRESULT STDMETHODCALLTYPE hkCopyItem(IFileOperation *op, IShellItem *item, IShellItem *dest, LPCWSTR newName, IFileOperationProgressSink *sink)
{
    if (sink)
        MarkUnsupported(op, L"per-item progress sink");
    else
        AddItem(op, FALSE, item, dest, newName);
    return oCopyItem(op, item, dest, newName, sink);
}

static HRESULT STDMETHODCALLTYPE hkMoveItem(IFileOperation *op, IShellItem *item, IShellItem *dest, LPCWSTR newName, IFileOperationProgressSink *sink)
{
    if (sink)
        MarkUnsupported(op, L"per-item progress sink");
    else
        AddItem(op, TRUE, item, dest, newName);
    return oMoveItem(op, item, dest, newName, sink);
}

// Any other queued action means we can't skip the native PerformOperations.
static HRESULT STDMETHODCALLTYPE hkApplyPropsItem(IFileOperation *op, IShellItem *i)
{
    MarkUnsupported(op, L"ApplyPropertiesToItem");
    return oApplyPropsItem(op, i);
}
static HRESULT STDMETHODCALLTYPE hkApplyPropsItems(IFileOperation *op, IUnknown *i)
{
    MarkUnsupported(op, L"ApplyPropertiesToItems");
    return oApplyPropsItems(op, i);
}
static HRESULT STDMETHODCALLTYPE hkRenameItem(IFileOperation *op, IShellItem *i, LPCWSTR n, IFileOperationProgressSink *s)
{
    MarkUnsupported(op, L"RenameItem");
    return oRenameItem(op, i, n, s);
}
static HRESULT STDMETHODCALLTYPE hkRenameItems(IFileOperation *op, IUnknown *i, LPCWSTR n)
{
    MarkUnsupported(op, L"RenameItems");
    return oRenameItems(op, i, n);
}
static HRESULT STDMETHODCALLTYPE hkDeleteItem(IFileOperation *op, IShellItem *i, IFileOperationProgressSink *s)
{
    MarkUnsupported(op, L"DeleteItem");
    return oDeleteItem(op, i, s);
}
static HRESULT STDMETHODCALLTYPE hkDeleteItems(IFileOperation *op, IUnknown *i)
{
    MarkUnsupported(op, L"DeleteItems");
    return oDeleteItems(op, i);
}
static HRESULT STDMETHODCALLTYPE hkNewItem(IFileOperation *op, IShellItem *d, DWORD a, LPCWSTR n, LPCWSTR t, IFileOperationProgressSink *s)
{
    MarkUnsupported(op, L"NewItem");
    return oNewItem(op, d, a, n, t, s);
}

static HRESULT STDMETHODCALLTYPE hkPerform(IFileOperation *op)
{
    OpEntry mine = { 0 };
    AcquireSRWLockExclusive(&gOpsLock);
    OpEntry *e = FindEntry(op, FALSE);
    if (e) {
        mine = *e;
        ZeroMemory(e, sizeof(*e));  // ownership of items moves to `mine`
    }
    ReleaseSRWLockExclusive(&gOpsLock);

    ULONGLONG t0 = GetTickCount64();
    FcProgress prog = { 0 };
    FcResult r = FC_FALLBACK;
    if (mine.n && !mine.unsupported) {
        if (Disabled())
            FcLog(L"op=%p kill switch on, native", op);
        else
            r = RunWithProgress(mine.items, mine.n, &prog);
    }
    int n = mine.n;
    FreeEntry(&mine);

    if (r == FC_FALLBACK) {
        HRESULT hr = oPerform(op);
        if (n)
            FcLog(L"op=%p NATIVE items=%d hr=0x%08lX %llu ms", op, n, hr, GetTickCount64() - t0);
        return hr;
    }
    static const PCWSTR names[] = { L"OK", L"FALLBACK", L"ERROR", L"CANCELLED" };
    ULONGLONG ms = GetTickCount64() - t0;
    LONG64 bytes = prog.bytesDone;
    FcLog(L"op=%p FASTCOPY %s items=%d files=%ld/%ld bytes=%lld %llu ms (%.1f MB/s)", op, names[r], n,
          prog.filesDone, prog.filesTotal, bytes, ms, ms ? bytes / 1048576.0 / (ms / 1000.0) : 0.0);
    return r == FC_OK ? S_OK : r == FC_CANCELLED ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : E_FAIL;
}

static BOOL Hook(void *target, void *detour, void **orig, PCWSTR name)
{
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s != MH_OK)
        FcLog(L"MH_CreateHook(%s) failed: %d", name, s);
    return s == MH_OK;
}

static DWORD WINAPI InitThread(LPVOID unused)
{
    (void)unused;
    FcLog(L"fc_hook loaded into pid=%lu", GetCurrentProcessId());
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOperation *op = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOperation, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOperation, (void **)&op);
    if (FAILED(hr) || MH_Initialize() != MH_OK) {
        FcLog(L"init failed hr=0x%08lX, staying inactive", hr);
        goto done;
    }
    // Every FileOperation instance shares this vtable, so hooking the bodies covers Explorer's.
    IFileOperationVtbl *vt = op->lpVtbl;
    BOOL ok = Hook(vt->CopyItems, hkCopyItems, (void **)&oCopyItems, L"CopyItems")
            && Hook(vt->MoveItems, hkMoveItems, (void **)&oMoveItems, L"MoveItems")
            && Hook(vt->CopyItem, hkCopyItem, (void **)&oCopyItem, L"CopyItem")
            && Hook(vt->MoveItem, hkMoveItem, (void **)&oMoveItem, L"MoveItem")
            && Hook(vt->SetOperationFlags, hkSetFlags, (void **)&oSetFlags, L"SetOperationFlags")
            && Hook(vt->ApplyPropertiesToItem, hkApplyPropsItem, (void **)&oApplyPropsItem, L"ApplyPropertiesToItem")
            && Hook(vt->ApplyPropertiesToItems, hkApplyPropsItems, (void **)&oApplyPropsItems, L"ApplyPropertiesToItems")
            && Hook(vt->RenameItem, hkRenameItem, (void **)&oRenameItem, L"RenameItem")
            && Hook(vt->RenameItems, hkRenameItems, (void **)&oRenameItems, L"RenameItems")
            && Hook(vt->DeleteItem, hkDeleteItem, (void **)&oDeleteItem, L"DeleteItem")
            && Hook(vt->DeleteItems, hkDeleteItems, (void **)&oDeleteItems, L"DeleteItems")
            && Hook(vt->NewItem, hkNewItem, (void **)&oNewItem, L"NewItem")
            && Hook(vt->PerformOperations, hkPerform, (void **)&oPerform, L"PerformOperations");
    // All or nothing: a partial set could skip PerformOperations with unrecorded work queued.
    if (ok && MH_EnableHook(MH_ALL_HOOKS) == MH_OK)
        FcLog(L"hooks active");
    else
        FcLog(L"hooks NOT enabled, Explorer stays native");

done:
    if (op)
        IFileOperation_Release(op);
    if (SUCCEEDED(hrCo))
        CoUninitialize();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        gInst = inst;
        DisableThreadLibraryCalls(inst);
        // COM and MinHook must not run under the loader lock.
        HANDLE th = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (th)
            CloseHandle(th);
    }
    return TRUE;
}
