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

// Shared between the engine (writer) and the progress window (reader). Zero-initialize.
typedef struct {
    volatile LONG64 bytesDone;
    volatile LONG64 bytesTotal;  // known after planning; moves count their verify pass too
    volatile LONG filesDone, filesTotal;
    volatile LONG phase;
    volatile LONG cancel, paused;  // set by the UI
    SRWLOCK lock;                  // guards `current`
    WCHAR current[MAX_PATH];
} FcProgress;

typedef enum {
    FC_OK,         // everything copied/moved
    FC_FALLBACK,   // nothing was written; let Windows do it (conflicts, virtual paths, reparse points...)
    FC_ERROR,      // failed after writing; sources of moves are never deleted in that case
    FC_CANCELLED   // user cancelled: finished files stay, moved files are only removed from the source once verified
} FcResult;

FcResult FcRun(const FcItem *items, int n, FcProgress *prog);

void FcLog(const wchar_t *fmt, ...);
