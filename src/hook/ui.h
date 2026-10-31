#pragma once
#include "../engine/engine.h"

extern HINSTANCE gInst;

// Runs FcRun on a worker thread; if it takes longer than a moment, shows a progress window
// (pause/cancel) on the calling thread until it finishes.
FcResult RunWithProgress(const FcItem *items, int n, FcProgress *prog);
