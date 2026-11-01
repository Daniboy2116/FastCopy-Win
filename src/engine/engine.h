#pragma once
#include <windows.h>

// One queued Explorer operation: copy or move `src` into folder `dstDir`, optionally renamed.
typedef struct {
    BOOL move;
    PWSTR src;
    PWSTR dstDir;
    PWSTR newName;  // NULL keeps the source name
} FcItem;

enum { FC_PHASE_PLAN, FC_PHASE_COPY, FC_PHASE_FINISH };

// What to do with files that already exist at the destination (asked once per operation).
// FC_CONFLICT_EACH: the answer is per file, in prog->conflictPick (REPLACE, SKIP or BOTH each).
typedef enum { FC_CONFLICT_REPLACE, FC_CONFLICT_SKIP, FC_CONFLICT_BOTH, FC_CONFLICT_CANCEL, FC_CONFLICT_EACH } FcConflictChoice;
typedef struct FcProgress FcProgress;
typedef FcConflictChoice (*FcConflictFn)(FcProgress *prog, int count, PCWSTR example);

// Shared between the engine (writer) and the progress window (reader). Zero-initialize.
struct FcProgress {
    volatile LONG64 bytesDone;
    volatile LONG64 bytesTotal;  // known after planning; moves count their verify pass too
    volatile LONG filesDone, filesTotal;
    volatile LONG phase;
    volatile LONG cancel, paused;  // set by the UI
    FcConflictFn onConflict;       // NULL: any existing target falls back to Windows
    SRWLOCK lock;                  // guards `current`
    WCHAR current[MAX_PATH];
    // Conflict hand-off between the engine thread (asks, then polls conflictChoice) and the UI (answers).
    HANDLE conflictEvent;  // signalled by the engine when it starts asking; optional
    volatile LONG conflictPending, conflictChoice, conflictCount;  // choice: -1 until answered, else FcConflictChoice
    WCHAR conflictExample[MAX_PATH];
    // While a question is open: the conflicting files (source, existing target) and, for an EACH answer,
    // where to write one FcConflictChoice per file. Owned by the engine; valid until the answer is read.
    PCWSTR *conflictSrc, *conflictDst;
    volatile LONG *conflictPick;
};

typedef enum {
    FC_OK,         // everything copied/moved
    FC_FALLBACK,   // nothing was written; let Windows do it (conflicts, virtual paths, reparse points...)
    FC_ERROR,      // failed after writing; sources of moves are never deleted in that case
    FC_CANCELLED   // user cancelled: finished files stay, moved files are only removed from the source once verified
} FcResult;

FcResult FcRun(const FcItem *items, int n, FcProgress *prog);

void FcLog(const wchar_t *fmt, ...);
