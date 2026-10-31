// Progress window shown while the engine works: Windows 11 look (light/dark, accent colour,
// per-monitor DPI), taskbar progress, Pause/Resume and Cancel (Esc or closing also cancels).
#define _WIN32_WINNT 0x0A00
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <stdio.h>
#include "ui.h"

#define SHOW_DELAY_MS 500  // operations shorter than this never show a window
#define TIMER_MS 250
#define ID_PAUSE 100
#define WIDTH 480
#define HEIGHT 168

typedef struct {
    FcProgress *prog;
    BOOL move;
    WCHAR destName[MAX_PATH];
    HWND hwnd, pause, cancel;
    HFONT font, fontTitle;
    UINT dpi;
    BOOL dark;
    COLORREF accent;
    ULONGLONG lastTick;
    LONG64 lastBytes;
    double speed;  // bytes/s, smoothed
    ITaskbarList3 *taskbar;
} Ui;

typedef struct {
    COLORREF bg, text, sub, track, btn, btnPressed, btnBorder;
} Palette;

static const Palette kLight = { RGB(243, 243, 243), RGB(26, 26, 26), RGB(96, 96, 96), RGB(220, 220, 220),
                                RGB(251, 251, 251), RGB(235, 235, 235), RGB(210, 210, 210) };
static const Palette kDark = { RGB(32, 32, 32), RGB(255, 255, 255), RGB(200, 200, 200), RGB(70, 70, 70),
                               RGB(55, 55, 55), RGB(45, 45, 45), RGB(75, 75, 75) };

static int S(Ui *ui, int px) { return MulDiv(px, ui->dpi, 96); }

static DWORD RegDword(PCWSTR key, PCWSTR value, DWORD def)
{
    DWORD v, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, NULL, &v, &size) == ERROR_SUCCESS ? v : def;
}

static void LoadFonts(Ui *ui)
{
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, ui->dpi);
    if (ui->font) DeleteObject(ui->font);
    if (ui->fontTitle) DeleteObject(ui->fontTitle);
    ui->font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW title = ncm.lfMessageFont;
    title.lfHeight = title.lfHeight * 4 / 3;
    title.lfWeight = FW_SEMIBOLD;
    ui->fontTitle = CreateFontIndirectW(&title);
    SendMessageW(ui->pause, WM_SETFONT, (WPARAM)ui->font, TRUE);
    SendMessageW(ui->cancel, WM_SETFONT, (WPARAM)ui->font, TRUE);
}

static void Layout(Ui *ui)
{
    int bw = S(ui, 96), bh = S(ui, 32), m = S(ui, 20);
    RECT rc;
    GetClientRect(ui->hwnd, &rc);
    SetWindowPos(ui->cancel, NULL, rc.right - m - bw, rc.bottom - m - bh, bw, bh, SWP_NOZORDER);
    SetWindowPos(ui->pause, NULL, rc.right - m - 2 * bw - S(ui, 8), rc.bottom - m - bh, bw, bh, SWP_NOZORDER);
}

static void FormatEta(double seconds, PWSTR out, int cap)
{
    if (seconds < 60)
        swprintf_s(out, cap, L"quedan %d s", (int)seconds + 1);
    else if (seconds < 3600)
        swprintf_s(out, cap, L"quedan %d min", (int)(seconds / 60) + 1);
    else
        swprintf_s(out, cap, L"quedan %d h %d min", (int)(seconds / 3600), (int)(seconds / 60) % 60);
}

static void Paint(Ui *ui, HDC target)
{
    const Palette *pal = ui->dark ? &kDark : &kLight;
    FcProgress *pr = ui->prog;
    RECT rc;
    GetClientRect(ui->hwnd, &rc);
    // Double buffered to avoid flicker on every timer tick.
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(pal->bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    int m = S(ui, 20);
    WCHAR line[512], done[32], total[32];
    LONG files = pr->filesTotal;
    PCWSTR verb = ui->move ? L"Moviendo" : L"Copiando";
    if (pr->phase == FC_PHASE_PLAN)
        swprintf_s(line, 512, L"Preparando: %s a %s", ui->move ? L"mover" : L"copiar", ui->destName);
    else
        swprintf_s(line, 512, L"%s %ld %s a %s", verb, files, files == 1 ? L"archivo" : L"archivos", ui->destName);
    RECT r = { m, m, rc.right - m, m + S(ui, 26) };
    SelectObject(dc, ui->fontTitle);
    SetTextColor(dc, pal->text);
    DrawTextW(dc, line, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    AcquireSRWLockShared(&pr->lock);
    wcscpy_s(line, 512, PathFindFileNameW(pr->current));
    ReleaseSRWLockShared(&pr->lock);
    SelectObject(dc, ui->font);
    SetTextColor(dc, pal->sub);
    r.top = r.bottom + S(ui, 4);
    r.bottom = r.top + S(ui, 20);
    DrawTextW(dc, line, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // Thin rounded bar, like Windows 11's progress controls.
    LONG64 tot = pr->bytesTotal, dn = pr->bytesDone;
    double frac = tot > 0 ? (double)dn / tot : 0;
    if (frac > 1) frac = 1;
    int barY = r.bottom + S(ui, 10), barH = S(ui, 4), barW = rc.right - 2 * m;
    HBRUSH track = CreateSolidBrush(pal->track), fill = CreateSolidBrush(ui->prog->paused ? RGB(157, 93, 0) : ui->accent);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, track);
    RoundRect(dc, m, barY, m + barW, barY + barH, barH, barH);
    if (frac > 0) {
        SelectObject(dc, fill);
        RoundRect(dc, m, barY, m + max((int)(barW * frac), barH), barY + barH, barH, barH);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(track);
    DeleteObject(fill);

    if (pr->cancel) {
        wcscpy_s(line, 512, L"Cancelando...");
    } else if (pr->phase == FC_PHASE_PLAN) {
        wcscpy_s(line, 512, L"Calculando...");
    } else {
        StrFormatByteSizeW(dn, done, 32);
        StrFormatByteSizeW(tot, total, 32);
        int n = swprintf_s(line, 512, L"%d %% · %s de %s", (int)(frac * 100), done, total);
        if (pr->paused) {
            wcscat_s(line, 512, L" · En pausa");
        } else if (ui->speed > 1) {
            WCHAR speed[32], eta[48];
            StrFormatByteSizeW((LONGLONG)ui->speed, speed, 32);
            FormatEta((tot - dn) / ui->speed, eta, 48);
            swprintf_s(line + n, 512 - n, L" · %s/s · %s", speed, eta);
        }
    }
    SetTextColor(dc, pal->sub);
    r.top = barY + barH + S(ui, 10);
    r.bottom = r.top + S(ui, 20);
    DrawTextW(dc, line, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static void DrawButton(Ui *ui, const DRAWITEMSTRUCT *di)
{
    const Palette *pal = ui->dark ? &kDark : &kLight;
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    HBRUSH bg = CreateSolidBrush(pal->bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    BOOL disabled = di->itemState & ODS_DISABLED;
    HBRUSH face = CreateSolidBrush(di->itemState & ODS_SELECTED ? pal->btnPressed : pal->btn);
    HPEN border = CreatePen(PS_SOLID, di->itemState & ODS_FOCUS ? S(ui, 2) : 1,
                            di->itemState & ODS_FOCUS ? ui->accent : pal->btnBorder);
    HGDIOBJ ob = SelectObject(dc, face), op = SelectObject(dc, border);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, S(ui, 8), S(ui, 8));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(face);
    DeleteObject(border);

    WCHAR text[32];
    GetWindowTextW(di->hwndItem, text, 32);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? pal->sub : pal->text);
    SelectObject(dc, ui->font);
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void Tick(Ui *ui)
{
    ULONGLONG now = GetTickCount64();
    LONG64 bytes = ui->prog->bytesDone;
    if (ui->lastTick && now > ui->lastTick && !ui->prog->paused) {
        double inst = (bytes - ui->lastBytes) * 1000.0 / (now - ui->lastTick);
        ui->speed = ui->speed ? ui->speed * 0.7 + inst * 0.3 : inst;
    }
    ui->lastTick = now;
    ui->lastBytes = bytes;

    if (ui->taskbar) {
        if (ui->prog->phase == FC_PHASE_PLAN) {
            ITaskbarList3_SetProgressState(ui->taskbar, ui->hwnd, TBPF_INDETERMINATE);
        } else {
            ITaskbarList3_SetProgressState(ui->taskbar, ui->hwnd, ui->prog->paused ? TBPF_PAUSED : TBPF_NORMAL);
            ITaskbarList3_SetProgressValue(ui->taskbar, ui->hwnd, bytes, max(ui->prog->bytesTotal, 1));
        }
    }
    InvalidateRect(ui->hwnd, NULL, FALSE);
}

static void Cancel(Ui *ui)
{
    ui->prog->cancel = 1;
    ui->prog->paused = 0;
    EnableWindow(ui->pause, FALSE);
    EnableWindow(ui->cancel, FALSE);
    InvalidateRect(ui->hwnd, NULL, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Ui *ui = (Ui *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(ui, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM:
        DrawButton(ui, (DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_TIMER:
        Tick(ui);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) {
            Cancel(ui);
        } else if (LOWORD(wp) == ID_PAUSE) {
            ui->prog->paused = !ui->prog->paused;
            SetWindowTextW(ui->pause, ui->prog->paused ? L"Reanudar" : L"Pausar");
            InvalidateRect(ui->pause, NULL, FALSE);
            Tick(ui);
        }
        return 0;
    case WM_CLOSE:
        Cancel(ui);  // the window goes away when the engine stops
        return 0;
    case WM_DPICHANGED: {
        RECT *s = (RECT *)lp;
        ui->dpi = HIWORD(wp);
        LoadFonts(ui);
        SetWindowPos(hwnd, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(ui);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateProgressWindow(Ui *ui)
{
    static ATOM cls;
    if (!cls) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = gInst;
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        wc.lpszClassName = L"FastCopyProgress";
        WCHAR exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);  // reuse Explorer's own icon
        wc.hIcon = ExtractIconW(gInst, exe, 0);
        cls = RegisterClassExW(&wc);
    }
    ui->dark = RegDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", 1) == 0;
    // DWM stores the accent as 0xAABBGGRR, which is a COLORREF once alpha is dropped.
    ui->accent = RegDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFC06700) & 0xFFFFFF;

    // Centre over the Explorer window the user is working in.
    RECT anchor;
    HWND fg = GetForegroundWindow();
    if (!fg || !GetWindowRect(fg, &anchor))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
    UINT dpi = fg ? GetDpiForWindow(fg) : GetDpiForSystem();
    ui->dpi = dpi ? dpi : 96;
    RECT wr = { 0, 0, S(ui, WIDTH), S(ui, HEIGHT) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&wr, style, FALSE, 0, ui->dpi);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    int x = (anchor.left + anchor.right - w) / 2, y = (anchor.top + anchor.bottom - h) / 2;

    ui->hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"FastCopyProgress", ui->move ? L"Moviendo" : L"Copiando",
                               style, x, y, w, h, NULL, NULL, gInst, ui);
    if (!ui->hwnd)
        return NULL;
    BOOL dark = ui->dark;
    DwmSetWindowAttribute(ui->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    ui->pause = CreateWindowExW(0, L"BUTTON", L"Pausar", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                0, 0, 0, 0, ui->hwnd, (HMENU)ID_PAUSE, gInst, NULL);
    ui->cancel = CreateWindowExW(0, L"BUTTON", L"Cancelar", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 0, 0, 0, 0, ui->hwnd, (HMENU)IDCANCEL, gInst, NULL);
    LoadFonts(ui);
    Layout(ui);
    CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&ui->taskbar);
    if (ui->taskbar && FAILED(ITaskbarList3_HrInit(ui->taskbar))) {
        ITaskbarList3_Release(ui->taskbar);
        ui->taskbar = NULL;
    }
    ShowWindow(ui->hwnd, SW_SHOWNORMAL);
    SetTimer(ui->hwnd, 1, TIMER_MS, NULL);
    Tick(ui);
    return ui->hwnd;
}

typedef struct {
    const FcItem *items;
    int n;
    FcProgress *prog;
    FcResult res;
} Work;

static DWORD WINAPI Worker(LPVOID arg)
{
    Work *w = arg;
    w->res = FcRun(w->items, w->n, w->prog);
    return 0;
}

FcResult RunWithProgress(const FcItem *items, int n, FcProgress *prog)
{
    Work work = { items, n, prog, FC_ERROR };
    HANDLE th = CreateThread(NULL, 0, Worker, &work, 0, NULL);
    if (!th)
        return FcRun(items, n, prog);

    if (WaitForSingleObject(th, SHOW_DELAY_MS) == WAIT_TIMEOUT) {
        Ui ui = { prog, items[0].move };
        wcscpy_s(ui.destName, MAX_PATH, PathFindFileNameW(items[0].dstDir));
        HWND hwnd = CreateProgressWindow(&ui);
        BOOL quit = FALSE;
        WPARAM quitCode = 0;
        while (MsgWaitForMultipleObjects(1, &th, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) {
                    quit = TRUE;
                    quitCode = m.wParam;
                } else if (!hwnd || !IsDialogMessageW(hwnd, &m)) {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
            }
        }
        if (ui.taskbar) {
            ITaskbarList3_SetProgressState(ui.taskbar, hwnd, TBPF_NOPROGRESS);
            ITaskbarList3_Release(ui.taskbar);
        }
        if (hwnd)
            DestroyWindow(hwnd);
        DeleteObject(ui.font);
        DeleteObject(ui.fontTitle);
        if (quit)
            PostQuitMessage((int)quitCode);  // not ours to swallow
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    return work.res;
}
