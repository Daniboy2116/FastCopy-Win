// Progress window shown while the engine works: Windows 11 look (light/dark, accent colour,
// per-monitor DPI), taskbar progress, Pause/Resume and Cancel (Esc or closing also cancels).
// "Más detalles" morphs the progress bar into a live speed graph (the bar is the graph
// flattened: its fill spans the same progress the curve covers) and reveals the
// item/data/time/route breakdown. The choice is remembered in HKCU\Software\FastCopy\ShowDetails.
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

#define SHOW_DELAY_MS 500  // operations shorter than this never show a window
#define TIMER_TICK 1
#define TIMER_FRAME 2
#define TICK_MS 250
#define FRAME_MS 16
#define EXPAND_MS 300
#define COLLAPSE_MS 240  // exits a little faster than entrances
#define ID_PAUSE 100
#define ID_DETAILS 101
// Layout in 96-dpi pixels; everything between the two states is interpolated by the morph value.
#define WIDTH 480
#define METER_TOP 80
#define BAR_H 4
#define GRAPH_H 120
#define ROWS_H 104
#define BUTTONS_H 52
#define COLLAPSED_H 168
#define EXPANDED_H (METER_TOP + GRAPH_H + 10 + 20 + 10 + ROWS_H + 16 + BUTTONS_H)
#define GRAPH_POINTS 200  // speed samples across 0..100 % of the operation
#define SS 3              // meter supersampling: GDI draws aliased, downscaling smooths the edges
#define SETTINGS_KEY L"Software\\FastCopy"

typedef struct {
    FcProgress *prog;
    BOOL move;
    WCHAR destName[MAX_PATH], srcDir[MAX_PATH], dstDir[MAX_PATH];
    HWND hwnd, pause, cancel, details, hover;
    HFONT font, fontTitle, fontKpi, fontIcon;
    UINT dpi;
    BOOL dark, expanded, motion;
    COLORREF accent;
    // Morph: t = 0 bar, 1 graph. Animated from its current value, so a toggle mid-way reverses smoothly.
    double t, tFrom, tTo;
    ULONGLONG animStart, animMs;
    double shownFrac;  // progress drawn, eased towards the real value between engine ticks
    ULONGLONG lastTick, activeMs;
    LONG64 lastBytes;
    double speed, peak;  // bytes/s; speed is smoothed
    float graph[GRAPH_POINTS];
    int graphN;
    ITaskbarList3 *taskbar;
} Ui;

typedef struct {
    COLORREF bg, text, sub, track, btn, btnHover, btnPressed, btnBorder, card, cardBorder;
} Palette;

static const Palette kLight = { RGB(243, 243, 243), RGB(26, 26, 26), RGB(96, 96, 96), RGB(220, 220, 220),
                                RGB(251, 251, 251), RGB(246, 246, 246), RGB(235, 235, 235), RGB(210, 210, 210),
                                RGB(251, 251, 251), RGB(229, 229, 229) };
static const Palette kDark = { RGB(32, 32, 32), RGB(255, 255, 255), RGB(200, 200, 200), RGB(70, 70, 70),
                               RGB(55, 55, 55), RGB(62, 62, 62), RGB(45, 45, 45), RGB(75, 75, 75),
                               RGB(43, 43, 43), RGB(29, 29, 29) };
static const COLORREF kPausedColor = RGB(157, 93, 0);

static const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

static int S(Ui *ui, int px) { return MulDiv(px, ui->dpi, 96); }
static int Lerp(int a, int b, double t) { return a + (int)((b - a) * t + (b > a ? 0.5 : -0.5)); }
static double Clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

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

static double RealFrac(Ui *ui)
{
    LONG64 tot = ui->prog->bytesTotal;
    return tot > 0 ? Clamp01((double)ui->prog->bytesDone / tot) : 0;
}

static void LoadFonts(Ui *ui)
{
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, ui->dpi);
    HFONT *fonts[] = { &ui->font, &ui->fontTitle, &ui->fontKpi, &ui->fontIcon };
    for (int i = 0; i < 4; i++)
        if (*fonts[i])
            DeleteObject(*fonts[i]);
    ui->font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = ncm.lfMessageFont.lfHeight * 4 / 3;
    lf.lfWeight = FW_SEMIBOLD;
    ui->fontTitle = CreateFontIndirectW(&lf);
    lf.lfHeight = ncm.lfMessageFont.lfHeight * 5 / 3;
    ui->fontKpi = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    lf.lfHeight = ncm.lfMessageFont.lfHeight * 5 / 6;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe Fluent Icons");
    ui->fontIcon = CreateFontIndirectW(&lf);
    SendMessageW(ui->pause, WM_SETFONT, (WPARAM)ui->font, TRUE);
    SendMessageW(ui->cancel, WM_SETFONT, (WPARAM)ui->font, TRUE);
    SendMessageW(ui->details, WM_SETFONT, (WPARAM)ui->font, TRUE);
}

// Buttons ride the bottom edge, so they follow the window while it grows.
static void Layout(Ui *ui)
{
    int bw = S(ui, 96), bh = S(ui, 32), m = S(ui, 20);
    RECT rc;
    GetClientRect(ui->hwnd, &rc);
    int y = rc.bottom - m - bh;
    SetWindowPos(ui->cancel, NULL, rc.right - m - bw, y, bw, bh, SWP_NOZORDER);
    SetWindowPos(ui->pause, NULL, rc.right - m - 2 * bw - S(ui, 8), y, bw, bh, SWP_NOZORDER);
    SetWindowPos(ui->details, NULL, m - S(ui, 8), y, S(ui, 150), bh, SWP_NOZORDER);
}

static void ApplyHeight(Ui *ui)
{
    RECT wr = { 0, 0, S(ui, WIDTH), Lerp(S(ui, COLLAPSED_H), S(ui, EXPANDED_H), ui->t) };
    AdjustWindowRectExForDpi(&wr, kStyle, FALSE, WS_EX_CONTROLPARENT, ui->dpi);
    SetWindowPos(ui->hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void StartFrames(Ui *ui) { SetTimer(ui->hwnd, TIMER_FRAME, FRAME_MS, NULL); }

static void ToggleDetails(Ui *ui)
{
    ui->expanded = !ui->expanded;
    DWORD v = ui->expanded;
    RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"ShowDetails", REG_DWORD, &v, sizeof(v));
    SetWindowTextW(ui->details, ui->expanded ? L"Menos detalles" : L"Más detalles");
    ui->tFrom = ui->t;
    ui->tTo = ui->expanded ? 1 : 0;
    if (!ui->motion) {
        ui->t = ui->tTo;
        ApplyHeight(ui);
        InvalidateRect(ui->hwnd, NULL, FALSE);
        return;
    }
    // A reversal mid-way only travels what's left, in the same proportion of time.
    ui->animMs = (ULONGLONG)((ui->expanded ? EXPAND_MS : COLLAPSE_MS) * fabs(ui->tTo - ui->tFrom));
    ui->animStart = GetTickCount64();
    StartFrames(ui);
}

// One animation frame: advances the morph and eases the drawn progress. FALSE once settled.
static BOOL Frame(Ui *ui)
{
    BOOL busy = FALSE;
    if (ui->t != ui->tTo) {
        double p = ui->animMs ? (double)(GetTickCount64() - ui->animStart) / ui->animMs : 1;
        ui->t = p >= 1 ? ui->tTo : ui->tFrom + (ui->tTo - ui->tFrom) * EaseOut(p);
        ApplyHeight(ui);
        busy = ui->t != ui->tTo;
    }
    double real = RealFrac(ui), diff = real - ui->shownFrac;
    if (fabs(diff) > 0.0005) {
        ui->shownFrac += diff * 0.2;  // ~75 ms time constant: hides the 250 ms engine steps
        busy = TRUE;
    } else {
        ui->shownFrac = real;
    }
    InvalidateRect(ui->hwnd, NULL, FALSE);
    return busy;
}

static RECT MeterRect(Ui *ui, int width)
{
    RECT r = { S(ui, 20), S(ui, METER_TOP), width - S(ui, 20), S(ui, METER_TOP) + Lerp(S(ui, BAR_H), S(ui, GRAPH_H), ui->t) };
    return r;
}

// The bar and the graph are one shape. At t = 0 every sample's area reaches the top, so the
// accent fill up to shownFrac is exactly the progress bar; as t grows the shape deepens into
// the speed curve, the track becomes the graph background and the fill turns into a gradient.
static void DrawMeter(Ui *ui, HDC target, RECT r, const Palette *pal)
{
    double t = ui->t;
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

    int radius = Lerp(h, S(ui, 8) * SS, t);  // pill bar -> rounded card corners
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

    COLORREF acc = ui->prog->paused ? kPausedColor : ui->accent;
    int fillW = (int)(ui->shownFrac * w);
    // Headroom keeps the curve under the speed figure drawn over the top-left corner.
    double maxY = ui->peak * 1.45 > 1 ? ui->peak * 1.45 : 1;
    POINT pts[GRAPH_POINTS + 2];
    int n = ui->graphN >= 2 ? ui->graphN : 0;
    for (int k = 0; k < n; k++) {
        int curveY = h - SS - (int)(ui->graph[k] / maxY * (h - 4 * SS));
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
        double avg = ui->activeMs ? ui->prog->bytesDone * 1000.0 / ui->activeMs : 0;
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
        int idx = (int)(ui->shownFrac * (GRAPH_POINTS - 1));
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
        FcProgress *pr = ui->prog;
        WCHAR a[64], b2[64], line[256];
        RECT tr = { r.left + S(ui, 12), r.top + S(ui, 8), r.right - S(ui, 12), r.top + S(ui, 36) };
        SelectObject(target, ui->fontKpi);
        SetTextColor(target, Mix(pal->text, meterBg, kk));
        if (pr->paused)
            wcscpy_s(line, 256, L"En pausa");
        else if (pr->phase == FC_PHASE_PLAN || ui->speed <= 1)
            wcscpy_s(line, 256, L"—");
        else {
            StrFormatByteSizeW((LONGLONG)ui->speed, a, 64);
            swprintf_s(line, 256, L"%s/s", a);
        }
        DrawTextW(target, line, -1, &tr, DT_SINGLELINE | DT_TOP | DT_NOPREFIX);
        double avg = ui->activeMs ? pr->bytesDone * 1000.0 / ui->activeMs : 0;
        if (avg > 1) {
            StrFormatByteSizeW((LONGLONG)avg, a, 64);
            StrFormatByteSizeW((LONGLONG)ui->peak, b2, 64);
            swprintf_s(line, 256, L"media %s/s  ·  máxima %s/s", a, b2);
            SelectObject(target, ui->font);
            SetTextColor(target, Mix(pal->sub, meterBg, kk));
            tr.top += S(ui, 6);
            DrawTextW(target, line, -1, &tr, DT_SINGLELINE | DT_TOP | DT_RIGHT | DT_NOPREFIX);
        }
    }
}

static void DrawRow(Ui *ui, HDC dc, const Palette *pal, COLORREF bg, double a, int x, int y, int right,
                    PCWSTR label, PCWSTR value, UINT ellipsis)
{
    if (a <= 0)
        return;
    y += (int)((1 - a) * S(ui, 6));  // rises into place while it fades in
    RECT r = { x, y, x + S(ui, 92), y + S(ui, 22) };
    SetTextColor(dc, Mix(pal->sub, bg, a));
    DrawTextW(dc, label, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    r.left = r.right;
    r.right = right;
    SetTextColor(dc, Mix(pal->text, bg, a));
    DrawTextW(dc, value, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | ellipsis);
}

static void PaintRows(Ui *ui, HDC dc, const Palette *pal, RECT card)
{
    double cardIn = Clamp01((ui->t - 0.45) / 0.35);
    if (cardIn <= 0)
        return;
    COLORREF cardBg = Mix(pal->card, pal->bg, cardIn);
    HBRUSH fill = CreateSolidBrush(cardBg);
    HPEN border = CreatePen(PS_SOLID, 1, Mix(pal->cardBorder, pal->bg, cardIn));
    HGDIOBJ ob = SelectObject(dc, fill), op = SelectObject(dc, border);
    RoundRect(dc, card.left, card.top, card.right, card.bottom, S(ui, 8), S(ui, 8));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fill);
    DeleteObject(border);

    FcProgress *pr = ui->prog;
    WCHAR a[64], b[64], c[64], line[512];
    int x = card.left + S(ui, 12), right = card.right - S(ui, 12), y = card.top + S(ui, 8);
    LONG64 tot = pr->bytesTotal, dn = pr->bytesDone;
    SelectObject(dc, ui->font);
    // Rows cascade in, 70 ms of morph apart.
#define ROW_IN(i) Clamp01((ui->t - 0.55 - (i) * 0.07) / 0.3)

    if (pr->phase == FC_PHASE_PLAN) {
        wcscpy_s(line, 512, L"Calculando…");
    } else {
        FormatInt(pr->filesDone, a, 64);
        FormatInt(pr->filesTotal, b, 64);
        FormatInt(pr->filesTotal - pr->filesDone, c, 64);
        swprintf_s(line, 512, L"%s de %s  ·  quedan %s", a, b, c);
    }
    DrawRow(ui, dc, pal, cardBg, ROW_IN(0), x, y, right, ui->move ? L"Movidos" : L"Copiados", line, DT_END_ELLIPSIS);

    y += S(ui, 22);
    StrFormatByteSizeW(dn, a, 64);
    StrFormatByteSizeW(tot, b, 64);
    StrFormatByteSizeW(tot > dn ? tot - dn : 0, c, 64);
    swprintf_s(line, 512, L"%s de %s  ·  quedan %s", a, b, c);
    // Moves read every byte twice (copy, then verify), and the totals say so.
    DrawRow(ui, dc, pal, cardBg, ROW_IN(1), x, y, right, ui->move ? L"Copia + verif." : L"Datos", line, DT_END_ELLIPSIS);

    y += S(ui, 22);
    FormatClock(ui->activeMs / 1000.0, a, 64);
    if (ui->speed > 1 && !pr->paused) {
        FormatClock((tot - dn) / ui->speed, b, 64);
        swprintf_s(line, 512, L"%s transcurrido  ·  quedan ~%s", a, b);
    } else {
        swprintf_s(line, 512, L"%s transcurrido", a);
    }
    DrawRow(ui, dc, pal, cardBg, ROW_IN(2), x, y, right, L"Tiempo", line, DT_END_ELLIPSIS);

    y += S(ui, 22);
    swprintf_s(line, 512, L"%s  →  %s", ui->srcDir, ui->dstDir);
    DrawRow(ui, dc, pal, cardBg, ROW_IN(3), x, y, right, L"Ruta", line, DT_PATH_ELLIPSIS);
#undef ROW_IN
}

static void Paint(Ui *ui, HDC target)
{
    const Palette *pal = ui->dark ? &kDark : &kLight;
    FcProgress *pr = ui->prog;
    RECT rc;
    GetClientRect(ui->hwnd, &rc);
    // Double buffered to avoid flicker on every frame.
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
    if (pr->phase == FC_PHASE_PLAN)
        swprintf_s(line, 512, L"Preparando: %s a %s", ui->move ? L"mover" : L"copiar", ui->destName);
    else
        swprintf_s(line, 512, L"%s %ld %s a %s", ui->move ? L"Moviendo" : L"Copiando", files,
                   files == 1 ? L"archivo" : L"archivos", ui->destName);
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

    RECT meter = MeterRect(ui, rc.right);
    DrawMeter(ui, dc, meter, pal);

    LONG64 tot = pr->bytesTotal, dn = pr->bytesDone;
    if (pr->cancel) {
        wcscpy_s(line, 512, L"Cancelando...");
    } else if (pr->phase == FC_PHASE_PLAN) {
        wcscpy_s(line, 512, L"Calculando...");
    } else {
        StrFormatByteSizeW(dn, done, 32);
        StrFormatByteSizeW(tot, total, 32);
        int n = swprintf_s(line, 512, L"%d %% · %s de %s", (int)(RealFrac(ui) * 100), done, total);
        if (pr->paused) {
            wcscat_s(line, 512, L" · En pausa");
        } else if (ui->speed > 1) {
            WCHAR speed[32], eta[48];
            StrFormatByteSizeW((LONGLONG)ui->speed, speed, 32);
            FormatEta((tot - dn) / ui->speed, eta, 48);
            swprintf_s(line + n, 512 - n, L" · %s/s · %s", speed, eta);
        }
    }
    SelectObject(dc, ui->font);
    SetTextColor(dc, pal->sub);
    r.top = meter.bottom + S(ui, 10);  // the stats line rides below the meter as it grows
    r.bottom = r.top + S(ui, 20);
    DrawTextW(dc, line, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (ui->t > 0) {
        RECT card = { m, r.bottom + S(ui, 10), rc.right - m, r.bottom + S(ui, 10 + ROWS_H) };
        int saved = SaveDC(dc);
        IntersectClipRect(dc, 0, 0, rc.right, rc.bottom - S(ui, BUTTONS_H));  // never under the buttons
        PaintRows(ui, dc, pal, card);
        RestoreDC(dc, saved);
    }

    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static void FillRound(Ui *ui, HDC dc, RECT rc, COLORREF face, COLORREF edge, int edgeW)
{
    HBRUSH b = CreateSolidBrush(face);
    HPEN p = edgeW ? CreatePen(PS_SOLID, edgeW, edge) : NULL;
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p ? (HGDIOBJ)p : GetStockObject(NULL_PEN));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, S(ui, 8), S(ui, 8));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    if (p)
        DeleteObject(p);
}

static void DrawButton(Ui *ui, const DRAWITEMSTRUCT *di)
{
    const Palette *pal = ui->dark ? &kDark : &kLight;
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    HBRUSH bg = CreateSolidBrush(pal->bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    BOOL disabled = di->itemState & ODS_DISABLED, focus = di->itemState & ODS_FOCUS;
    BOOL pressed = di->itemState & ODS_SELECTED, hovered = !disabled && di->hwndItem == ui->hover;
    WCHAR text[32];
    GetWindowTextW(di->hwndItem, text, 32);

    if (di->CtlID == ID_DETAILS) {
        // Text button: no surface at rest, a soft one on hover/press, chevron + label.
        if (pressed || hovered)
            FillRound(ui, dc, rc, pressed ? pal->btnPressed : pal->btnHover, 0, 0);
        if (focus) {
            HPEN ring = CreatePen(PS_SOLID, S(ui, 2), ui->accent);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH)), op = SelectObject(dc, ring);
            RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, S(ui, 8), S(ui, 8));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(ring);
        }
        RECT ir = { rc.left + S(ui, 8), rc.top, rc.left + S(ui, 28), rc.bottom };
        SetTextColor(dc, pal->text);
        SelectObject(dc, ui->fontIcon);
        DrawTextW(dc, ui->expanded ? L"\xE70E" : L"\xE70D", -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT tr = { ir.right + S(ui, 4), rc.top, rc.right, rc.bottom };
        SelectObject(dc, ui->font);
        DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    FillRound(ui, dc, rc, pressed ? pal->btnPressed : hovered ? pal->btnHover : pal->btn,
              focus ? ui->accent : pal->btnBorder, focus ? S(ui, 2) : 1);
    SetTextColor(dc, disabled ? pal->sub : pressed ? Mix(pal->text, pal->btnPressed, 0.8) : pal->text);
    SelectObject(dc, ui->font);
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// Owner-drawn buttons get no hover state from Windows: track it here.
static LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    Ui *ui = (Ui *)ref;
    (void)id;
    if (msg == WM_MOUSEMOVE && ui->hover != hwnd) {
        HWND prev = ui->hover;
        ui->hover = hwnd;
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(hwnd, NULL, FALSE);
        if (prev)
            InvalidateRect(prev, NULL, FALSE);
    } else if (msg == WM_MOUSELEAVE && ui->hover == hwnd) {
        ui->hover = NULL;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void Tick(Ui *ui)
{
    FcProgress *pr = ui->prog;
    ULONGLONG now = GetTickCount64();
    LONG64 bytes = pr->bytesDone;
    BOOL running = pr->phase == FC_PHASE_COPY && !pr->paused;
    if (ui->lastTick && now > ui->lastTick && running) {
        double inst = (bytes - ui->lastBytes) * 1000.0 / (now - ui->lastTick);
        ui->speed = ui->speed ? ui->speed * 0.7 + inst * 0.3 : inst;
        ui->activeMs += now - ui->lastTick;
        if (ui->speed > ui->peak)
            ui->peak = ui->speed;
        // One sample per slice of progress; a fast tick fills the slices it skipped.
        if (pr->bytesTotal > 0) {
            int idx = (int)((double)bytes / pr->bytesTotal * (GRAPH_POINTS - 1));
            if (idx >= GRAPH_POINTS)
                idx = GRAPH_POINTS - 1;
            for (int k = ui->graphN; k <= idx; k++)
                ui->graph[k] = (float)ui->speed;
            if (idx + 1 > ui->graphN)
                ui->graphN = idx + 1;
        }
    }
    ui->lastTick = now;
    ui->lastBytes = bytes;

    if (ui->taskbar) {
        if (pr->phase == FC_PHASE_PLAN) {
            ITaskbarList3_SetProgressState(ui->taskbar, ui->hwnd, TBPF_INDETERMINATE);
        } else {
            ITaskbarList3_SetProgressState(ui->taskbar, ui->hwnd, pr->paused ? TBPF_PAUSED : TBPF_NORMAL);
            ITaskbarList3_SetProgressValue(ui->taskbar, ui->hwnd, bytes, max(pr->bytesTotal, 1));
        }
    }
    if (!ui->motion)
        ui->shownFrac = RealFrac(ui);
    else if (fabs(RealFrac(ui) - ui->shownFrac) > 0.0005)
        StartFrames(ui);
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
    case WM_SIZE:
        if (ui && ui->cancel) {
            Layout(ui);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_DRAWITEM:
        DrawButton(ui, (DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_TIMER:
        if (wp == TIMER_FRAME) {
            if (!Frame(ui))
                KillTimer(hwnd, TIMER_FRAME);
        } else {
            Tick(ui);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) {
            Cancel(ui);
        } else if (LOWORD(wp) == ID_PAUSE) {
            ui->prog->paused = !ui->prog->paused;
            SetWindowTextW(ui->pause, ui->prog->paused ? L"Reanudar" : L"Pausar");
            InvalidateRect(ui->pause, NULL, FALSE);
            Tick(ui);
        } else if (LOWORD(wp) == ID_DETAILS) {
            ToggleDetails(ui);
            InvalidateRect(ui->details, NULL, FALSE);
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
        ApplyHeight(ui);
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
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"FastCopyProgress";
        WCHAR exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);  // reuse Explorer's own icon
        wc.hIcon = ExtractIconW(gInst, exe, 0);
        cls = RegisterClassExW(&wc);
    }
    ui->dark = RegDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", 1) == 0;
    // DWM stores the accent as 0xAABBGGRR, which is a COLORREF once alpha is dropped.
    ui->accent = RegDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFC06700) & 0xFFFFFF;
    ui->expanded = RegDword(SETTINGS_KEY, L"ShowDetails", 0) != 0;
    ui->t = ui->tFrom = ui->tTo = ui->expanded ? 1 : 0;
    ui->motion = TRUE;  // "Animation effects" off in Windows settings => every change is instant
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &ui->motion, 0);

    // Centre over the Explorer window the user is working in.
    RECT anchor;
    HWND fg = GetForegroundWindow();
    if (!fg || !GetWindowRect(fg, &anchor))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
    UINT dpi = fg ? GetDpiForWindow(fg) : GetDpiForSystem();
    ui->dpi = dpi ? dpi : 96;
    RECT wr = { 0, 0, S(ui, WIDTH), Lerp(S(ui, COLLAPSED_H), S(ui, EXPANDED_H), ui->t) };
    AdjustWindowRectExForDpi(&wr, kStyle, FALSE, WS_EX_CONTROLPARENT, ui->dpi);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    int x = (anchor.left + anchor.right - w) / 2, y = (anchor.top + anchor.bottom - h) / 2;

    ui->hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"FastCopyProgress", ui->move ? L"Moviendo" : L"Copiando",
                               kStyle, x, y, w, h, NULL, NULL, gInst, ui);
    if (!ui->hwnd)
        return NULL;
    BOOL dark = ui->dark;
    DwmSetWindowAttribute(ui->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    ui->details = CreateWindowExW(0, L"BUTTON", ui->expanded ? L"Menos detalles" : L"Más detalles",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, ui->hwnd,
                                  (HMENU)ID_DETAILS, gInst, NULL);
    ui->pause = CreateWindowExW(0, L"BUTTON", L"Pausar", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                0, 0, 0, 0, ui->hwnd, (HMENU)ID_PAUSE, gInst, NULL);
    ui->cancel = CreateWindowExW(0, L"BUTTON", L"Cancelar", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 0, 0, 0, 0, ui->hwnd, (HMENU)IDCANCEL, gInst, NULL);
    HWND buttons[] = { ui->details, ui->pause, ui->cancel };
    for (int i = 0; i < 3; i++)
        SetWindowSubclass(buttons[i], ButtonProc, 1, (DWORD_PTR)ui);
    LoadFonts(ui);
    Layout(ui);
    CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&ui->taskbar);
    if (ui->taskbar && FAILED(ITaskbarList3_HrInit(ui->taskbar))) {
        ITaskbarList3_Release(ui->taskbar);
        ui->taskbar = NULL;
    }
    ShowWindow(ui->hwnd, SW_SHOWNORMAL);
    SetTimer(ui->hwnd, TIMER_TICK, TICK_MS, NULL);
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
        Ui *ui = calloc(1, sizeof(Ui));  // ~1 KB of graph samples: keep it off Explorer's thread stack
        ui->prog = prog;
        ui->move = items[0].move;
        wcscpy_s(ui->destName, MAX_PATH, PathFindFileNameW(items[0].dstDir));
        wcscpy_s(ui->dstDir, MAX_PATH, items[0].dstDir);
        wcscpy_s(ui->srcDir, MAX_PATH, items[0].src);
        PathRemoveFileSpecW(ui->srcDir);
        HWND hwnd = CreateProgressWindow(ui);
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
        if (ui->taskbar) {
            ITaskbarList3_SetProgressState(ui->taskbar, hwnd, TBPF_NOPROGRESS);
            ITaskbarList3_Release(ui->taskbar);
        }
        if (hwnd)
            DestroyWindow(hwnd);
        HFONT fonts[] = { ui->font, ui->fontTitle, ui->fontKpi, ui->fontIcon };
        for (int i = 0; i < 4; i++)
            if (fonts[i])
                DeleteObject(fonts[i]);
        free(ui);
        if (quit)
            PostQuitMessage((int)quitCode);  // not ours to swallow
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    return work.res;
}
