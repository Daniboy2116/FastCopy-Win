// Progress window shown while the engine works. Like Windows' own dialog, every running
// operation appears in ONE window: a dedicated UI thread owns it and Explorer's operation
// threads only add/remove themselves (WM_FC_ADD / WM_FC_REMOVE to a message-only window), so
// all window state lives on that thread. Windows 11 look (light/dark, accent, per-monitor DPI),
// taskbar progress, per-operation Pause/Cancel icon buttons. "Más detalles" morphs each
// progress bar into a live speed graph (the bar is the graph flattened) and reveals the
// item/data/time/route breakdown; the choice is remembered in HKCU\Software\FastCopy.
#define _WIN32_WINNT 0x0A00
#define UNICODE
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <directmanipulation.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <math.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui.h"

#define SHOW_DELAY_MS 500  // operations shorter than this never show up
#define TIMER_TICK 1
#define TIMER_FRAME 2
#define TICK_MS 250
#define FRAME_MS 16
#define EXPAND_MS 300
#define COLLAPSE_MS 240  // exits a little faster than entrances
#define WM_FC_ADD (WM_APP + 1)
#define WM_FC_REMOVE (WM_APP + 2)
#define ID_DETAILS 101
#define ID_OPBUTTON 200
#define ID_CONFLICT 300   // four buttons per operation: 300 replace, 301 skip, 302 keep both, 303 choose file by file
#define CONFLICT_BTNS 4
#define MAX_OPS 32
// Layout in 96-dpi pixels; everything between the two states is interpolated by the morph value.
#define WIDTH 500
#define MARGIN 20
#define HEADER_H 32       // "N operaciones en curso", only with more than one
#define METER_OFF 60      // panel top -> meter top
#define BAR_H 4
#define GRAPH_H 120
#define ROWS_H 104
#define PANEL_BASE_H 108  // collapsed panel incl. the gap below it
#define CONFLICT_H 132    // a panel asking what to do with files that already exist
#define FOOTER_H 52
#define ICON_BTN 32
#define GRAPH_POINTS 200  // speed samples across 0..100 % of the operation
#define SS 3              // meter supersampling: GDI draws aliased, downscaling smooths the edges
#define SETTINGS_KEY L"Software\\FastCopy"

typedef struct {
    FcProgress *prog;
    BOOL move;
    WCHAR srcName[MAX_PATH], destName[MAX_PATH], srcDir[MAX_PATH], dstDir[MAX_PATH];
    HWND pause, cancel, cbtn[CONFLICT_BTNS];
    double cf;         // 0..1: how far the panel has turned into the conflict question
    BOOL added;        // the UI thread took it over
    BOOL cseen;        // its current question was already scrolled into view
    HBITMAP meterBmp;  // cached pixels of the meter and the key they were drawn for
    unsigned char meterKey[96];  // a MeterKey
    double appear;     // 0..1 while its panel unfolds into the window
    double shownFrac;  // progress drawn, eased towards the real value between engine ticks
    ULONGLONG lastTick, activeMs;
    LONG64 lastBytes;
    double speed, peak;  // bytes/s; speed is smoothed
    float graph[GRAPH_POINTS];
    int graphN;
} Op;

typedef struct {
    HWND hwnd, details, tip, hover;
    HFONT font, fontTitle, fontKpi, fontChevron, fontGlyph;
    UINT dpi;
    BOOL dark, expanded, motion;
    COLORREF accent;
    // Morph: t = 0 bars, 1 graphs. Animated from its current value, so a toggle mid-way reverses smoothly.
    double t, tFrom, tTo;
    ULONGLONG animStart, animMs;
    int h;      // client height, eased towards the layout's height
    BOOL drag;  // the scroll thumb is being dragged
    int grab;   // pointer offset inside the thumb while dragging
    int content, scroll;  // total panel height and how far the list is scrolled (px); the window caps at 85 % of the screen
    Op *ops[MAX_OPS];
    int n;
    ITaskbarList3 *taskbar;
    // Precision touchpads: inside Explorer, Windows sends no wheel messages for them; like Explorer's
    // own views, the window takes two-finger pans through DirectManipulation (dm*).
    IDirectManipulationManager *dm;
    IDirectManipulationUpdateManager *dmUpdate;
    IDirectManipulationViewport *dmView;
    DWORD dmCookie;
    BOOL dmActive;     // a pan or its inertia is running: DirectManipulation needs Update() every frame
    float dmY, dmRem;  // last content offset seen, and the sub-pixel rest not yet scrolled
} Dlg;

typedef struct {
    COLORREF bg, text, sub, track, btn, btnHover, btnPressed, btnBorder, card, cardBorder;
} Palette;

static const Palette kLight = { RGB(243, 243, 243), RGB(26, 26, 26), RGB(96, 96, 96), RGB(220, 220, 220),
                                RGB(251, 251, 251), RGB(232, 232, 232), RGB(224, 224, 224), RGB(210, 210, 210),
                                RGB(251, 251, 251), RGB(229, 229, 229) };
static const Palette kDark = { RGB(32, 32, 32), RGB(255, 255, 255), RGB(200, 200, 200), RGB(70, 70, 70),
                               RGB(55, 55, 55), RGB(52, 52, 52), RGB(44, 44, 44), RGB(75, 75, 75),
                               RGB(43, 43, 43), RGB(29, 29, 29) };
static const COLORREF kPausedColor = RGB(157, 93, 0);
static const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

static Dlg gDlg;   // UI thread only
static HWND gMgr;  // message-only window of the UI thread
static INIT_ONCE gUiOnce = INIT_ONCE_STATIC_INIT;

static int S(Dlg *d, int px) { return MulDiv(px, d->dpi, 96); }
static int Lerp(int a, int b, double t) { return a + (int)((b - a) * t + (b > a ? 0.5 : -0.5)); }
static double Clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static const Palette *Pal(Dlg *d) { return d->dark ? &kDark : &kLight; }

static COLORREF Mix(COLORREF a, COLORREF b, double t)  // t = weight of a
{
    return RGB((int)(GetRValue(a) * t + GetRValue(b) * (1 - t)), (int)(GetGValue(a) * t + GetGValue(b) * (1 - t)),
               (int)(GetBValue(a) * t + GetBValue(b) * (1 - t)));
}

// cubic-bezier(0.23, 1, 0.32, 1), the shared ease-out curve: solve x(u) = p by Newton, return y(u).
static double EaseOut(double p)
{
    const double x1 = 0.23, x2 = 0.32;
    double u = p;
    for (int i = 0; i < 8; i++) {
        double x = 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u - p;
        double dx = 3 * (1 - u) * (1 - u) * x1 + 6 * (1 - u) * u * (x2 - x1) + 3 * u * u * (1 - x2);
        if (fabs(dx) < 1e-6)
            break;
        u = Clamp01(u - x / dx);
    }
    return 1 - (1 - u) * (1 - u) * (1 - u);  // y(u) with y1 = y2 = 1
}

static DWORD RegDword(PCWSTR key, PCWSTR value, DWORD def)
{
    DWORD v, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, NULL, &v, &size) == ERROR_SUCCESS ? v : def;
}

static void FormatInt(LONG64 v, PWSTR out, int cap)
{
    WCHAR raw[32], dec[8], thou[8];
    swprintf_s(raw, 32, L"%lld", v);
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, dec, 8);
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, thou, 8);
    NUMBERFMTW nf = { 0, 0, 3, dec, thou, 1 };
    if (!GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, raw, &nf, out, cap))
        wcscpy_s(out, cap, raw);
}

static void FormatClock(double seconds, PWSTR out, int cap)
{
    int s = (int)(seconds + 0.5);
    if (s >= 3600)
        swprintf_s(out, cap, L"%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        swprintf_s(out, cap, L"%d:%02d", s / 60, s % 60);
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

static double RealFrac(Op *o)
{
    LONG64 tot = o->prog->bytesTotal;
    return tot > 0 ? Clamp01((double)o->prog->bytesDone / tot) : 0;
}

static int CALLBACK FontFound(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM found)
{
    (void)lf;
    (void)tm;
    (void)type;
    *(BOOL *)found = TRUE;
    return 0;
}

// Windows 11's icon font; Windows 10 and Server 2019/2022 have its predecessor, with the same code points.
static PCWSTR IconFont(void)
{
    LOGFONTW lf = { 0 };
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe Fluent Icons");
    BOOL found = FALSE;
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExW(dc, &lf, FontFound, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
}

static void LoadFonts(Dlg *d)
{
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, d->dpi);
    HFONT *fonts[] = { &d->font, &d->fontTitle, &d->fontKpi, &d->fontChevron, &d->fontGlyph };
    for (int i = 0; i < 5; i++)
        if (*fonts[i])
            DeleteObject(*fonts[i]);
    d->font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = ncm.lfMessageFont.lfHeight * 4 / 3;
    lf.lfWeight = FW_SEMIBOLD;
    d->fontTitle = CreateFontIndirectW(&lf);
    lf.lfHeight = ncm.lfMessageFont.lfHeight * 5 / 3;
    d->fontKpi = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, IconFont());
    lf.lfHeight = ncm.lfMessageFont.lfHeight * 5 / 6;
    d->fontChevron = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(d, 16);
    d->fontGlyph = CreateFontIndirectW(&lf);
    SendMessageW(d->details, WM_SETFONT, (WPARAM)d->font, TRUE);
}

static int HeaderH(Dlg *d) { return d->n > 1 ? S(d, HEADER_H) : 0; }

static int PanelH(Dlg *d, Op *o)
{
    int normal = (int)(S(d, PANEL_BASE_H) + (S(d, GRAPH_H - BAR_H) + S(d, 10 + ROWS_H)) * d->t);
    return (int)(Lerp(normal, S(d, CONFLICT_H), o->cf) * o->appear + 0.5);
}

// Height the window wants now; also decides how many panels fit (85 % of the work area).
static int LayoutHeight(Dlg *d)
{
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(d->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT frame = { 0, 0, 0, 0 };
    AdjustWindowRectExForDpi(&frame, kStyle, FALSE, WS_EX_CONTROLPARENT, d->dpi);
    int maxH = (mi.rcWork.bottom - mi.rcWork.top) * 85 / 100 - (frame.bottom - frame.top);
    int h = S(d, MARGIN) + HeaderH(d) + S(d, FOOTER_H);
    d->content = 0;
    for (int i = 0; i < d->n; i++)
        d->content += PanelH(d, d->ops[i]);
    return d->n ? min(h + d->content, max(maxH, h + PanelH(d, d->ops[0]))) : h;
}

static int PanelTop(Dlg *d, int index)
{
    int y = S(d, MARGIN) + HeaderH(d);
    for (int i = 0; i < index; i++)
        y += PanelH(d, d->ops[i]);
    return y - d->scroll;
}

static void ApplyHeight(Dlg *d)
{
    RECT wr;
    GetWindowRect(d->hwnd, &wr);
    RECT cr = { 0, 0, S(d, WIDTH), d->h };
    AdjustWindowRectExForDpi(&cr, kStyle, FALSE, WS_EX_CONTROLPARENT, d->dpi);
    int w = cr.right - cr.left, h = cr.bottom - cr.top, y = wr.top;
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(d->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    if (y + h > mi.rcWork.bottom)  // grow upwards rather than off the screen
        y = max(mi.rcWork.top, mi.rcWork.bottom - h);
    SetWindowPos(d->hwnd, NULL, wr.left, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Child buttons follow their panel; ones scrolled out of the window or still unfolding hide.
static void Layout(Dlg *d)
{
    RECT rc;
    GetClientRect(d->hwnd, &rc);
    int m = S(d, MARGIN), b = S(d, ICON_BTN), footerTop = rc.bottom - S(d, FOOTER_H);
    int viewTop = m + HeaderH(d), gap = S(d, 8), bh = S(d, 34);
    // Buttons move without painting; then the window and all of them paint in one go, so while
    // scrolling the buttons never show up a frame away from the panel they belong to.
    HDWP dw = BeginDeferWindowPos(2 + 6 * d->n);
    const UINT keep = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS;
    dw = DeferWindowPos(dw, d->details, NULL, m - S(d, 8), footerTop + S(d, 10), S(d, 150), S(d, 32), keep);
    for (int i = 0; i < d->n && dw; i++) {
        Op *o = d->ops[i];
        int top = PanelTop(d, i), y = top + S(d, 22), panelBottom = top + PanelH(d, o);
        BOOL visible = y >= viewTop && y + b <= panelBottom && y + b <= footerTop;
        BOOL ask = o->prog->conflictPending != 0;
        dw = DeferWindowPos(dw, o->cancel, NULL, rc.right - m - b + S(d, 6), y, b, b, keep | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        dw = DeferWindowPos(dw, o->pause, NULL, rc.right - m - 2 * b + S(d, 2), y, b, b, keep | (visible && !ask ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        int by = top + S(d, 84);
        BOOL bvisible = ask && o->cf > 0.6 && by >= viewTop && by + bh <= panelBottom && by + bh <= footerTop;
        // One file has nothing to choose file by file: three buttons then, four otherwise.
        int nb = o->prog->conflictCount > 1 ? CONFLICT_BTNS : CONFLICT_BTNS - 1, bw = (rc.right - 2 * m - (nb - 1) * gap) / nb;
        for (int k = 0; k < CONFLICT_BTNS && dw; k++)
            if (o->cbtn[k])
                dw = DeferWindowPos(dw, o->cbtn[k], NULL, m + k * (bw + gap), by, bw, bh, keep | (bvisible && k < nb ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
    if (dw)
        EndDeferWindowPos(dw);
    RedrawWindow(d->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void StartFrames(Dlg *d) { SetTimer(d->hwnd, TIMER_FRAME, FRAME_MS, NULL); }

static void ToggleDetails(Dlg *d)
{
    d->expanded = !d->expanded;
    DWORD v = d->expanded;
    RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"ShowDetails", REG_DWORD, &v, sizeof(v));
    SetWindowTextW(d->details, d->expanded ? L"Menos detalles" : L"Más detalles");
    d->tFrom = d->t;
    d->tTo = d->expanded ? 1 : 0;
    // A reversal mid-way only travels what's left, in the same proportion of time.
    d->animMs = d->motion ? (ULONGLONG)((d->expanded ? EXPAND_MS : COLLAPSE_MS) * fabs(d->tTo - d->tFrom)) : 0;
    d->animStart = GetTickCount64();
    StartFrames(d);
}

// One animation frame. FALSE once everything has settled.
static BOOL Frame(Dlg *d)
{
    if (d->dmActive)
        IDirectManipulationUpdateManager_Update(d->dmUpdate, NULL);  // moves d->scroll via DmContent
    BOOL busy = d->dmActive, morphing = d->t != d->tTo;
    if (morphing) {
        double p = d->animMs ? (double)(GetTickCount64() - d->animStart) / d->animMs : 1;
        d->t = p >= 1 ? d->tTo : d->tFrom + (d->tTo - d->tFrom) * EaseOut(p);
        busy = d->t != d->tTo;
    }
    for (int i = 0; i < d->n; i++) {
        Op *o = d->ops[i];
        if (o->appear < 1) {
            o->appear = d->motion && o->appear < 0.995 ? o->appear + (1 - o->appear) * 0.22 : 1;
            busy |= o->appear < 1;
        }
        double ct = o->prog->conflictPending ? 1 : 0;
        if (o->cf != ct) {
            o->cf = d->motion && fabs(ct - o->cf) > 0.005 ? o->cf + (ct - o->cf) * 0.25 : ct;
            busy |= o->cf != ct;
        }
        double real = RealFrac(o), diff = real - o->shownFrac;
        if (d->motion && fabs(diff) > 0.0005) {
            o->shownFrac += diff * 0.2;  // ~75 ms time constant: hides the 250 ms engine steps
            busy = TRUE;
        } else {
            o->shownFrac = real;
        }
    }
    // The morph sets the height exactly (it is already eased); adds/removes ease towards it.
    int target = LayoutHeight(d), before = d->h;
    if (!d->motion || morphing || abs(target - d->h) <= 1)
        d->h = target;
    else {
        d->h += (target - d->h) * 3 / 10;
        busy = TRUE;
    }
    if (d->h != before)
        ApplyHeight(d);
    d->scroll = max(0, min(d->scroll, d->content - (d->h - S(d, MARGIN) - HeaderH(d) - S(d, FOOTER_H))));
    Layout(d);  // also repaints
    return busy;
}

static RECT MeterRect(Dlg *d, int top, int width)
{
    RECT r = { S(d, MARGIN), top + S(d, METER_OFF), width - S(d, MARGIN), top + S(d, METER_OFF) + Lerp(S(d, BAR_H), S(d, GRAPH_H), d->t) };
    return r;
}

// The bar and the graph are one shape. At t = 0 every sample's area reaches the top, so the
// accent fill up to shownFrac is exactly the progress bar; as t grows the shape deepens into
// the speed curve, the track becomes the graph background and the fill turns into a gradient.
static void RenderMeter(Dlg *d, Op *o, HDC target, RECT r)
{
    const Palette *pal = Pal(d);
    double t = d->t;
    int w = (r.right - r.left) * SS, h = (r.bottom - r.top) * SS;
    if (w <= 0 || h <= 0)
        return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    RECT all = { 0, 0, w, h };
    HBRUSH b = CreateSolidBrush(pal->bg);
    FillRect(dc, &all, b);
    DeleteObject(b);

    int radius = Lerp(h, S(d, 8) * SS, t);  // pill bar -> rounded card corners
    HRGN shape = CreateRoundRectRgn(0, 0, w + 1, h + 1, radius, radius);
    SelectClipRgn(dc, shape);
    COLORREF meterBg = Mix(pal->track, pal->card, 1 - t);
    b = CreateSolidBrush(meterBg);
    FillRect(dc, &all, b);
    DeleteObject(b);

    if (t > 0) {
        HPEN grid = CreatePen(PS_SOLID, SS, Mix(Mix(pal->track, pal->card, 0.6), meterBg, t));
        HGDIOBJ op = SelectObject(dc, grid);
        for (int i = 1; i <= 3; i++) {
            MoveToEx(dc, 0, h * i / 4, NULL);
            LineTo(dc, w, h * i / 4);
        }
        SelectObject(dc, op);
        DeleteObject(grid);
    }

    COLORREF acc = o->prog->paused ? kPausedColor : d->accent;
    int fillW = (int)(o->shownFrac * w);
    // Headroom keeps the curve under the speed figure drawn over the top-left corner.
    double maxY = o->peak * 1.45 > 1 ? o->peak * 1.45 : 1;
    POINT pts[GRAPH_POINTS + 2];
    int n = o->graphN >= 2 ? o->graphN : 0;
    for (int k = 0; k < n; k++) {
        int curveY = h - SS - (int)(o->graph[k] / maxY * (h - 4 * SS));
        pts[k].x = MulDiv(k, w - 1, GRAPH_POINTS - 1);
        pts[k].y = Lerp(0, curveY, t);
    }
    if (n) {  // extend the last sample to the drawn progress so the area never lags the bar
        pts[n].x = w;
        pts[n].y = pts[n - 1].y;
        pts[n + 1].x = w;
        pts[n + 1].y = h;
    }

    if (fillW > 0) {
        HRGN area;
        if (n) {
            POINT poly[GRAPH_POINTS + 3];
            memcpy(poly, pts, (n + 2) * sizeof(POINT));
            poly[n + 2].x = 0;
            poly[n + 2].y = h;
            area = CreatePolygonRgn(poly, n + 3, WINDING);
        } else {
            area = CreateRectRgn(0, 0, w, h);
        }
        HRGN upTo = CreateRectRgn(0, 0, fillW, h);
        CombineRgn(area, area, upTo, RGN_AND);
        CombineRgn(area, area, shape, RGN_AND);
        SelectClipRgn(dc, area);
        COLORREF top = Mix(acc, Mix(acc, pal->card, 0.35), 1 - t), bottom = Mix(acc, pal->card, 1 - t);
        TRIVERTEX v[2] = {
            { 0, 0, GetRValue(top) << 8, GetGValue(top) << 8, GetBValue(top) << 8, 0 },
            { w, h, GetRValue(bottom) << 8, GetGValue(bottom) << 8, GetBValue(bottom) << 8, 0 },
        };
        GRADIENT_RECT gr = { 0, 1 };
        GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
        DeleteObject(upTo);
        DeleteObject(area);
    }

    // Line, average and live dot fade in over the second half of the morph.
    double k = Clamp01((t - 0.5) * 2);
    if (k > 0 && n) {
        HRGN upTo = CreateRectRgn(0, 0, fillW + 2 * SS, h);
        CombineRgn(upTo, upTo, shape, RGN_AND);
        SelectClipRgn(dc, shape);
        double avg = o->activeMs ? o->prog->bytesDone * 1000.0 / o->activeMs : 0;
        if (avg > 0) {
            LOGBRUSH lb = { BS_SOLID, Mix(pal->sub, meterBg, k), 0 };
            HPEN dash = ExtCreatePen(PS_GEOMETRIC | PS_DASH | PS_ENDCAP_FLAT, SS, &lb, 0, NULL);
            HGDIOBJ op = SelectObject(dc, dash);
            int y = h - SS - (int)(avg / maxY * (h - 4 * SS));
            MoveToEx(dc, 0, y, NULL);
            LineTo(dc, w, y);
            SelectObject(dc, op);
            DeleteObject(dash);
        }
        SelectClipRgn(dc, upTo);
        COLORREF lineColor = Mix(acc, Mix(acc, pal->card, 0.35), k);
        LOGBRUSH lb = { BS_SOLID, lineColor, 0 };
        HPEN line = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 2 * SS, &lb, 0, NULL);
        HGDIOBJ op = SelectObject(dc, line);
        Polyline(dc, pts, n + 1);
        SelectObject(dc, op);
        DeleteObject(line);

        SelectClipRgn(dc, shape);
        int idx = (int)(o->shownFrac * (GRAPH_POINTS - 1));
        int dy = pts[idx < n ? idx : n - 1].y, rr = 4 * SS;
        HBRUSH dot = CreateSolidBrush(lineColor);
        HGDIOBJ ob = SelectObject(dc, dot);
        op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, fillW - rr, dy - rr, fillW + rr, dy + rr);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(dot);
        DeleteObject(upTo);
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(shape);

    SetStretchBltMode(target, HALFTONE);
    SetBrushOrgEx(target, 0, 0, NULL);
    StretchBlt(target, r.left, r.top, r.right - r.left, r.bottom - r.top, dc, 0, 0, w, h, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);

}

// The meter only changes with the operation's state, not when the list scrolls, so its pixels are cached.
typedef struct {
    double t, fill, peak, last, avg;
    int n, w, h;
    BOOL paused, dark;
    COLORREF accent;
} MeterKey;

static void DrawMeter(Dlg *d, Op *o, HDC target, RECT r)
{
    const Palette *pal = Pal(d);
    double t = d->t;
    int w = r.right - r.left, h = r.bottom - r.top;
    MeterKey key;
    memset(&key, 0, sizeof(key));
    key.t = t;
    key.fill = o->shownFrac;
    key.peak = o->peak;
    key.n = o->graphN;
    key.last = o->graphN ? o->graph[o->graphN - 1] : 0;
    key.avg = o->activeMs ? o->prog->bytesDone * 1000.0 / o->activeMs : 0;
    key.w = w;
    key.h = h;
    key.paused = o->prog->paused != 0;
    key.dark = d->dark;
    key.accent = d->accent;
    HDC mem = CreateCompatibleDC(target);
    if (o->meterBmp && !memcmp(&key, o->meterKey, sizeof(key))) {
        HGDIOBJ old = SelectObject(mem, o->meterBmp);
        BitBlt(target, r.left, r.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
    } else {
        RenderMeter(d, o, target, r);
        if (o->meterBmp)
            DeleteObject(o->meterBmp);
        o->meterBmp = CreateCompatibleBitmap(target, w, h);
        HGDIOBJ old = SelectObject(mem, o->meterBmp);
        BitBlt(mem, 0, 0, w, h, target, r.left, r.top, SRCCOPY);
        SelectObject(mem, old);
        memcpy(o->meterKey, &key, sizeof(key));
    }
    DeleteDC(mem);
    COLORREF meterBg = Mix(pal->track, pal->card, 1 - t);

    // Current speed as the largest figure, average and peak beside it; they arrive last.
    double kk = Clamp01((t - 0.6) / 0.4);
    if (kk > 0) {
        FcProgress *pr = o->prog;
        WCHAR a[64], b2[64], line[256];
        RECT tr = { r.left + S(d, 12), r.top + S(d, 8), r.right - S(d, 12), r.top + S(d, 36) };
        SelectObject(target, d->fontKpi);
        SetTextColor(target, Mix(pal->text, meterBg, kk));
        if (pr->paused)
            wcscpy_s(line, 256, L"En pausa");
        else if (pr->phase == FC_PHASE_PLAN || o->speed <= 1)
            wcscpy_s(line, 256, L"—");
        else {
            StrFormatByteSizeW((LONGLONG)o->speed, a, 64);
            swprintf_s(line, 256, L"%s/s", a);
        }
        DrawTextW(target, line, -1, &tr, DT_SINGLELINE | DT_TOP | DT_NOPREFIX);
        double avg = o->activeMs ? pr->bytesDone * 1000.0 / o->activeMs : 0;
        if (avg > 1) {
            StrFormatByteSizeW((LONGLONG)avg, a, 64);
            StrFormatByteSizeW((LONGLONG)o->peak, b2, 64);
            swprintf_s(line, 256, L"media %s/s  ·  máxima %s/s", a, b2);
            SelectObject(target, d->font);
            SetTextColor(target, Mix(pal->sub, meterBg, kk));
            tr.top += S(d, 6);
            DrawTextW(target, line, -1, &tr, DT_SINGLELINE | DT_TOP | DT_RIGHT | DT_NOPREFIX);
        }
    }
}

static void DrawRow(Dlg *d, HDC dc, COLORREF bg, double a, int x, int y, int right, PCWSTR label, PCWSTR value, UINT ellipsis)
{
    const Palette *pal = Pal(d);
    if (a <= 0)
        return;
    y += (int)((1 - a) * S(d, 6));  // rises into place while it fades in
    RECT r = { x, y, x + S(d, 92), y + S(d, 22) };
    SetTextColor(dc, Mix(pal->sub, bg, a));
    DrawTextW(dc, label, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    r.left = r.right;
    r.right = right;
    SetTextColor(dc, Mix(pal->text, bg, a));
    DrawTextW(dc, value, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | ellipsis);
}

static void PaintRows(Dlg *d, Op *o, HDC dc, RECT card)
{
    const Palette *pal = Pal(d);
    double cardIn = Clamp01((d->t - 0.45) / 0.35);
    if (cardIn <= 0)
        return;
    COLORREF cardBg = Mix(pal->card, pal->bg, cardIn);
    HBRUSH fill = CreateSolidBrush(cardBg);
    HPEN border = CreatePen(PS_SOLID, 1, Mix(pal->cardBorder, pal->bg, cardIn));
    HGDIOBJ ob = SelectObject(dc, fill), op = SelectObject(dc, border);
    RoundRect(dc, card.left, card.top, card.right, card.bottom, S(d, 8), S(d, 8));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fill);
    DeleteObject(border);

    FcProgress *pr = o->prog;
    WCHAR a[64], b[64], c[64], line[512];
    int x = card.left + S(d, 12), right = card.right - S(d, 12), y = card.top + S(d, 8);
    LONG64 tot = pr->bytesTotal, dn = pr->bytesDone;
    SelectObject(dc, d->font);
    // Rows cascade in, 70 ms of morph apart.
#define ROW_IN(i) Clamp01((d->t - 0.55 - (i) * 0.07) / 0.3)

    if (pr->phase == FC_PHASE_PLAN) {
        wcscpy_s(line, 512, L"Calculando…");
    } else {
        FormatInt(pr->filesDone, a, 64);
        FormatInt(pr->filesTotal, b, 64);
        FormatInt(pr->filesTotal - pr->filesDone, c, 64);
        swprintf_s(line, 512, L"%s de %s  ·  quedan %s", a, b, c);
    }
    DrawRow(d, dc, cardBg, ROW_IN(0), x, y, right, o->move ? L"Movidos" : L"Copiados", line, DT_END_ELLIPSIS);

    y += S(d, 22);
    StrFormatByteSizeW(dn, a, 64);
    StrFormatByteSizeW(tot, b, 64);
    StrFormatByteSizeW(tot > dn ? tot - dn : 0, c, 64);
    swprintf_s(line, 512, L"%s de %s  ·  quedan %s", a, b, c);
    // Moves read every byte twice (copy, then verify), and the totals say so.
    DrawRow(d, dc, cardBg, ROW_IN(1), x, y, right, o->move ? L"Copia + verif." : L"Datos", line, DT_END_ELLIPSIS);

    y += S(d, 22);
    FormatClock(o->activeMs / 1000.0, a, 64);
    if (o->speed > 1 && !pr->paused) {
        FormatClock((tot - dn) / o->speed, b, 64);
        swprintf_s(line, 512, L"%s transcurrido  ·  quedan ~%s", a, b);
    } else {
        swprintf_s(line, 512, L"%s transcurrido", a);
    }
    DrawRow(d, dc, cardBg, ROW_IN(2), x, y, right, L"Tiempo", line, DT_END_ELLIPSIS);

    y += S(d, 22);
    swprintf_s(line, 512, L"%s  →  %s", o->srcDir, o->dstDir);
    DrawRow(d, dc, cardBg, ROW_IN(3), x, y, right, L"Ruta", line, DT_PATH_ELLIPSIS);
#undef ROW_IN
}

// Draws text pieces one after another on a line; the piece that overflows gets the ellipsis.
static void DrawPieces(HDC dc, int x, int y, int right, int h, PCWSTR *text, const COLORREF *color, int n)
{
    for (int i = 0; i < n && x < right; i++) {
        RECT r = { x, y, right, y + h };
        SIZE sz;
        GetTextExtentPoint32W(dc, text[i], (int)wcslen(text[i]), &sz);
        SetTextColor(dc, color[i]);
        DrawTextW(dc, text[i], -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        x += sz.cx;
    }
}

static void PaintPanel(Dlg *d, Op *o, HDC dc, int top, int width)
{
    const Palette *pal = Pal(d);
    FcProgress *pr = o->prog;
    int m = S(d, MARGIN), right = width - m;
    WCHAR head[64], line[512], done[32], total[32];

    // "Copiando 3001 archivos de <origen> a <destino>", folder names in the accent colour.
    LONG files = pr->filesTotal;
    if (pr->phase == FC_PHASE_PLAN)
        swprintf_s(head, 64, L"Preparando: %s de ", o->move ? L"mover" : L"copiar");
    else
        swprintf_s(head, 64, L"%s %ld %s de ", o->move ? L"Moviendo" : L"Copiando", files, files == 1 ? L"archivo" : L"archivos");
    PCWSTR pieces[] = { head, o->srcName, L" a ", o->destName };
    COLORREF colors[] = { pal->text, d->accent, pal->text, d->accent };
    SelectObject(dc, d->font);
    DrawPieces(dc, m, top, right, S(d, 20), pieces, colors, 4);

    if (pr->conflictPending) {
        // The question replaces the meter; its three buttons are child windows below this text.
        WCHAR ask[128];
        LONG count = pr->conflictCount;
        if (count == 1)
            swprintf_s(ask, 128, L"«%s» ya existe en el destino", PathFindFileNameW(pr->conflictExample));
        else
            swprintf_s(ask, 128, L"%ld archivos ya existen en el destino", count);
        RECT q = { m, top + S(d, 24), right - S(d, ICON_BTN) - S(d, 8), top + S(d, 52) };
        SelectObject(dc, d->fontTitle);
        SetTextColor(dc, pal->text);
        DrawTextW(dc, ask, -1, &q, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        swprintf_s(line, 512, count == 1 ? L"%s" : L"Por ejemplo: %s", pr->conflictExample);
        q.top = top + S(d, 54);
        q.bottom = q.top + S(d, 22);
        q.right = right;
        SelectObject(dc, d->font);
        SetTextColor(dc, pal->sub);
        DrawTextW(dc, line, -1, &q, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        return;
    }

    // "45 % completado" large, with the icon buttons (child windows) to its right.
    int pct = (int)(RealFrac(o) * 100);
    if (pr->cancel)
        wcscpy_s(line, 512, L"Cancelando…");
    else if (pr->paused)
        swprintf_s(line, 512, L"En pausa · %d %%", pct);
    else
        swprintf_s(line, 512, L"%d %% completado", pct);
    RECT r = { m, top + S(d, 24), right - 2 * S(d, ICON_BTN) - S(d, 8), top + S(d, 52) };
    SelectObject(dc, d->fontTitle);
    SetTextColor(dc, pal->text);
    DrawTextW(dc, line, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT meter = MeterRect(d, top, width);
    DrawMeter(d, o, dc, meter);

    LONG64 tot = pr->bytesTotal, dn = pr->bytesDone;
    if (pr->phase == FC_PHASE_PLAN) {
        wcscpy_s(line, 512, L"Calculando…");
    } else {
        AcquireSRWLockShared(&pr->lock);
        int n = swprintf_s(line, 512, L"%s", PathFindFileNameW(pr->current));
        ReleaseSRWLockShared(&pr->lock);
        StrFormatByteSizeW(dn, done, 32);
        StrFormatByteSizeW(tot, total, 32);
        n += swprintf_s(line + n, 512 - n, L"%s%s de %s", n ? L" · " : L"", done, total);
        if (o->speed > 1 && !pr->paused) {
            WCHAR speed[32], eta[48];
            StrFormatByteSizeW((LONGLONG)o->speed, speed, 32);
            FormatEta((tot - dn) / o->speed, eta, 48);
            swprintf_s(line + n, 512 - n, L" · %s/s · %s", speed, eta);
        }
    }
    SelectObject(dc, d->font);
    SetTextColor(dc, pal->sub);
    r.left = m;
    r.right = right;
    r.top = meter.bottom + S(d, 8);  // the stats line rides below the meter as it grows
    r.bottom = r.top + S(d, 20);
    DrawTextW(dc, line, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (d->t > 0) {
        RECT card = { m, r.bottom + S(d, 10), right, r.bottom + S(d, 10 + ROWS_H) };
        PaintRows(d, o, dc, card);
    }
}

// Scroll thumb geometry; FALSE when everything fits.
static BOOL ThumbGeom(Dlg *d, RECT *thumb, int *viewTop, int *viewH)
{
    RECT rc;
    GetClientRect(d->hwnd, &rc);
    *viewTop = S(d, MARGIN) + HeaderH(d);
    *viewH = rc.bottom - S(d, FOOTER_H) - *viewTop;
    if (*viewH <= 0 || d->content <= *viewH)
        return FALSE;
    int th = max(S(d, 24), *viewH * *viewH / d->content);
    int ty = *viewTop + (*viewH - th) * d->scroll / (d->content - *viewH);
    SetRect(thumb, rc.right - S(d, 8), ty, rc.right - S(d, 3), ty + th);
    return TRUE;
}

static void Paint(Dlg *d, HDC target)
{
    const Palette *pal = Pal(d);
    RECT rc;
    GetClientRect(d->hwnd, &rc);
    // Double buffered to avoid flicker on every frame.
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(pal->bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    int m = S(d, MARGIN), footerTop = rc.bottom - S(d, FOOTER_H);
    WCHAR line[128];

    if (d->n > 1) {
        swprintf_s(line, 128, L"%d operaciones en curso", d->n);
        RECT r = { m, m - S(d, 4), rc.right - m, m - S(d, 4) + S(d, 26) };
        SelectObject(dc, d->fontTitle);
        SetTextColor(dc, pal->text);
        DrawTextW(dc, line, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    HPEN sep = CreatePen(PS_SOLID, 1, Mix(pal->track, pal->bg, 0.6));
    int viewTop = m + HeaderH(d);
    for (int i = 0; i < d->n; i++) {
        Op *o = d->ops[i];
        int top = PanelTop(d, i), ph = PanelH(d, o);
        if (top + ph <= viewTop || top >= footerTop)
            continue;
        int saved = SaveDC(dc);
        // A panel only ever draws inside its own (unfolding) slot, between the header and the footer.
        IntersectClipRect(dc, 0, max(top, viewTop), rc.right, min(top + ph, footerTop));
        PaintPanel(d, o, dc, top, rc.right);
        RestoreDC(dc, saved);
        if (i > 0 && top - S(d, 9) >= viewTop) {
            HGDIOBJ op = SelectObject(dc, sep);
            MoveToEx(dc, m, top - S(d, 9), NULL);
            LineTo(dc, rc.right - m, top - S(d, 9));
            SelectObject(dc, op);
        }
    }
    DeleteObject(sep);

    RECT tr;
    int tvTop, tvH;
    if (ThumbGeom(d, &tr, &tvTop, &tvH)) {  // thin scroll thumb on the right edge
        HBRUSH thumb = CreateSolidBrush(d->drag ? pal->sub : pal->btnBorder);
        HGDIOBJ ob = SelectObject(dc, thumb), op = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, tr.left, tr.top, tr.right, tr.bottom, S(d, 5), S(d, 5));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(thumb);
    }

    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static Op *OpOfButton(Dlg *d, HWND btn, BOOL *isPause)
{
    for (int i = 0; i < d->n; i++) {
        if (d->ops[i]->pause == btn || d->ops[i]->cancel == btn) {
            *isPause = d->ops[i]->pause == btn;
            return d->ops[i];
        }
    }
    return NULL;
}

static void RoundFill(Dlg *d, HDC dc, RECT rc, COLORREF face, int radius)
{
    HBRUSH b = CreateSolidBrush(face);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

static void FocusRing(Dlg *d, HDC dc, RECT rc, int radius)
{
    HPEN ring = CreatePen(PS_SOLID, S(d, 2), d->accent);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH)), op = SelectObject(dc, ring);
    RoundRect(dc, rc.left + 1, rc.top + 1, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(ring);
}

// ---- File-by-file conflict picker: shared types and drawing ----
#define ID_PICK_OK 410
#define ID_PICK_BACK 411
#define ID_PICK_ALL 412  // +0 the source column, +1 the destination column
#define ID_PICK_LIST 414
#define PICK_W 680
#define PICK_ROW_H 64
#define PICK_HEAD_H 132  // title, explanation and the two column headers
#define PICK_FOOT_H 68

typedef struct {
    ULONGLONG size[2];
    FILETIME mt[2];
    int icon[2];  // system image list index
    BOOL stat;    // size, date and icon looked up (only for rows that get painted)
    BYTE keep;    // bit 0: keep the source version, bit 1: keep the one already at the destination
} PickRow;

typedef struct {
    HWND hwnd, list, ok, back, all[2];
    Op *op;
    int n, focus, hot;  // focus and hot: row * 2 + column, or -1
    PWSTR *src, *dst;   // copies: the engine's arrays may go away if the operation is cancelled
    PickRow *rows;
    HIMAGELIST icons;
} Pick;

static Pick *gPick;  // UI thread only; the owner window is disabled while it is open, so there is one at most

// 0 none of the column is kept, 1 some, 2 all
static int PickColState(const Pick *p, int col)
{
    int on = 0;
    for (int i = 0; i < p->n; i++)
        on += (p->rows[i].keep >> col) & 1;
    return on == 0 ? 0 : on == p->n ? 2 : 1;
}

static int PickUndecided(const Pick *p)  // files with neither version kept
{
    int n = 0;
    for (int i = 0; i < p->n; i++)
        n += !p->rows[i].keep;
    return n;
}

static void DrawCheck(Dlg *d, HDC dc, RECT box, int state)  // state: 0 off, 1 mixed, 2 on
{
    int r = S(d, 5);
    if (state) {
        RoundFill(d, dc, box, d->accent, r);
        SetTextColor(dc, RGB(255, 255, 255));
        if (state == 2) {
            SelectObject(dc, d->fontChevron);
            DrawTextW(dc, L"\xE73E", -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);  // CheckMark
        } else {
            HBRUSH w = CreateSolidBrush(RGB(255, 255, 255));
            int cy = (box.top + box.bottom) / 2, t = max(2, S(d, 2));
            RECT dash = { box.left + S(d, 5), cy - t / 2, box.right - S(d, 5), cy - t / 2 + t };
            FillRect(dc, &dash, w);
            DeleteObject(w);
        }
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, max(1, S(d, 1)), Mix(Pal(d)->sub, Pal(d)->bg, 0.8));
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH)), op = SelectObject(dc, pen);
    RoundRect(dc, box.left, box.top, box.right, box.bottom, r, r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static void DrawButton(Dlg *d, const DRAWITEMSTRUCT *di)
{
    const Palette *pal = Pal(d);
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    HBRUSH bg = CreateSolidBrush(pal->bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    BOOL disabled = di->itemState & ODS_DISABLED, focus = di->itemState & ODS_FOCUS;
    BOOL pressed = di->itemState & ODS_SELECTED, hovered = !disabled && di->hwndItem == d->hover;
    UINT id = di->CtlID;
    BOOL conflictBtn = id >= ID_CONFLICT && id < ID_CONFLICT + CONFLICT_BTNS;
    if (conflictBtn || id == ID_PICK_OK || id == ID_PICK_BACK) {
        // Keep both / Continue are the safe answers, so they get the accent; the destructive one stays quiet.
        BOOL accent = id == ID_CONFLICT + 2 || id == ID_PICK_OK;
        COLORREF fill = accent ? (disabled ? Mix(d->accent, pal->bg, 0.35) : pressed ? Mix(d->accent, RGB(0, 0, 0), 0.75) : hovered ? Mix(d->accent, RGB(0, 0, 0), 0.88) : d->accent)
                               : (pressed ? pal->btnPressed : hovered ? pal->btnHover : pal->btn);
        HBRUSH fb = CreateSolidBrush(fill);
        HPEN bp = CreatePen(PS_SOLID, 1, accent ? fill : pal->btnBorder);
        HGDIOBJ ob = SelectObject(dc, fb), op = SelectObject(dc, bp);
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, S(d, 8), S(d, 8));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(fb);
        DeleteObject(bp);
        if (focus)
            FocusRing(d, dc, rc, S(d, 8));
        WCHAR label[48];
        GetWindowTextW(di->hwndItem, label, 48);
        SetTextColor(dc, accent ? (disabled ? Mix(RGB(255, 255, 255), pal->bg, 0.6) : RGB(255, 255, 255)) : pal->text);
        SelectObject(dc, d->font);
        DrawTextW(dc, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    if (id == ID_PICK_ALL || id == ID_PICK_ALL + 1) {  // column header: select-all checkbox with the folder's name
        int col = id - ID_PICK_ALL;
        if (pressed || hovered)
            RoundFill(d, dc, rc, pressed ? pal->btnPressed : pal->btnHover, S(d, 8));
        if (focus)
            FocusRing(d, dc, rc, S(d, 8));
        int box = S(d, 20), cy = (rc.top + rc.bottom) / 2;
        RECT cb = { rc.left + S(d, 14), cy - box / 2, rc.left + S(d, 14) + box, cy - box / 2 + box };
        DrawCheck(d, dc, cb, gPick ? PickColState(gPick, col) : 0);
        WCHAR label[MAX_PATH + 16];
        GetWindowTextW(di->hwndItem, label, MAX_PATH + 16);
        RECT tr = { cb.right + S(d, 12), rc.top, rc.right - S(d, 8), rc.bottom };
        SetTextColor(dc, pal->text);
        SelectObject(dc, d->font);
        DrawTextW(dc, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return;
    }
    // Like Windows 11's subtle buttons: no surface at rest, a soft one on hover/press.
    if (pressed || hovered)
        RoundFill(d, dc, rc, pressed ? pal->btnPressed : pal->btnHover, S(d, 8));
    if (focus)
        FocusRing(d, dc, rc, S(d, 8));

    if (di->CtlID == ID_DETAILS) {
        WCHAR text[32];
        GetWindowTextW(di->hwndItem, text, 32);
        RECT ir = { rc.left + S(d, 8), rc.top, rc.left + S(d, 28), rc.bottom };
        SetTextColor(dc, pal->text);
        SelectObject(dc, d->fontChevron);
        DrawTextW(dc, d->expanded ? L"\xE70E" : L"\xE70D", -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT tr = { ir.right + S(d, 4), rc.top, rc.right, rc.bottom };
        SelectObject(dc, d->font);
        DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    BOOL isPause = FALSE;
    Op *o = OpOfButton(d, di->hwndItem, &isPause);
    PCWSTR glyph = !isPause ? L"\xE711" : o && o->prog->paused ? L"\xE768" : L"\xE769";  // Cancel / Play / Pause
    SetTextColor(dc, disabled ? Mix(pal->sub, pal->bg, 0.5) : pressed ? pal->sub : pal->text);
    SelectObject(dc, d->fontGlyph);
    DrawTextW(dc, glyph, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// Owner-drawn buttons get no hover state from Windows: track it here.
static LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    Dlg *d = (Dlg *)ref;
    (void)id;
    if (msg == WM_MOUSEMOVE && d->hover != hwnd) {
        HWND prev = d->hover;
        d->hover = hwnd;
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(hwnd, NULL, FALSE);
        if (prev)
            InvalidateRect(prev, NULL, FALSE);
    } else if (msg == WM_MOUSELEAVE && d->hover == hwnd) {
        d->hover = NULL;
        InvalidateRect(hwnd, NULL, FALSE);
    } else if (msg == WM_NCDESTROY && d->hover == hwnd) {
        d->hover = NULL;
    }
    if (msg == WM_MOUSEWHEEL || msg == WM_POINTERWHEEL)  // scroll the list even when the pointer is over an icon button
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void SetTip(Dlg *d, HWND btn, PCWSTR text, UINT msg)
{
    TTTOOLINFOW ti = { sizeof(ti) };
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = d->hwnd;
    ti.uId = (UINT_PTR)btn;
    ti.lpszText = (LPWSTR)text;
    SendMessageW(d->tip, msg, 0, (LPARAM)&ti);
    SetWindowTextW(btn, text);  // also the name screen readers announce
}

static HWND IconButton(Dlg *d, PCWSTR tip)
{
    HWND b = CreateWindowExW(0, L"BUTTON", tip, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, d->hwnd,
                             (HMENU)(UINT_PTR)ID_OPBUTTON, gInst, NULL);
    SetWindowSubclass(b, ButtonProc, 1, (DWORD_PTR)d);
    SetTip(d, b, tip, TTM_ADDTOOLW);
    return b;
}

static void Tick(Dlg *d)
{
    ULONGLONG now = GetTickCount64();
    LONG64 sumDone = 0, sumTot = 0;
    BOOL anyRunning = FALSE, anyPlanning = FALSE;
    for (int i = 0; i < d->n; i++) {
        Op *o = d->ops[i];
        FcProgress *pr = o->prog;
        LONG64 bytes = pr->bytesDone;
        BOOL running = pr->phase == FC_PHASE_COPY && !pr->paused;
        if (o->lastTick && now > o->lastTick && running) {
            double inst = (bytes - o->lastBytes) * 1000.0 / (now - o->lastTick);
            o->speed = o->speed ? o->speed * 0.7 + inst * 0.3 : inst;
            o->activeMs += now - o->lastTick;
            if (o->speed > o->peak)
                o->peak = o->speed;
            // One sample per slice of progress; a fast tick fills the slices it skipped.
            if (pr->bytesTotal > 0) {
                int idx = (int)((double)bytes / pr->bytesTotal * (GRAPH_POINTS - 1));
                if (idx >= GRAPH_POINTS)
                    idx = GRAPH_POINTS - 1;
                for (int k = o->graphN; k <= idx; k++)
                    o->graph[k] = (float)o->speed;
                if (idx + 1 > o->graphN)
                    o->graphN = idx + 1;
            }
        }
        BOOL asking = pr->conflictPending != 0;
        if (asking && !o->cbtn[0]) {
            static const PCWSTR names[CONFLICT_BTNS] = { L"Reemplazar", L"Omitir", L"Conservar ambos", L"Elegir archivos…" };
            for (int k = 0; k < CONFLICT_BTNS; k++) {
                o->cbtn[k] = CreateWindowExW(0, L"BUTTON", names[k], WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, d->hwnd,
                                             (HMENU)(UINT_PTR)(ID_CONFLICT + k), gInst, NULL);
                SetWindowSubclass(o->cbtn[k], ButtonProc, 1, (DWORD_PTR)d);
            }
        }
        if (asking && !o->cseen) {  // bring the question into view; scrolling away is the user's call afterwards
            o->cseen = TRUE;
            int abs = PanelTop(d, i) + d->scroll, viewTop = S(d, MARGIN) + HeaderH(d);
            RECT crc;
            GetClientRect(d->hwnd, &crc);
            if (abs - d->scroll < viewTop || abs - d->scroll + S(d, CONFLICT_H) > crc.bottom - S(d, FOOTER_H))
                d->scroll = max(0, abs - viewTop);
        } else if (!asking) {
            o->cseen = FALSE;
        }
        if ((asking ? 1.0 : 0.0) != o->cf)
            StartFrames(d);
        o->lastTick = now;
        o->lastBytes = bytes;
        sumDone += bytes;
        sumTot += pr->bytesTotal;
        anyRunning |= running;
        anyPlanning |= pr->phase == FC_PHASE_PLAN;
    }

    WCHAR title[64];
    int pct = sumTot ? (int)(sumDone * 100 / sumTot) : 0;
    if (d->n > 1)
        swprintf_s(title, 64, L"%d operaciones en curso", d->n);
    else
        swprintf_s(title, 64, L"%d %% completado", pct);
    SetWindowTextW(d->hwnd, title);
    if (d->taskbar) {
        if (!sumTot && anyPlanning) {
            ITaskbarList3_SetProgressState(d->taskbar, d->hwnd, TBPF_INDETERMINATE);
        } else {
            ITaskbarList3_SetProgressState(d->taskbar, d->hwnd, anyRunning || anyPlanning ? TBPF_NORMAL : TBPF_PAUSED);
            ITaskbarList3_SetProgressValue(d->taskbar, d->hwnd, sumDone, max(sumTot, 1));
        }
    }
    StartFrames(d);  // the frame loop eases progress and stops itself once settled
}

static void CancelOp(Op *o)
{
    o->prog->cancel = 1;
    o->prog->paused = 0;
    EnableWindow(o->pause, FALSE);
    EnableWindow(o->cancel, FALSE);
}

static void WheelScroll(Dlg *d, int delta)
{
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    int step = lines == WHEEL_PAGESCROLL ? d->h * 3 / 4 : (int)lines * S(d, 20);
    d->scroll = max(0, d->scroll - delta * step / WHEEL_DELTA);  // Frame clamps the far end
    Frame(d);
}

// ---- File-by-file conflict picker: window ----
// Opened from the "Elegir archivos…" button. A modeless window owned by the progress window, which is
// disabled while it is open. Every conflicting file shows its two versions side by side with a checkbox
// each, like Windows' own dialog: source only = replace, destination only = skip, both = keep both.
static const WCHAR kPickClass[] = L"FastCopyPicker", kListClass[] = L"FastCopyPickList";

static PCWSTR NoPrefix(PCWSTR p) { return !wcsncmp(p, L"\\\\?\\", 4) ? p + 4 : p; }

static void PickStat(Pick *p, int i)
{
    PickRow *r = &p->rows[i];
    if (r->stat)
        return;
    r->stat = TRUE;
    for (int c = 0; c < 2; c++) {
        PCWSTR path = c ? p->dst[i] : p->src[i], plain = NoPrefix(path);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
            r->size[c] = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            r->mt[c] = fa.ftLastWriteTime;
        }
        SHFILEINFOW fi = { 0 };
        HIMAGELIST il;
        if (wcslen(plain) < MAX_PATH)
            il = (HIMAGELIST)SHGetFileInfoW(plain, 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
        else  // the shell API takes no long paths: the icon of the type will do
            il = (HIMAGELIST)SHGetFileInfoW(PathFindFileNameW(plain), FILE_ATTRIBUTE_NORMAL, &fi, sizeof(fi),
                                            SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
        r->icon[c] = il ? fi.iIcon : -1;
        if (il)
            p->icons = il;
    }
}

static void PickDetail(const PickRow *r, int c, PWSTR out, int cap)
{
    if (!r->mt[c].dwHighDateTime && !r->mt[c].dwLowDateTime) {
        wcscpy_s(out, cap, L"No disponible");
        return;
    }
    WCHAR size[32], date[64], time[32];
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&r->mt[c], &local);
    FileTimeToSystemTime(&local, &st);
    StrFormatByteSizeW((LONGLONG)r->size[c], size, 32);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, date, 64, NULL);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, time, 32);
    swprintf_s(out, cap, L"%s · %s %s", size, date, time);
}

static void PickCols(Dlg *d, int clientW, int *x, int *w)
{
    int pad = S(d, 16), gap = S(d, 12);
    *w = (clientW - 2 * pad - gap) / 2;
    x[0] = pad;
    x[1] = pad + *w + gap;
}

static RECT PickCell(Dlg *d, int row, int col, int clientW, int scroll)
{
    int x[2], cw, inset = S(d, 4), top = row * S(d, PICK_ROW_H) - scroll;
    PickCols(d, clientW, x, &cw);
    RECT r = { x[col], top + inset, x[col] + cw, top + S(d, PICK_ROW_H) - inset };
    return r;
}

static int PickScroll(Pick *p)
{
    SCROLLINFO si = { sizeof(si), SIF_POS };
    GetScrollInfo(p->list, SB_VERT, &si);
    return si.nPos;
}

static void PickSetScroll(Pick *p, int pos)
{
    RECT rc;
    GetClientRect(p->list, &rc);
    int top = max(0, p->n * S(&gDlg, PICK_ROW_H) - rc.bottom);
    SCROLLINFO si = { sizeof(si), SIF_POS, 0, 0, 0, max(0, min(pos, top)) };
    SetScrollInfo(p->list, SB_VERT, &si, TRUE);
    InvalidateRect(p->list, NULL, FALSE);
}

static void PickReveal(Pick *p, int row)
{
    RECT rc;
    GetClientRect(p->list, &rc);
    int rowH = S(&gDlg, PICK_ROW_H), pos = PickScroll(p);
    if (row * rowH < pos)
        pos = row * rowH;
    else if ((row + 1) * rowH > pos + rc.bottom)
        pos = (row + 1) * rowH - rc.bottom;
    PickSetScroll(p, pos);
}

static void PickLayoutHeads(Pick *p)
{
    Dlg *d = &gDlg;
    RECT lc;
    GetClientRect(p->list, &lc);
    int x[2], cw;
    PickCols(d, lc.right, x, &cw);
    for (int c = 0; c < 2; c++)
        SetWindowPos(p->all[c], NULL, x[c], S(d, 88), cw, S(d, 36), SWP_NOZORDER | SWP_NOACTIVATE);
}

static void PickLayout(Pick *p)
{
    Dlg *d = &gDlg;
    if (!p->list || !p->back)  // still being created
        return;
    RECT rc;
    GetClientRect(p->hwnd, &rc);
    int head = S(d, PICK_HEAD_H), foot = S(d, PICK_FOOT_H), bw = S(d, 120), bh = S(d, 34), m = S(d, 24);
    SetWindowPos(p->list, NULL, 0, head, rc.right, max(0, rc.bottom - head - foot), SWP_NOZORDER | SWP_NOACTIVATE);
    int by = rc.bottom - foot + (foot - bh) / 2;
    SetWindowPos(p->ok, NULL, rc.right - m - bw, by, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(p->back, NULL, rc.right - m - 2 * bw - S(d, 8), by, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE, 0, p->n * S(d, PICK_ROW_H) - 1, 0 };
    GetClientRect(p->list, &rc);
    si.nPage = rc.bottom;
    SetScrollInfo(p->list, SB_VERT, &si, TRUE);
    PickSetScroll(p, PickScroll(p));
    PickLayoutHeads(p);
}

static void PickRefresh(Pick *p)
{
    EnableWindow(p->ok, PickUndecided(p) == 0);
    InvalidateRect(p->list, NULL, FALSE);
    InvalidateRect(p->all[0], NULL, FALSE);
    InvalidateRect(p->all[1], NULL, FALSE);
    InvalidateRect(p->ok, NULL, FALSE);
    InvalidateRect(p->hwnd, NULL, FALSE);
}

static void PickToggle(Pick *p, int cell)
{
    p->rows[cell / 2].keep ^= (BYTE)(1 << (cell % 2));
    PickRefresh(p);
}

static void PickPaintList(Pick *p, HDC target, RECT rc)
{
    Dlg *d = &gDlg;
    const Palette *pal = Pal(d);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HBRUSH b = CreateSolidBrush(pal->bg);
    FillRect(dc, &rc, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);

    int rowH = S(d, PICK_ROW_H), scroll = PickScroll(p), first = scroll / rowH, last = min(p->n - 1, (scroll + rc.bottom) / rowH);
    BOOL listFocus = GetFocus() == p->list;
    for (int i = first; i <= last; i++) {
        PickStat(p, i);
        PickRow *r = &p->rows[i];
        LONG newer = CompareFileTime(&r->mt[0], &r->mt[1]);
        for (int c = 0; c < 2; c++) {
            RECT cell = PickCell(d, i, c, rc.right, scroll);
            BOOL kept = (r->keep >> c) & 1;
            HBRUSH fb = CreateSolidBrush(p->hot == i * 2 + c ? pal->btnHover : pal->card);
            HPEN bp = CreatePen(PS_SOLID, 1, pal->cardBorder);
            HGDIOBJ ob = SelectObject(dc, fb), op = SelectObject(dc, bp);
            RoundRect(dc, cell.left, cell.top, cell.right, cell.bottom, S(d, 8), S(d, 8));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(fb);
            DeleteObject(bp);

            int cy = (cell.top + cell.bottom) / 2, box = S(d, 20);
            RECT cb = { cell.left + S(d, 14), cy - box / 2, cell.left + S(d, 14) + box, cy - box / 2 + box };
            DrawCheck(d, dc, cb, kept ? 2 : 0);
            int ix = cb.right + S(d, 12), slot = S(d, 36);
            if (p->icons && r->icon[c] >= 0) {
                int iw = 32, ih = 32;
                ImageList_GetIconSize(p->icons, &iw, &ih);
                ImageList_Draw(p->icons, r->icon[c], dc, ix + (slot - iw) / 2, cy - ih / 2, ILD_TRANSPARENT);
            }
            // Versions that will not be kept fade, so the result reads at a glance down the column.
            COLORREF nameCol = kept ? pal->text : Mix(pal->sub, pal->bg, 0.7), subCol = kept ? pal->sub : Mix(pal->sub, pal->bg, 0.55);
            int tx = ix + slot + S(d, 12), line = S(d, 20), ty = cy - S(d, 19);
            RECT nr = { tx, ty, cell.right - S(d, 12), ty + line };
            if ((c == 0 && newer > 0) || (c == 1 && newer < 0)) {
                RECT tag = nr;
                SelectObject(dc, d->font);
                DrawTextW(dc, L"Más reciente", -1, &tag, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
                int tw = tag.right - tag.left;
                RECT tr = { nr.right - tw, nr.top, nr.right, nr.bottom };
                SetTextColor(dc, kept ? d->accent : Mix(d->accent, pal->bg, 0.6));
                DrawTextW(dc, L"Más reciente", -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                nr.right -= tw + S(d, 8);
            }
            SelectObject(dc, d->font);
            SetTextColor(dc, nameCol);
            DrawTextW(dc, PathFindFileNameW(NoPrefix(c ? p->dst[i] : p->src[i])), -1, &nr,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            WCHAR detail[128];
            PickDetail(r, c, detail, 128);
            RECT dr = { tx, ty + line, cell.right - S(d, 12), ty + 2 * line - S(d, 2) };
            SetTextColor(dc, subCol);
            DrawTextW(dc, detail, -1, &dr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (listFocus && p->focus == i * 2 + c)
                FocusRing(d, dc, cell, S(d, 8));
        }
    }
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static void PickScrollBy(Pick *p, int delta)
{
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    RECT rc;
    GetClientRect(p->list, &rc);
    int step = lines == WHEEL_PAGESCROLL ? rc.bottom * 3 / 4 : (int)lines * S(&gDlg, 20);
    PickSetScroll(p, PickScroll(p) - delta * step / WHEEL_DELTA);
}

static int PickHit(Pick *p, int x, int y)  // the cell under a point of the list, or -1
{
    Dlg *d = &gDlg;
    RECT rc;
    GetClientRect(p->list, &rc);
    int scroll = PickScroll(p), row = (y + scroll) / S(d, PICK_ROW_H);
    if (y < 0 || row >= p->n)
        return -1;
    for (int c = 0; c < 2; c++) {
        RECT cell = PickCell(d, row, c, rc.right, scroll);
        POINT pt = { x, y };
        if (PtInRect(&cell, pt))
            return row * 2 + c;
    }
    return -1;
}

static LRESULT CALLBACK PickListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Pick *p = gPick;
    if (!p)
        return DefWindowProcW(hwnd, msg, wp, lp);
    RECT rc;
    GetClientRect(hwnd, &rc);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        PickPaintList(p, dc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        PickLayout(p);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        PickScrollBy(p, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos, line = S(&gDlg, 20);
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        }
        PickSetScroll(p, pos);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        int cell = PickHit(p, (short)LOWORD(lp), (short)HIWORD(lp));
        if (cell >= 0) {
            p->focus = cell;
            PickToggle(p, cell);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int cell = PickHit(p, (short)LOWORD(lp), (short)HIWORD(lp));
        if (cell != p->hot) {
            p->hot = cell;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        p->hot = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KEYDOWN: {
        int row = p->focus / 2, col = p->focus % 2, page = max(1, rc.bottom / S(&gDlg, PICK_ROW_H));
        switch (wp) {
        case VK_LEFT: col = 0; break;
        case VK_RIGHT: col = 1; break;
        case VK_UP: row--; break;
        case VK_DOWN: row++; break;
        case VK_PRIOR: row -= page; break;
        case VK_NEXT: row += page; break;
        case VK_HOME: row = 0; break;
        case VK_END: row = p->n - 1; break;
        case VK_SPACE:
            PickToggle(p, p->focus);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        row = max(0, min(row, p->n - 1));
        p->focus = row * 2 + col;
        PickReveal(p, row);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ClosePick(Dlg *d, BOOL commit)
{
    Pick *p = gPick;
    if (!p)
        return;
    gPick = NULL;
    FcProgress *pr = p->op->prog;
    // The engine is still waiting (cancelling is done on this thread and Op removal closes the picker first),
    // so its pick array is alive.
    if (commit && pr->conflictPending && pr->conflictPick && !pr->cancel && pr->conflictCount == p->n) {
        for (int i = 0; i < p->n; i++) {
            BYTE k = p->rows[i].keep;
            pr->conflictPick[i] = k == 1 ? FC_CONFLICT_REPLACE : k == 2 ? FC_CONFLICT_SKIP : FC_CONFLICT_BOTH;
        }
        InterlockedExchange(&pr->conflictChoice, FC_CONFLICT_EACH);
        pr->conflictPending = 0;
        StartFrames(d);
    }
    EnableWindow(d->hwnd, TRUE);  // before destroying, or Windows activates some other app's window
    DestroyWindow(p->hwnd);
    for (int i = 0; i < p->n; i++) {
        free(p->src[i]);
        free(p->dst[i]);
    }
    free(p->src);
    free(p->dst);
    free(p->rows);
    free(p);
}

static void PickPaint(Pick *p, HDC dc, RECT rc)
{
    Dlg *d = &gDlg;
    const Palette *pal = Pal(d);
    int m = S(d, 24);
    HBRUSH b = CreateSolidBrush(pal->bg);
    FillRect(dc, &rc, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);

    RECT t = { m, S(d, 20), rc.right - m, S(d, 52) };
    SelectObject(dc, d->fontTitle);
    SetTextColor(dc, pal->text);
    DrawTextW(dc, L"Elige qué archivos conservar", -1, &t, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT s = { m, S(d, 54), rc.right - m, S(d, 86) };
    SelectObject(dc, d->font);
    SetTextColor(dc, pal->sub);
    DrawTextW(dc, L"Marca la versión que quieres conservar de cada archivo. Si marcas las dos, se conservan ambas y la copia recibe un número en el nombre.",
              -1, &s, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);

    int foot = rc.bottom - S(d, PICK_FOOT_H);
    HPEN line = CreatePen(PS_SOLID, 1, pal->cardBorder);
    HGDIOBJ op = SelectObject(dc, line);
    MoveToEx(dc, 0, foot, NULL);
    LineTo(dc, rc.right, foot);
    SelectObject(dc, op);
    DeleteObject(line);

    WCHAR msg[96];
    int undecided = PickUndecided(p);
    if (undecided)
        swprintf_s(msg, 96, undecided == 1 ? L"Marca al menos una versión de %d archivo" : L"Marca al menos una versión de %d archivos", undecided);
    else
        swprintf_s(msg, 96, L"%d archivos con el mismo nombre", p->n);
    RECT h;
    GetWindowRect(p->back, &h);
    MapWindowPoints(NULL, p->hwnd, (POINT *)&h, 2);
    RECT mr = { m, foot, h.left - S(d, 12), rc.bottom };
    SetTextColor(dc, undecided ? (d->dark ? RGB(252, 225, 0) : kPausedColor) : pal->sub);
    DrawTextW(dc, msg, -1, &mr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static LRESULT CALLBACK PickProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Pick *p = gPick;
    Dlg *d = &gDlg;
    if (!p)
        return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC target = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(dc, bmp);
        PickPaint(p, dc, rc);
        BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM:
        DrawButton(d, (DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_SETFOCUS:
        SetFocus(p->list);
        return 0;
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id == ID_PICK_ALL || id == ID_PICK_ALL + 1) {
            int col = id - ID_PICK_ALL, on = PickColState(p, col) != 2;
            for (int i = 0; i < p->n; i++)
                p->rows[i].keep = (BYTE)(on ? p->rows[i].keep | (1 << col) : p->rows[i].keep & ~(1 << col));
            PickRefresh(p);
        } else if (id == ID_PICK_OK || id == IDOK) {
            if (!PickUndecided(p))
                ClosePick(d, TRUE);
        } else if (id == ID_PICK_BACK || id == IDCANCEL) {
            ClosePick(d, FALSE);  // back to the question; the operation goes on waiting
        }
        return 0;
    }
    case WM_CLOSE:
        ClosePick(d, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void SetScrollTheme(Dlg *d, HWND w)  // dark scroll bars in dark mode
{
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    typedef HRESULT(WINAPI * SetThemeFn)(HWND, LPCWSTR, LPCWSTR);
    SetThemeFn f = ux ? (SetThemeFn)GetProcAddress(ux, "SetWindowTheme") : NULL;
    if (f)
        f(w, d->dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
}

static void OpenPick(Dlg *d, Op *o)
{
    FcProgress *pr = o->prog;
    int n = pr->conflictCount;
    if (gPick || n < 2 || !pr->conflictPending || !pr->conflictSrc || !pr->conflictPick)
        return;
    Pick *p = calloc(1, sizeof(Pick));
    p->op = o;
    p->n = n;
    p->hot = -1;
    p->src = calloc(n, sizeof(PWSTR));
    p->dst = calloc(n, sizeof(PWSTR));
    p->rows = calloc(n, sizeof(PickRow));
    for (int i = 0; i < n; i++) {
        p->src[i] = _wcsdup(pr->conflictSrc[i]);
        p->dst[i] = _wcsdup(pr->conflictDst[i]);
        p->rows[i].keep = 3;  // keeping both loses nothing, so it is where the user starts
        p->rows[i].icon[0] = p->rows[i].icon[1] = -1;
    }
    gPick = p;

    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(d->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT wr = { 0, 0, S(d, PICK_W), 0 };
    AdjustWindowRectExForDpi(&wr, style, FALSE, WS_EX_CONTROLPARENT, d->dpi);
    int frameH = wr.bottom - wr.top;
    int ch = min(S(d, 600), (mi.rcWork.bottom - mi.rcWork.top) * 85 / 100 - frameH);
    wr.bottom = max(ch, S(d, PICK_HEAD_H + PICK_FOOT_H + PICK_ROW_H));
    AdjustWindowRectExForDpi(&wr, style, FALSE, WS_EX_CONTROLPARENT, d->dpi);
    RECT owner;
    GetWindowRect(d->hwnd, &owner);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    int x = max(mi.rcWork.left, min((owner.left + owner.right - w) / 2, mi.rcWork.right - w));
    int y = max(mi.rcWork.top, min((owner.top + owner.bottom - h) / 2, mi.rcWork.bottom - h));
    p->hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kPickClass, L"Reemplazar u omitir archivos", style, x, y, w, h, d->hwnd, NULL, gInst, NULL);
    if (!p->hwnd) {
        gPick = NULL;
        for (int i = 0; i < n; i++) {
            free(p->src[i]);
            free(p->dst[i]);
        }
        free(p->src);
        free(p->dst);
        free(p->rows);
        free(p);
        return;
    }
    BOOL dark = d->dark;
    if (FAILED(DwmSetWindowAttribute(p->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))))
        DwmSetWindowAttribute(p->hwnd, 19, &dark, sizeof(dark));

    // Tab order is creation order: the two select-all headers, the list, then the buttons.
    WCHAR label[MAX_PATH + 16];
    for (int c = 0; c < 2; c++) {
        swprintf_s(label, MAX_PATH + 16, c ? L"Destino · %s" : L"Origen · %s", c ? o->destName : o->srcName);
        p->all[c] = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, p->hwnd,
                                    (HMENU)(UINT_PTR)(ID_PICK_ALL + c), gInst, NULL);
        SetWindowSubclass(p->all[c], ButtonProc, 1, (DWORD_PTR)d);
    }
    p->list = CreateWindowExW(0, kListClass, L"Archivos en conflicto", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0, 0, 0, 0, p->hwnd, (HMENU)ID_PICK_LIST, gInst, NULL);
    SetScrollTheme(d, p->list);
    p->ok = CreateWindowExW(0, L"BUTTON", L"Continuar", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, p->hwnd,
                            (HMENU)ID_PICK_OK, gInst, NULL);
    p->back = CreateWindowExW(0, L"BUTTON", L"Volver", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, p->hwnd,
                              (HMENU)ID_PICK_BACK, gInst, NULL);
    SetWindowSubclass(p->ok, ButtonProc, 1, (DWORD_PTR)d);
    SetWindowSubclass(p->back, ButtonProc, 1, (DWORD_PTR)d);
    PickLayout(p);
    EnableWindow(d->hwnd, FALSE);
    ShowWindow(p->hwnd, SW_SHOWNORMAL);
    SetFocus(p->list);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Dlg *d = &gDlg;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(d, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case DM_POINTERHITTEST: {  // a touchpad contact begins over the window: hand it to DirectManipulation
        POINTER_INPUT_TYPE type;
        UINT32 id = GET_POINTERID_WPARAM(wp);
        if (d->dmView && GetPointerType(id, &type) && type == PT_TOUCHPAD)
            IDirectManipulationViewport_SetContact(d->dmView, id);
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_POINTERWHEEL:  // processes that turn the mouse into pointer input
        WheelScroll(d, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_LBUTTONDOWN: {
        RECT t, rc;
        int top, h, x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        GetClientRect(hwnd, &rc);
        if (x >= rc.right - S(d, 18) && ThumbGeom(d, &t, &top, &h)) {
            if (y >= t.top && y < t.bottom) {
                d->drag = TRUE;
                d->grab = y - t.top;
                SetCapture(hwnd);
            } else {  // click on the track: a page up or down
                d->scroll += (y < t.top ? -1 : 1) * h * 9 / 10;
                d->scroll = max(0, d->scroll);
                Frame(d);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (d->drag) {
            RECT t;
            int top, h, y = (short)HIWORD(lp);
            if (ThumbGeom(d, &t, &top, &h)) {
                int range = h - (t.bottom - t.top);
                d->scroll = range > 0 ? max(0, (y - d->grab - top) * (d->content - h) / range) : 0;
                Frame(d);
            }
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (d->drag) {
            d->drag = FALSE;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (d->details) {
            Layout(d);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_DRAWITEM:
        DrawButton(d, (DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_TIMER:
        if (wp == TIMER_FRAME) {
            if (!Frame(d))
                KillTimer(hwnd, TIMER_FRAME);
        } else {
            Tick(d);
        }
        return 0;
    case WM_COMMAND: {
        BOOL isPause = FALSE;
        Op *o = OpOfButton(d, (HWND)lp, &isPause);
        Op *ask = NULL;
        for (int i = 0; i < d->n && !ask; i++)
            for (int k = 0; k < CONFLICT_BTNS; k++)
                if (d->ops[i]->cbtn[k] && d->ops[i]->cbtn[k] == (HWND)lp)
                    ask = d->ops[i];
        if (ask && LOWORD(wp) == ID_CONFLICT + 3) {
            OpenPick(d, ask);  // the question stays open until the list is answered
        } else if (ask) {
            InterlockedExchange(&ask->prog->conflictChoice, LOWORD(wp) - ID_CONFLICT);
            ask->prog->conflictPending = 0;
            StartFrames(d);
        } else if (LOWORD(wp) == ID_DETAILS) {
            ToggleDetails(d);
            InvalidateRect(d->details, NULL, FALSE);
        } else if (o && isPause) {
            o->prog->paused = !o->prog->paused;
            SetTip(d, o->pause, o->prog->paused ? L"Reanudar" : L"Pausar", TTM_UPDATETIPTEXTW);
            InvalidateRect(o->pause, NULL, FALSE);
            Tick(d);
        } else if (o) {
            CancelOp(o);
        } else if (LOWORD(wp) == IDCANCEL && d->n == 1) {
            CancelOp(d->ops[0]);  // Esc cancels only when there is no doubt which operation
        }
        return 0;
    }
    case WM_CLOSE:
        for (int i = 0; i < d->n; i++)
            CancelOp(d->ops[i]);  // the window goes away when the last operation stops
        return 0;
    case WM_DPICHANGED: {
        RECT *s = (RECT *)lp;
        d->dpi = HIWORD(wp);
        LoadFonts(d);
        SetWindowPos(hwnd, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE);
        d->h = LayoutHeight(d);
        ApplyHeight(d);
        Layout(d);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

#define DM_SIZE 1000     // touchpad viewport
#define DM_TALL 200000   // content far taller than the viewport, so a pan never reaches its edge and bounces;
                         // it starts in the middle and is put back there after every pan
static const GUID kClsidDm = { 0x54E211B6, 0x3650, 0x4F75, { 0x83, 0x34, 0xFA, 0x35, 0x95, 0x98, 0xE1, 0xC5 } };
static const GUID kIidDm = { 0xFBF5D3B4, 0x70C7, 0x4163, { 0x93, 0x22, 0x5A, 0x6F, 0x66, 0x0D, 0x6F, 0xBC } };
static const GUID kIidDmUpdate = { 0xB0AE62FD, 0xBE34, 0x46E7, { 0x9C, 0xAA, 0xD3, 0x61, 0xFA, 0xCB, 0xB9, 0xCC } };
static const GUID kIidDmView = { 0x28B85A3D, 0x60A0, 0x48BD, { 0x9B, 0xA1, 0x5C, 0xE8, 0xD9, 0xEA, 0x3A, 0x6D } };
static const GUID kIidDmContent = { 0xB89962CB, 0x3D89, 0x442B, { 0xBB, 0x58, 0x50, 0x98, 0xFA, 0x0F, 0x9F, 0x16 } };
static const GUID kIidDmHandler = { 0x952121DA, 0xD69F, 0x45F9, { 0xB0, 0xF9, 0xF2, 0x39, 0x44, 0x32, 0x1A, 0x6D } };

static void DmRecentre(Dlg *d)
{
    d->dmY = -(float)(DM_TALL / 2);  // the content offset ZoomToRect leaves
    d->dmRem = 0;
    IDirectManipulationViewport_ZoomToRect(d->dmView, 0, DM_TALL / 2, DM_SIZE, DM_TALL / 2 + DM_SIZE, FALSE);
}

static HRESULT STDMETHODCALLTYPE DmQueryInterface(IDirectManipulationViewportEventHandler *self, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &kIidDmHandler)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE DmRef(IDirectManipulationViewportEventHandler *self) { (void)self; return 1; }  // static object

static HRESULT STDMETHODCALLTYPE DmStatus(IDirectManipulationViewportEventHandler *self, IDirectManipulationViewport *view,
                                          DIRECTMANIPULATION_STATUS now, DIRECTMANIPULATION_STATUS before)
{
    (void)self;
    (void)before;
    (void)view;
    Dlg *d = &gDlg;
    d->dmActive = now == DIRECTMANIPULATION_RUNNING || now == DIRECTMANIPULATION_INERTIA;
    if (d->dmActive) {
        StartFrames(d);
    } else if (now == DIRECTMANIPULATION_READY) {  // pan and inertia done: back to the middle for the next one
        DmRecentre(d);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DmUpdated(IDirectManipulationViewportEventHandler *self, IDirectManipulationViewport *view)
{
    (void)self;
    (void)view;
    return S_OK;
}

// The content offset is where the fingers dragged it; the list scrolls by how much that changed.
static HRESULT STDMETHODCALLTYPE DmContent(IDirectManipulationViewportEventHandler *self, IDirectManipulationViewport *view,
                                           IDirectManipulationContent *content)
{
    (void)self;
    (void)view;
    Dlg *d = &gDlg;
    float m[6];
    if (FAILED(IDirectManipulationContent_GetContentTransform(content, m, 6)))
        return S_OK;
    d->dmRem += m[5] - d->dmY;
    d->dmY = m[5];
    int px = (int)d->dmRem;
    d->dmRem -= px;
    d->scroll = max(0, d->scroll - px);  // Frame clamps the far end
    return S_OK;
}

static IDirectManipulationViewportEventHandlerVtbl kDmVtbl = { DmQueryInterface, DmRef, DmRef, DmStatus, DmUpdated, DmContent };
static IDirectManipulationViewportEventHandler kDmHandler = { &kDmVtbl };

static void DmInit(Dlg *d)
{
    RECT r = { 0, 0, DM_SIZE, DM_SIZE }, tall = { 0, 0, DM_SIZE, DM_TALL };
    IDirectManipulationContent *content = NULL;
    if (FAILED(CoCreateInstance(&kClsidDm, NULL, CLSCTX_INPROC_SERVER, &kIidDm, (void **)&d->dm)) ||
        FAILED(IDirectManipulationManager_GetUpdateManager(d->dm, &kIidDmUpdate, (void **)&d->dmUpdate)) ||
        FAILED(IDirectManipulationManager_CreateViewport(d->dm, NULL, d->hwnd, &kIidDmView, (void **)&d->dmView)) ||
        FAILED(IDirectManipulationViewport_ActivateConfiguration(d->dmView, DIRECTMANIPULATION_CONFIGURATION_INTERACTION |
                                                                                DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_Y |
                                                                                DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_INERTIA |
                                                                                DIRECTMANIPULATION_CONFIGURATION_RAILS_Y)) ||
        FAILED(IDirectManipulationViewport_SetViewportOptions(d->dmView, DIRECTMANIPULATION_VIEWPORT_OPTIONS_MANUALUPDATE)) ||
        FAILED(IDirectManipulationViewport_AddEventHandler(d->dmView, d->hwnd, &kDmHandler, &d->dmCookie)) ||
        FAILED(IDirectManipulationViewport_SetViewportRect(d->dmView, &r)) ||
        FAILED(IDirectManipulationViewport_GetPrimaryContent(d->dmView, &kIidDmContent, (void **)&content)) ||
        FAILED(IDirectManipulationContent_SetContentRect(content, &tall)) ||
        FAILED(IDirectManipulationManager_Activate(d->dm, d->hwnd)) ||
        FAILED(IDirectManipulationViewport_Enable(d->dmView))) {
        FcLog(L"DirectManipulation unavailable: touchpad scrolling off");
        if (content)
            IDirectManipulationContent_Release(content);
        return;  // the wheel and the scroll thumb still work
    }
    IDirectManipulationContent_Release(content);
    DmRecentre(d);
    IDirectManipulationUpdateManager_Update(d->dmUpdate, NULL);
}

static void DmFree(Dlg *d)
{
    if (d->dmView) {
        IDirectManipulationViewport_Stop(d->dmView);
        if (d->dmCookie)
            IDirectManipulationViewport_RemoveEventHandler(d->dmView, d->dmCookie);
        IDirectManipulationViewport_Abandon(d->dmView);
        IDirectManipulationViewport_Release(d->dmView);
    }
    if (d->dm) {
        IDirectManipulationManager_Deactivate(d->dm, d->hwnd);
        IDirectManipulationManager_Release(d->dm);
    }
    if (d->dmUpdate)
        IDirectManipulationUpdateManager_Release(d->dmUpdate);
}

static BOOL CreateDlgWindow(Dlg *d)
{
    d->dark = RegDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", 1) == 0;
    // DWM stores the accent as 0xAABBGGRR, which is a COLORREF once alpha is dropped.
    d->accent = RegDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFC06700) & 0xFFFFFF;
    d->expanded = RegDword(SETTINGS_KEY, L"ShowDetails", 0) != 0;
    d->t = d->tFrom = d->tTo = d->expanded ? 1 : 0;
    d->motion = TRUE;  // "Animation effects" off in Windows settings => every change is instant
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &d->motion, 0);

    // Centre over the Explorer window the user is working in.
    RECT anchor;
    HWND fg = GetForegroundWindow();
    if (!fg || !GetWindowRect(fg, &anchor))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
    UINT dpi = fg ? GetDpiForWindow(fg) : GetDpiForSystem();
    d->dpi = dpi ? dpi : 96;
    RECT wr = { 0, 0, S(d, WIDTH), S(d, MARGIN + FOOTER_H) };
    AdjustWindowRectExForDpi(&wr, kStyle, FALSE, WS_EX_CONTROLPARENT, d->dpi);
    int w = wr.right - wr.left, x = (anchor.left + anchor.right - w) / 2;
    int y = (anchor.top + anchor.bottom) / 2 - S(d, 120);

    d->hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"FastCopyProgress", L"", kStyle, x, y, w, wr.bottom - wr.top,
                              NULL, NULL, gInst, NULL);
    if (!d->hwnd)
        return FALSE;
    BOOL dark = d->dark;
    if (FAILED(DwmSetWindowAttribute(d->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))))
        DwmSetWindowAttribute(d->hwnd, 19, &dark, sizeof(dark));  // its number before Windows 10 20H1 (1809-1909, Server 2019)
    d->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, d->hwnd, NULL, gInst, NULL);
    d->details = CreateWindowExW(0, L"BUTTON", d->expanded ? L"Menos detalles" : L"Más detalles",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, d->hwnd,
                                 (HMENU)ID_DETAILS, gInst, NULL);
    SetWindowSubclass(d->details, ButtonProc, 1, (DWORD_PTR)d);
    LoadFonts(d);
    CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&d->taskbar);
    if (d->taskbar && FAILED(ITaskbarList3_HrInit(d->taskbar))) {
        ITaskbarList3_Release(d->taskbar);
        d->taskbar = NULL;
    }
    DmInit(d);
    SetTimer(d->hwnd, TIMER_TICK, TICK_MS, NULL);
    return TRUE;
}

static void DestroyDlgWindow(Dlg *d)
{
    if (d->taskbar) {
        ITaskbarList3_SetProgressState(d->taskbar, d->hwnd, TBPF_NOPROGRESS);
        ITaskbarList3_Release(d->taskbar);
    }
    DmFree(d);
    DestroyWindow(d->hwnd);  // children and the tooltip go with it
    HFONT fonts[] = { d->font, d->fontTitle, d->fontKpi, d->fontChevron, d->fontGlyph };
    for (int i = 0; i < 5; i++)
        if (fonts[i])
            DeleteObject(fonts[i]);
    ZeroMemory(d, sizeof(*d));
}

static void AddOp(Dlg *d, Op *o)
{
    if (d->n == MAX_OPS)
        return;  // shortcut: past 32 at once an operation runs without a panel; raise MAX_OPS if that happens
    BOOL fresh = !d->hwnd;
    if (fresh && !CreateDlgWindow(d))
        return;
    o->appear = fresh || !d->motion ? 1 : 0;  // the first panel arrives with the window itself
    o->pause = IconButton(d, L"Pausar");
    o->cancel = IconButton(d, L"Cancelar");
    d->ops[d->n++] = o;
    o->added = TRUE;
    if (fresh) {
        d->h = LayoutHeight(d);
        ApplyHeight(d);
        Layout(d);
        ShowWindow(d->hwnd, SW_SHOWNORMAL);
    }
    Tick(d);
}

static void RemoveOp(Dlg *d, Op *o)
{
    int i = 0;
    while (i < d->n && d->ops[i] != o)
        i++;
    if (i == d->n)
        return;
    SetTip(d, o->pause, L"", TTM_DELTOOLW);
    SetTip(d, o->cancel, L"", TTM_DELTOOLW);
    if (o->meterBmp)
        DeleteObject(o->meterBmp);
    DestroyWindow(o->pause);
    DestroyWindow(o->cancel);
    if (gPick && gPick->op == o)
        ClosePick(d, FALSE);
    for (int k = 0; k < CONFLICT_BTNS; k++)
        if (o->cbtn[k])
            DestroyWindow(o->cbtn[k]);
    memmove(&d->ops[i], &d->ops[i + 1], (d->n - i - 1) * sizeof(Op *));
    d->n--;
    if (!d->n)
        DestroyDlgWindow(d);
    else
        Tick(d);  // the rest slide up as the window eases to its new height
}

static LRESULT CALLBACK MgrProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_FC_ADD) {
        AddOp(&gDlg, (Op *)lp);
        return 0;
    }
    if (msg == WM_FC_REMOVE) {
        RemoveOp(&gDlg, (Op *)lp);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI UiThread(LPVOID ready)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // taskbar progress
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = gInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"FastCopyProgress";
    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);  // reuse Explorer's own icon
    wc.hIcon = ExtractIconW(gInst, exe, 0);
    RegisterClassExW(&wc);
    WNDCLASSEXW pc = wc;
    pc.lpfnWndProc = PickProc;
    pc.lpszClassName = kPickClass;
    RegisterClassExW(&pc);
    pc.lpfnWndProc = PickListProc;
    pc.lpszClassName = kListClass;
    pc.hIcon = NULL;
    RegisterClassExW(&pc);
    WNDCLASSEXW mc = { sizeof(mc) };
    mc.lpfnWndProc = MgrProc;
    mc.hInstance = gInst;
    mc.lpszClassName = L"FastCopyUiManager";
    RegisterClassExW(&mc);
    gMgr = CreateWindowExW(0, L"FastCopyUiManager", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, gInst, NULL);
    SetEvent(ready);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (gPick && IsDialogMessageW(gPick->hwnd, &m))
            continue;
        if (!gDlg.hwnd || !IsDialogMessageW(gDlg.hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}

static BOOL CALLBACK StartUi(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, UiThread, ready, 0, NULL);
    if (th) {
        WaitForSingleObject(ready, 5000);
        CloseHandle(th);  // lives as long as Explorer
    }
    CloseHandle(ready);
    return TRUE;
}

static FcConflictChoice AskConflictDialog(int count, PCWSTR example);

// One question per operation. The progress window shows it inside the operation's panel; this thread
// just publishes it and polls for the answer (the UI thread sets conflictChoice, or the user cancels).
static FcConflictChoice AskConflict(FcProgress *prog, int count, PCWSTR example)
{
    if (!prog->conflictEvent)
        return AskConflictDialog(count, example);
    wcscpy_s(prog->conflictExample, MAX_PATH, example);
    prog->conflictCount = count;
    prog->conflictChoice = -1;
    prog->conflictPending = 1;
    SetEvent(prog->conflictEvent);
    while (prog->conflictChoice < 0 && !prog->cancel)
        Sleep(30);
    LONG c = prog->conflictChoice;
    prog->conflictPending = 0;
    return c < 0 ? FC_CONFLICT_CANCEL : (FcConflictChoice)c;
}

typedef struct {
    const FcItem *items;
    int n;
    FcProgress *prog;
    FcResult res;
} Work;

// Fallback when there is no window to ask in.
static FcConflictChoice AskConflictDialog(int count, PCWSTR example)
{
    WCHAR head[256];
    if (count == 1)
        swprintf_s(head, 256, L"El destino ya tiene un archivo llamado «%s»", PathFindFileNameW(example));
    else
        swprintf_s(head, 256, L"El destino ya tiene %d archivos con el mismo nombre", count);
    TASKDIALOG_BUTTON links[] = {
        { 100, L"Reemplazar los archivos en el destino" },
        { 101, L"Omitir estos archivos" },
        { 102, L"Conservar ambos archivos\nSe añadirá un número al nombre de los que ya existían." },
    };
    TASKDIALOGCONFIG cfg = { sizeof(cfg) };
    cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    cfg.pszWindowTitle = L"Reemplazar u omitir archivos";
    cfg.pszMainInstruction = head;
    cfg.pszContent = example;
    cfg.pButtons = links;
    cfg.cButtons = 3;
    int id = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&cfg, &id, NULL, NULL)))
        return FC_CONFLICT_CANCEL;
    return id == 100 ? FC_CONFLICT_REPLACE : id == 101 ? FC_CONFLICT_SKIP : id == 102 ? FC_CONFLICT_BOTH : FC_CONFLICT_CANCEL;
}

static DWORD WINAPI Worker(LPVOID arg)
{
    Work *w = arg;
    w->res = FcRun(w->items, w->n, w->prog);
    return 0;
}

FcResult RunWithProgress(const FcItem *items, int n, FcProgress *prog)
{
    Work work = { items, n, prog, FC_ERROR };
    prog->onConflict = AskConflict;
    prog->conflictEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, Worker, &work, 0, NULL);
    if (!th) {
        FcResult r = FcRun(items, n, prog);
        if (prog->conflictEvent)
            CloseHandle(prog->conflictEvent);
        return r;
    }

    // The window appears after a short delay, or at once if the engine has a question to ask.
    HANDLE wait[2] = { th, prog->conflictEvent };
    DWORD r = WaitForMultipleObjects(prog->conflictEvent ? 2 : 1, wait, FALSE, SHOW_DELAY_MS);
    if (r != WAIT_OBJECT_0) {
        InitOnceExecuteOnce(&gUiOnce, StartUi, NULL, NULL);
        Op *o = gMgr ? calloc(1, sizeof(Op)) : NULL;  // ~1 KB of samples: off Explorer's thread stack
        if (o) {
            o->prog = prog;
            o->move = items[0].move;
            wcsncpy_s(o->destName, MAX_PATH, PathFindFileNameW(items[0].dstDir), _TRUNCATE);
            wcsncpy_s(o->dstDir, MAX_PATH, items[0].dstDir, _TRUNCATE);
            wcsncpy_s(o->srcDir, MAX_PATH, items[0].src, _TRUNCATE);
            PathRemoveFileSpecW(o->srcDir);
            wcsncpy_s(o->srcName, MAX_PATH, PathFindFileNameW(o->srcDir), _TRUNCATE);
            // Sent, not posted: sent messages jump ahead of posted ones, so a posted add could
            // arrive after this op's remove and leave the UI holding freed memory.
            SendMessageW(gMgr, WM_FC_ADD, 0, (LPARAM)o);
        }
        BOOL ui = o && o->added;
        for (BOOL asked = r == WAIT_OBJECT_0 + 1;; asked = TRUE) {
            if (asked && !ui && prog->conflictPending)  // no window to ask in: use a dialog
                prog->conflictChoice = AskConflictDialog(prog->conflictCount, prog->conflictExample);
            if (WaitForMultipleObjects(prog->conflictEvent ? 2 : 1, wait, FALSE, INFINITE) == WAIT_OBJECT_0)
                break;
        }
        if (o) {
            SendMessageW(gMgr, WM_FC_REMOVE, 0, (LPARAM)o);  // returns once the UI thread let go of it
            free(o);
        }
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    if (prog->conflictEvent)
        CloseHandle(prog->conflictEvent);
    return work.res;
}
