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
#define FOOTER_H 52
#define ICON_BTN 32
#define GRAPH_POINTS 200  // speed samples across 0..100 % of the operation
#define SS 3              // meter supersampling: GDI draws aliased, downscaling smooths the edges
#define SETTINGS_KEY L"Software\\FastCopy"

typedef struct {
    FcProgress *prog;
    BOOL move;
    WCHAR srcName[MAX_PATH], destName[MAX_PATH], srcDir[MAX_PATH], dstDir[MAX_PATH];
    HWND pause, cancel;
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
    int shown;  // panels that fit on screen; the rest are summarised in the footer
    Op *ops[MAX_OPS];
    int n;
    ITaskbarList3 *taskbar;
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
static const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

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
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe Fluent Icons");
    lf.lfHeight = ncm.lfMessageFont.lfHeight * 5 / 6;
    d->fontChevron = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(d, 16);
    d->fontGlyph = CreateFontIndirectW(&lf);
    SendMessageW(d->details, WM_SETFONT, (WPARAM)d->font, TRUE);
}

static int HeaderH(Dlg *d) { return d->n > 1 ? S(d, HEADER_H) : 0; }

static int PanelH(Dlg *d, Op *o)
{
    return (int)((S(d, PANEL_BASE_H) + (S(d, GRAPH_H - BAR_H) + S(d, 10 + ROWS_H)) * d->t) * o->appear + 0.5);
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
    d->shown = 0;
    for (int i = 0; i < d->n; i++) {
        int ph = PanelH(d, d->ops[i]);
        if (d->shown && h + ph > maxH)
            break;
        h += ph;
        d->shown++;
    }
    return h;
}

static int PanelTop(Dlg *d, int index)
{
    int y = S(d, MARGIN) + HeaderH(d);
    for (int i = 0; i < index; i++)
        y += PanelH(d, d->ops[i]);
    return y;
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
    SetWindowPos(d->details, NULL, m - S(d, 8), footerTop + S(d, 10), S(d, 150), S(d, 32), SWP_NOZORDER);
    for (int i = 0; i < d->n; i++) {
        Op *o = d->ops[i];
        int top = PanelTop(d, i), y = top + S(d, 22);
        BOOL visible = i < d->shown && y + b <= top + PanelH(d, o) && y + b <= footerTop;
        SetWindowPos(o->cancel, NULL, rc.right - m - b + S(d, 6), y, b, b, SWP_NOZORDER | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        SetWindowPos(o->pause, NULL, rc.right - m - 2 * b + S(d, 2), y, b, b, SWP_NOZORDER | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
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
    BOOL busy = FALSE, morphing = d->t != d->tTo;
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
    Layout(d);
    InvalidateRect(d->hwnd, NULL, FALSE);
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
static void DrawMeter(Dlg *d, Op *o, HDC target, RECT r)
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
    for (int i = 0; i < d->shown; i++) {
        Op *o = d->ops[i];
        int top = PanelTop(d, i), ph = PanelH(d, o);
        int saved = SaveDC(dc);
        // A panel only ever draws inside its own (unfolding) slot and above the footer.
        IntersectClipRect(dc, 0, top, rc.right, min(top + ph, footerTop));
        PaintPanel(d, o, dc, top, rc.right);
        RestoreDC(dc, saved);
        if (i > 0) {
            HGDIOBJ op = SelectObject(dc, sep);
            MoveToEx(dc, m, top - S(d, 9), NULL);
            LineTo(dc, rc.right - m, top - S(d, 9));
            SelectObject(dc, op);
        }
    }
    DeleteObject(sep);

    if (d->shown < d->n) {
        swprintf_s(line, 128, d->n - d->shown == 1 ? L"y 1 operación más" : L"y %d operaciones más", d->n - d->shown);
        RECT r = { m, footerTop, rc.right - m, rc.bottom };
        SelectObject(dc, d->font);
        SetTextColor(dc, pal->sub);
        DrawTextW(dc, line, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
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
        if (LOWORD(wp) == ID_DETAILS) {
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
    DwmSetWindowAttribute(d->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
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
    SetTimer(d->hwnd, TIMER_TICK, TICK_MS, NULL);
    return TRUE;
}

static void DestroyDlgWindow(Dlg *d)
{
    if (d->taskbar) {
        ITaskbarList3_SetProgressState(d->taskbar, d->hwnd, TBPF_NOPROGRESS);
        ITaskbarList3_Release(d->taskbar);
    }
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
    DestroyWindow(o->pause);
    DestroyWindow(o->cancel);
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
    WNDCLASSEXW mc = { sizeof(mc) };
    mc.lpfnWndProc = MgrProc;
    mc.hInstance = gInst;
    mc.lpszClassName = L"FastCopyUiManager";
    RegisterClassExW(&mc);
    gMgr = CreateWindowExW(0, L"FastCopyUiManager", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, gInst, NULL);
    SetEvent(ready);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
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
        InitOnceExecuteOnce(&gUiOnce, StartUi, NULL, NULL);
        Op *o = gMgr ? calloc(1, sizeof(Op)) : NULL;  // ~1 KB of samples: off Explorer's thread stack
        if (o) {
            o->prog = prog;
            o->move = items[0].move;
            wcscpy_s(o->destName, MAX_PATH, PathFindFileNameW(items[0].dstDir));
            wcscpy_s(o->dstDir, MAX_PATH, items[0].dstDir);
            wcscpy_s(o->srcDir, MAX_PATH, items[0].src);
            PathRemoveFileSpecW(o->srcDir);
            wcscpy_s(o->srcName, MAX_PATH, PathFindFileNameW(o->srcDir));
            // Sent, not posted: sent messages jump ahead of posted ones, so a posted add could
            // arrive after this op's remove and leave the UI holding freed memory.
            SendMessageW(gMgr, WM_FC_ADD, 0, (LPARAM)o);
        }
        WaitForSingleObject(th, INFINITE);
        if (o) {
            SendMessageW(gMgr, WM_FC_REMOVE, 0, (LPARAM)o);  // returns once the UI thread let go of it
            free(o);
        }
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    return work.res;
}
