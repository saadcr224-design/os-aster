/*
 * VISION for Windows - plan who uses which screen, keyboard and mouse.
 *
 * This program lets you divide the screens, keyboards and mice of one PC
 * between several "workplaces" and saves that plan.  It does NOT yet run the
 * extra workplaces: that needs kernel drivers (see ../DESIGN.md).
 *
 * Build: see build.sh (MinGW-w64) in this folder.
 */

#include "common.h"
#include "resource.h"
#include "update.h"
#include "version.h"

#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dbt.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int run_controller(HINSTANCE inst, BOOL test, BOOL automatic);
int run_agent(HINSTANCE inst, BOOL test);
int run_keep_apart(HINSTANCE inst, BOOL test, BOOL automatic);

enum { ID_ADD = 101, ID_REMOVE, /* not 1 or 2: Enter and Esc send IDOK (1) and IDCANCEL (2) */ ID_FIND, ID_REFRESH, ID_SAVE, ID_START, ID_SHARING, ID_AUTO, ID_SETUPS, ID_UPDATE, ID_AUTOUPDATE, ID_MENU_SEAT = 1000, ID_MENU_NONE = 1099 };
enum { HIT_NONE, HIT_SCREEN, HIT_DEVICE, HIT_SEAT, HIT_IDLE, HIT_SEAT_START, HIT_UPDATE };

static const COLORREF SEAT_COLORS[MAX_SEATS] = {
    RGB(37, 99, 205), RGB(214, 110, 20), RGB(34, 139, 84),  RGB(140, 72, 190), RGB(200, 50, 80),
    RGB(0, 140, 150), RGB(150, 110, 20), RGB(90, 90, 200),  RGB(190, 60, 160), RGB(80, 120, 60),
};
#define COL_BG      RGB(243, 244, 246)
#define COL_PANEL   RGB(255, 255, 255)
#define COL_LINE    RGB(214, 217, 222)
#define COL_TEXT    RGB(28, 32, 38)
#define COL_MUTED   RGB(98, 105, 115)
#define COL_OK      RGB(30, 130, 76)
#define COL_WARN    RGB(190, 100, 0)
#define COL_NOTE_BG RGB(255, 244, 214)
#define COL_NOTE_TX RGB(110, 72, 0)
#define COL_FLASH   RGB(255, 236, 150)
#define COL_UNUSED  RGB(140, 146, 155)

static BOOL g_dirty, g_finding;
static int g_dpi = 96;
static HWND g_wnd, g_btnAdd, g_btnRemove, g_btnFind, g_btnRefresh, g_btnSave, g_btnStart, g_btnSharing, g_btnAuto, g_btnSetups, g_btnUpdate, g_chkAutoUpdate;
/* Starting workplaces one by one. */
static RECT g_startPill[MAX_SEATS];
static DWORD g_startAt[MAX_SEATS];          /* when Start was pressed, to tell "Starting" from "Error" */
static WCHAR g_startError[MAX_SEATS][200];  /* why the last Start was refused */
static BOOL g_savedOk;
/* Updates: none, available, downloading, installing, failed */
enum { UP_NONE, UP_FOUND, UP_DOWNLOADING, UP_INSTALLING, UP_FAILED };
static int g_update, g_updatePct;
static BOOL g_updateBlink, g_checkingByHand;
static int g_updateCountdown; /* seconds until an automatic update; 0 = none */
static void update_button(void);
#define WM_FIRST_RUN (WM_APP + 60)
static WCHAR g_updateWhy[200];
static RECT g_rcNote;                      /* show the green "Setup saved successfully" message */
/* Automatic setup: which workplace is being asked for which device. */
static int g_wizSeat = -1, g_wizKind;
static HWND g_wizPrompt;
static BOOL g_wizTaken[MAX_DEVICES];
static DWORD g_wizEscAt; /* when Esc last skipped a step, so one press is not counted twice */
static void wizard_input(const RAWINPUT *in);
static void wizard_skip(void);
static HFONT g_fTitle, g_fHead, g_fText, g_fBold, g_fSmall, g_fHuge;
static WCHAR g_status[300] = L"";
static int g_hoverKind, g_hoverIndex, g_menuKind, g_menuIndex, g_scroll, g_screenScroll;
static RECT g_rcScreens, g_rcDevices, g_rcSeats, g_rcBar;
static RECT g_seatDraw[MAX_SEATS];
static RECT g_screenRow[MAX_SCREENS], g_idleRow[MAX_IDLE]; /* rows of the screen list */

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static void share_summary(int seat, WCHAR *out, size_t chars);
static void refresh_hardware(BOOL automatic);

/* ---- what each workplace has -------------------------------------------- */

static BOOL save_all(void)
{
    if (!save_setup()) return FALSE;
    g_dirty = FALSE;
    return TRUE;
}

static void seat_label(int seat, WCHAR *out, size_t chars) { seat_name(seat, out, chars); }

static COLORREF seat_color(int seat) { return seat == NOT_USED ? COL_UNUSED : SEAT_COLORS[seat]; }

static COLORREF tint(COLORREF c, int percentWhite)
{
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    return RGB(r + (255 - r) * percentWhite / 100, g + (255 - g) * percentWhite / 100, b + (255 - b) * percentWhite / 100);
}

static void set_status(const WCHAR *text)
{
    g_savedOk = FALSE;
    lstrcpynW(g_status, text, 300);
    InvalidateRect(g_wnd, &g_rcBar, FALSE);
}

static void update_buttons(void)
{
    EnableWindow(g_btnAdd, g_seatCount < MAX_SEATS);
    EnableWindow(g_btnRemove, g_seatCount > 1);
    EnableWindow(g_btnSave, TRUE); /* also to save the same layout again under another name */
    EnableWindow(g_btnStart, !g_demo && g_seatCount > 1);
    SetWindowTextW(g_btnFind, g_finding ? L"Stop finding" : L"Find a device");
}

static void changed(void)
{
    g_dirty = TRUE;
    update_buttons();
    InvalidateRect(g_wnd, NULL, FALSE);
}

/* ---- state of each workplace ----------------------------------------------- */

static BOOL engine_running(void) { return FindWindowW(L"VISIONKeepApart", NULL) != NULL; }

/* Not started / Starting / Running / Error, with the reason for an error. */
static int seat_state(int seat, WCHAR *why, size_t chars)
{
    why[0] = 0;
    if (!want_running(seat)) {
        if (g_startError[seat][0]) { lstrcpynW(why, g_startError[seat], (int)chars); return WS_ERROR; }
        return WS_NOT_STARTED;
    }
    WCHAR detail[200];
    int reported = read_status(seat, detail, 200);
    if (engine_running() && reported == WS_RUNNING) return WS_RUNNING;
    if (engine_running() && reported == WS_ERROR) { lstrcpynW(why, detail, (int)chars); return WS_ERROR; }
    if (GetTickCount() - g_startAt[seat] < 8000) return WS_STARTING;
    lstrcpynW(why, L"VISION could not start it. Press Start to try again.", (int)chars);
    return WS_ERROR;
}

/* ---- layout -------------------------------------------------------------- */

#define ROW_H 46
#define SCREEN_ROW_H 34

#define TILE_W 172
#define TILE_H 112
#define TILE_GAP 10

/* The device portal shows every keyboard and mouse as a tile. */
static int tile_columns(void)
{
    int w = g_rcDevices.right - g_rcDevices.left - S(TILE_GAP) * 2;
    int cols = (w + S(TILE_GAP)) / (S(TILE_W) + S(TILE_GAP));
    return cols < 1 ? 1 : cols;
}

static int device_rows_height(void)
{
    int cols = tile_columns(), rows = (g_deviceCount + cols - 1) / cols;
    return rows * (S(TILE_H) + S(TILE_GAP)) + S(TILE_GAP);
}

/* Scroll so a device's tile is visible. */
static void show_device(int i)
{
    int cols = tile_columns(), viewH = g_rcDevices.bottom - g_rcDevices.top - S(62);
    int top = (i / cols) * (S(TILE_H) + S(TILE_GAP));
    if (top < g_scroll) g_scroll = top;
    if (top + S(TILE_H) + S(TILE_GAP) * 2 > g_scroll + viewH) g_scroll = top + S(TILE_H) + S(TILE_GAP) * 2 - viewH;
    if (g_scroll < 0) g_scroll = 0;
}

static void layout(void)
{
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int margin = S(20), top = S(128), barH = S(60), sideW = S(310);
    int bottom = rc.bottom - barH - S(14);
    int leftW = rc.right - sideW - margin * 3;
    int screensH = (bottom - top) * 37 / 100;
    int listH = S(56) + S(16) + (g_screenCount + g_idleCount) * S(SCREEN_ROW_H); /* room for every row of the screen list */
    if (listH > screensH) screensH = listH;
    if (screensH > (bottom - top) * 60 / 100) screensH = (bottom - top) * 60 / 100;

    SetRect(&g_rcBar, 0, rc.bottom - barH, rc.right, rc.bottom);
    SetRect(&g_rcScreens, margin, top, margin + leftW, top + screensH);
    SetRect(&g_rcDevices, margin, top + screensH + S(14), margin + leftW, bottom);
    SetRect(&g_rcSeats, rc.right - margin - sideW, top, rc.right - margin, bottom);

    int by = rc.bottom - barH + S(14), bh = S(32);
    MoveWindow(g_btnAuto, rc.right - margin - S(200), S(20), S(200), S(38), TRUE);
    MoveWindow(g_btnUpdate, rc.right - margin - S(200) - S(10) - S(190), S(8), S(190), S(36), TRUE);
    MoveWindow(g_chkAutoUpdate, rc.right - margin - S(200) - S(10) - S(190), S(48), S(190), S(22), TRUE);
    MoveWindow(g_btnSetups, rc.right - margin - S(132), top - S(2), S(132), S(28), TRUE);
    MoveWindow(g_btnFind, margin, by, S(130), bh, TRUE);
    MoveWindow(g_btnRefresh, margin + S(140), by, S(90), bh, TRUE);
    MoveWindow(g_btnSharing, margin + S(240), by, S(190), bh, TRUE);
    MoveWindow(g_btnStart, rc.right - margin - S(170), by, S(170), bh, TRUE);
    MoveWindow(g_btnSave, rc.right - margin - S(170) - S(130), by, S(120), bh, TRUE);
    int sy = g_rcSeats.bottom - bh;
    MoveWindow(g_btnAdd, g_rcSeats.left, sy, S(160), bh, TRUE);
    MoveWindow(g_btnRemove, g_rcSeats.left + S(168), sy, sideW - S(168), bh, TRUE);

    int maxScroll = device_rows_height() - (g_rcDevices.bottom - g_rcDevices.top - S(62));
    if (maxScroll < 0) maxScroll = 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;
    if (g_scroll < 0) g_scroll = 0;
}

/* ---- painting ------------------------------------------------------------ */

static void fill(HDC dc, const RECT *rc, COLORREF color)
{
    SetBkColor(dc, color);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, rc, NULL, 0, NULL);
}

static void text(HDC dc, HFONT font, COLORREF color, RECT rc, UINT flags, const WCHAR *str)
{
    HFONT old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, str, -1, &rc, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void box(HDC dc, RECT rc, int radius, COLORREF inside, COLORREF border, int width)
{
    HPEN pen = CreatePen(PS_INSIDEFRAME, width, border);
    HBRUSH brush = CreateSolidBrush(inside);
    HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, brush);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

/* A round badge with a tick (yes) or a dash (no). */
static void badge(HDC dc, int x, int y, int size, BOOL yes)
{
    COLORREF color = yes ? COL_OK : COL_WARN;
    HBRUSH brush = CreateSolidBrush(color);
    HPEN none = GetStockObject(NULL_PEN), white = CreatePen(PS_SOLID, S(2), RGB(255, 255, 255));
    HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, none);
    Ellipse(dc, x, y, x + size + 1, y + size + 1);
    SelectObject(dc, white);
    if (yes) {
        POINT tick[3] = { { x + size * 27 / 100, y + size * 52 / 100 }, { x + size * 44 / 100, y + size * 68 / 100 },
                          { x + size * 73 / 100, y + size * 34 / 100 } };
        Polyline(dc, tick, 3);
    } else {
        MoveToEx(dc, x + size * 30 / 100, y + size / 2, NULL);
        LineTo(dc, x + size * 72 / 100, y + size / 2);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(white);
}

static void section_title(HDC dc, RECT area, int number, const WCHAR *title, const WCHAR *hint)
{
    WCHAR num[4];
    RECT circle = { area.left, area.top, area.left + S(26), area.top + S(26) };
    RECT t = { area.left + S(36), area.top, area.right, area.top + S(26) };
    RECT h = { area.left + S(36), area.top + S(28), area.right, area.top + S(48) };
    box(dc, circle, S(26), COL_TEXT, COL_TEXT, 1);
    _snwprintf(num, 4, L"%d", number);
    text(dc, g_fBold, RGB(255, 255, 255), circle, DT_CENTER | DT_VCENTER | DT_SINGLELINE, num);
    text(dc, g_fHead, COL_TEXT, t, DT_VCENTER | DT_SINGLELINE, title);
    text(dc, g_fText, COL_MUTED, h, DT_SINGLELINE | DT_END_ELLIPSIS, hint);
}

/* The list of connected screens: one row each, with a button to choose its
 * workplace.  Screens that are plugged in but off get a "Turn on" button. */
static void paint_screen_list(HDC dc, RECT stage)
{
    RECT list = { stage.left + (stage.right - stage.left) * 36 / 100, stage.top + S(8), stage.right - S(10), stage.bottom - S(8) };
    RECT divider = { list.left - S(6), stage.top + S(12), list.left - S(5), stage.bottom - S(12) };
    fill(dc, &divider, RGB(236, 238, 241));
    int saved = SaveDC(dc);
    IntersectClipRect(dc, list.left, list.top, list.right, list.bottom);
    int rowsH = (g_screenCount + g_idleCount) * S(SCREEN_ROW_H), maxScroll = rowsH - (list.bottom - list.top);
    if (maxScroll < 0) maxScroll = 0;
    if (g_screenScroll > maxScroll) g_screenScroll = maxScroll;
    if (g_screenScroll < 0) g_screenScroll = 0;
    int y = list.top - g_screenScroll;
    for (int i = 0; i < MAX_SCREENS; i++) SetRectEmpty(&g_screenRow[i]);
    for (int i = 0; i < MAX_IDLE; i++) SetRectEmpty(&g_idleRow[i]);

    for (int i = 0; i < g_screenCount + g_idleCount; i++, y += S(SCREEN_ROW_H)) {
        BOOL idle = i >= g_screenCount;
        Screen *sc = idle ? NULL : &g_screens[i];
        IdleScreen *id = idle ? &g_idle[i - g_screenCount] : NULL;
        RECT row = { list.left, y, list.right, y + S(SCREEN_ROW_H) };
        BOOL hover = idle ? (g_hoverKind == HIT_IDLE && g_hoverIndex == i - g_screenCount)
                          : (g_hoverKind == HIT_SCREEN && g_hoverIndex == i);
        COLORREF color = idle ? RGB(190, 120, 0) : seat_color(sc->seat);
        WCHAR line[160], label[40], num[8];
        if (idle) g_idleRow[i - g_screenCount] = row; else g_screenRow[i] = row;
        if (hover) fill(dc, &row, idle ? RGB(255, 240, 200) : RGB(240, 244, 250));

        RECT badgeRc = { row.left + S(6), y + S(6), row.left + S(28), y + S(SCREEN_ROW_H) - S(6) };
        box(dc, badgeRc, S(6), idle ? COL_NOTE_BG : tint(color, 82), color, 1);
        if (idle) lstrcpynW(num, L"+", 8); else _snwprintf(num, 8, L"%d", i + 1);
        text(dc, g_fBold, color, badgeRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE, num);

        int pillW = S(124);
        RECT pill = { row.right - pillW - S(4), y + S(5), row.right - S(4), y + S(SCREEN_ROW_H) - S(5) };
        RECT nameRc = { row.left + S(36), y, pill.left - S(8), y + S(SCREEN_ROW_H) };
        if (idle)
            _snwprintf(line, 160, L"%ls%ls%ls  \x2014  %ls", id->name, id->connector[0] ? L" \x00B7 " : L"", id->connector,
                       id->mirrored ? L"copy of another screen" : L"connected, not in use");
        else
            _snwprintf(line, 160, L"%ls%ls%ls  \x00B7  %ld \x00D7 %ld", sc->name[0] ? sc->name : L"Screen",
                       sc->connector[0] ? L" \x00B7 " : L"", sc->connector,
                       sc->area.right - sc->area.left, sc->area.bottom - sc->area.top);
        line[159] = 0;
        text(dc, g_fText, COL_TEXT, nameRc, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, line);

        box(dc, pill, S(8), idle ? COL_NOTE_BG : tint(color, 86), color, 1);
        RECT pillText = { pill.left + S(10), pill.top, pill.right - S(22), pill.bottom };
        if (idle) lstrcpynW(label, L"Turn on", 40); else seat_label(sc->seat, label, 40);
        if (idle) pillText.right = pill.right - S(10);
        text(dc, idle ? g_fBold : g_fText, color, pillText, DT_VCENTER | DT_SINGLELINE | (idle ? DT_CENTER : 0), label);
        if (!idle) {
            POINT arrow[3] = { { pill.right - S(18), (pill.top + pill.bottom) / 2 - S(2) },
                               { pill.right - S(10), (pill.top + pill.bottom) / 2 - S(2) },
                               { pill.right - S(14), (pill.top + pill.bottom) / 2 + S(3) } };
            HBRUSH brush = CreateSolidBrush(color);
            HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            Polygon(dc, arrow, 3);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(brush);
        }
    }
    if (g_screenCount + g_idleCount == 0) {
        text(dc, g_fText, COL_MUTED, list, DT_CENTER | DT_VCENTER | DT_SINGLELINE, L"Connect a screen with a cable and it appears here.");
    }
    RestoreDC(dc, saved);
    if (maxScroll > 0) { /* a thin bar shows there are more rows: turn the mouse wheel */
        int h = list.bottom - list.top, barH = h * h / rowsH, barY = list.top + (h - barH) * g_screenScroll / maxScroll;
        RECT bar = { list.right + S(3), barY, list.right + S(7), barY + barH };
        fill(dc, &bar, RGB(190, 195, 203));
    }
}

static void paint_screens(HDC dc)
{
    RECT area = g_rcScreens, stage;
    section_title(dc, area, 1, L"Screens: monitors, TVs and laptop displays",
                  L"Every connected screen gets its own workplace. Use the button on a row to change which one.");
    SetRect(&stage, area.left, area.top + S(56), area.right, area.bottom);
    box(dc, stage, S(10), COL_PANEL, COL_LINE, 1);
    paint_screen_list(dc, stage);
    stage.right = stage.left + (stage.right - stage.left) * 36 / 100; /* the picture keeps the left part */
    if (g_screenCount == 0) {
        text(dc, g_fText, COL_MUTED, stage, DT_CENTER | DT_VCENTER | DT_SINGLELINE, L"No screens found.");
        return;
    }

    RECT all = g_screens[0].area;
    for (int i = 1; i < g_screenCount; i++) UnionRect(&all, &all, &g_screens[i].area);
    int pad = S(18), availW = stage.right - stage.left - pad * 2, availH = stage.bottom - stage.top - pad * 2;
    int allW = all.right - all.left, allH = all.bottom - all.top;
    if (allW < 1 || allH < 1 || availW < 1 || availH < 1) return;
    /* Fit the desktop into the stage, but do not blow one small screen up to fill it. */
    double scale = (double)availW / allW;
    if ((double)availH / allH < scale) scale = (double)availH / allH;
    double cap = (double)S(300) / 1920.0;
    if (scale > cap) scale = cap;
    int offX = stage.left + pad + (availW - (int)(allW * scale)) / 2;
    int offY = stage.top + pad + (availH - (int)(allH * scale)) / 2;

    for (int i = 0; i < g_screenCount; i++) {
        Screen *s = &g_screens[i];
        WCHAR line[64], label[32];
        COLORREF color = seat_color(s->seat);
        BOOL hover = g_hoverKind == HIT_SCREEN && g_hoverIndex == i;
        SetRect(&s->draw, offX + (int)((s->area.left - all.left) * scale) + S(3),
                offY + (int)((s->area.top - all.top) * scale) + S(3),
                offX + (int)((s->area.right - all.left) * scale) - S(3),
                offY + (int)((s->area.bottom - all.top) * scale) - S(3));
        box(dc, s->draw, S(8), tint(color, hover ? 70 : 82), color, S(hover ? 3 : 2));

        RECT r = s->draw;
        int h = r.bottom - r.top;
        RECT big = { r.left, r.top + h * 8 / 100, r.right, r.top + h * 50 / 100 };
        RECT mid = { r.left + S(4), r.top + h * 50 / 100, r.right - S(4), r.top + h * 70 / 100 };
        RECT low = { r.left + S(4), r.top + h * 70 / 100, r.right - S(4), r.bottom - S(4) };
        _snwprintf(line, 64, L"%d", i + 1);
        if (h < S(84) || r.right - r.left < S(130)) { /* small picture: the number only, details are in the list */
            text(dc, h < S(44) ? g_fBold : g_fHuge, color, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, line);
            continue;
        }
        text(dc, g_fHuge, color, big, DT_CENTER | DT_VCENTER | DT_SINGLELINE, line);
        seat_label(s->seat, label, 32);
        text(dc, g_fBold, color, mid, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, label);
        if (s->name[0] || s->connector[0])
            _snwprintf(line, 64, L"%ls%ls%ls", s->name[0] ? s->name : L"Screen", s->connector[0] ? L"  \x00B7  " : L"", s->connector);
        else
            _snwprintf(line, 64, L"%ld \x00D7 %ld%ls", s->area.right - s->area.left, s->area.bottom - s->area.top,
                       s->primary ? L"  \x00B7  main" : L"");
        line[63] = 0;
        text(dc, g_fSmall, COL_MUTED, low, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS, line);
    }
}

/* scale in percent: 100 for list rows, larger for the device tiles. */
#define Z(v) MulDiv(S(v), scale, 100)
static void paint_device_icon_scaled(HDC dc, RECT rc, int kinds, COLORREF color, int scale)
{
    HPEN pen = CreatePen(PS_SOLID, Z(2), color);
    HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
    if (kinds & DEV_KEYBOARD) {
        int w = Z(kinds & DEV_MOUSE ? 18 : 26), x = (kinds & DEV_MOUSE) ? cx - Z(16) : cx - w / 2; /* side by side, centred */
        RoundRect(dc, x, cy - Z(8), x + w, cy + Z(8), Z(4), Z(4));
        MoveToEx(dc, x + Z(5), cy + Z(3), NULL);
        LineTo(dc, x + w - Z(5), cy + Z(3));
        MoveToEx(dc, x + Z(5), cy - Z(3), NULL);
        LineTo(dc, x + w - Z(5), cy - Z(3));
    }
    if (kinds & DEV_MOUSE) {
        int x = (kinds & DEV_KEYBOARD) ? cx + Z(6) : cx - Z(7), w = (kinds & DEV_KEYBOARD) ? Z(10) : Z(14);
        RoundRect(dc, x, cy - Z(11), x + w, cy + Z(11), w, w);
        MoveToEx(dc, x + w / 2, cy - Z(7), NULL);
        LineTo(dc, x + w / 2, cy - Z(2));
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

#undef Z

static void paint_devices(HDC dc)
{
    RECT area = g_rcDevices, list;
    section_title(dc, area, 2, L"Devices: keyboards and mice",
                  L"Each device works only on its own workplace's screen. Click one to move it. The one in use blinks.");
    SetRect(&list, area.left, area.top + S(56), area.right, area.bottom);
    box(dc, list, S(10), COL_PANEL, COL_LINE, 1);
    if (g_deviceCount == 0) {
        text(dc, g_fText, COL_MUTED, list, DT_CENTER | DT_VCENTER | DT_SINGLELINE, L"No keyboards or mice found.");
        return;
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, list.left + 1, list.top + S(3), list.right - 1, list.bottom - S(3));
    DWORD now = GetTickCount();
    int cols = tile_columns();
    for (int i = 0; i < g_deviceCount; i++) {
        Device *d = &g_devices[i];
        WCHAR label[NAME_CHARS + 20];
        COLORREF color = seat_color(d->seat);
        BOOL hover = g_hoverKind == HIT_DEVICE && g_hoverIndex == i;
        BOOL inUse = d->flashUntil && (int)(d->flashUntil - now) > 0;
        BOOL blinkOn = inUse && (now / 250) % 2 == 0; /* in use: blinks on and off */
        int x = list.left + S(TILE_GAP) + (i % cols) * (S(TILE_W) + S(TILE_GAP));
        int y = list.top + S(TILE_GAP) + (i / cols) * (S(TILE_H) + S(TILE_GAP)) - g_scroll;
        SetRect(&d->draw, x, y, x + S(TILE_W), y + S(TILE_H));
        box(dc, d->draw, S(12), blinkOn ? COL_FLASH : hover ? RGB(240, 244, 250) : RGB(250, 251, 253),
            inUse ? color : hover ? tint(color, 40) : COL_LINE, inUse ? S(3) : 1);

        /* the device itself, drawn large, in its workplace's colour */
        RECT icon = { x, y + S(8), x + S(TILE_W), y + S(48) };
        paint_device_icon_scaled(dc, icon, d->kinds, d->seat == NOT_USED ? COL_UNUSED : color, 170);
        if (inUse) {
            RECT tag = { x + S(TILE_W) - S(62), y + S(6), x + S(TILE_W) - S(6), y + S(24) };
            box(dc, tag, S(8), color, color, 1);
            text(dc, g_fSmall, RGB(255, 255, 255), tag, DT_CENTER | DT_VCENTER | DT_SINGLELINE, L"in use");
        }
        RECT name = { x + S(8), y + S(50), x + S(TILE_W) - S(8), y + S(70) };
        text(dc, g_fBold, COL_TEXT, name, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS, d->name);

        /* which workplace (and screen) it belongs to; click to move it */
        RECT pill = { x + S(10), y + S(TILE_H) - S(34), x + S(TILE_W) - S(10), y + S(TILE_H) - S(10) };
        box(dc, pill, S(8), tint(color, 86), color, 1);
        int scr = d->seat >= 0 ? seat_first_screen(d->seat) : -1;
        seat_label(d->seat, label, NAME_CHARS);
        if (scr >= 0) _snwprintf(label + wcslen(label), 20, L" \x00B7 screen %d", scr + 1);
        label[NAME_CHARS + 19] = 0;
        RECT pillText = { pill.left + S(8), pill.top, pill.right - S(20), pill.bottom };
        text(dc, g_fSmall, color, pillText, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, label);
        POINT arrow[3] = { { pill.right - S(16), (pill.top + pill.bottom) / 2 - S(2) },
                           { pill.right - S(8), (pill.top + pill.bottom) / 2 - S(2) },
                           { pill.right - S(12), (pill.top + pill.bottom) / 2 + S(3) } };
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, arrow, 3);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
    }
    RestoreDC(dc, saved);
}

static void paint_seats(HDC dc)
{
    RECT area = g_rcSeats;
    RECT title = { area.left, area.top, area.right - S(136), area.top + S(26) };
    RECT hint = { area.left, area.top + S(28), area.right, area.top + S(48) };
    text(dc, g_fHead, COL_TEXT, title, DT_VCENTER | DT_SINGLELINE, L"Workplaces");
    text(dc, g_fText, COL_MUTED, hint, DT_SINGLELINE | DT_END_ELLIPSIS, L"Start each one with its own button.");

    /* With many workplaces the cards shrink, down to one line each. */
    int gap = S(6), room = area.bottom - S(40) - (area.top + S(56));
    int cardH = (room - gap * (g_seatCount - 1)) / g_seatCount, y = area.top + S(56);
    if (cardH > S(116)) cardH = S(116);
    if (cardH < S(30)) cardH = S(30);
    for (int seat = 0; seat < MAX_SEATS; seat++) { SetRectEmpty(&g_seatDraw[seat]); SetRectEmpty(&g_startPill[seat]); }
    for (int seat = 0; seat < g_seatCount; seat++, y += cardH + gap) {
        WCHAR label[NAME_CHARS], why[200];
        COLORREF color = SEAT_COLORS[seat];
        int state = seat_state(seat, why, 200);
        RECT card = { area.left, y, area.right, y + cardH };
        g_seatDraw[seat] = card;
        box(dc, card, S(10), COL_PANEL, COL_LINE, 1);
        BOOL small = cardH < S(60);
        int pad = small ? S(5) : S(10), top = small ? (cardH - S(24)) / 2 : S(8);
        RECT stripe = { card.left + 1, card.top + pad, card.left + S(6), card.bottom - pad };
        fill(dc, &stripe, color);

        /* Start / Stop button of this workplace only. */
        static const WCHAR *stateText[] = { L"Not started", L"Starting\x2026", L"Running", L"Error" };
        static const COLORREF stateColor[] = { RGB(110, 116, 128), RGB(190, 110, 0), RGB(30, 130, 76), RGB(190, 40, 40) };
        BOOL stoppable = want_running(seat);
        RECT pill = { card.right - S(74), card.top + top, card.right - S(10), card.top + top + S(24) };
        g_startPill[seat] = pill;
        BOOL hoverPill = g_hoverKind == HIT_SEAT_START && g_hoverIndex == seat;
        COLORREF pillColor = state == WS_STARTING ? RGB(170, 175, 185) : stoppable ? RGB(190, 40, 40) : RGB(30, 130, 76);
        box(dc, pill, S(8), hoverPill && state != WS_STARTING ? tint(pillColor, 15) : pillColor, pillColor, 1);
        text(dc, g_fBold, RGB(255, 255, 255), pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE,
             state == WS_STARTING ? L"\x2026" : stoppable ? L"Stop" : L"Start");

        RECT statusRc = { card.right - S(170), card.top + top, pill.left - S(8), card.top + top + S(24) };
        RECT name = { card.left + S(16), card.top + top, statusRc.left - S(4), card.top + top + S(24) };
        seat_label(seat, label, NAME_CHARS);
        text(dc, small ? g_fBold : g_fHead, color, name, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, label);
        text(dc, g_fBold, stateColor[state], statusRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, stateText[state]);
        if (small) continue; /* details: click the card */

        struct { const WCHAR *word; BOOL yes; } parts[3] = {
            { L"Screen", seat_screens(seat) > 0 },
            { L"Keyboard", seat_has(seat, DEV_KEYBOARD) },
            { L"Mouse", seat_has(seat, DEV_MOUSE) },
        };
        int x = card.left + S(16), partW = (card.right - card.left - S(28)) / 3;
        for (int p = 0; p < 3; p++, x += partW) {
            RECT word = { x + S(22), card.top + S(38), x + partW, card.top + S(58) };
            badge(dc, x, card.top + S(40), S(16), parts[p].yes);
            text(dc, g_fText, COL_TEXT, word, DT_VCENTER | DT_SINGLELINE, parts[p].word);
        }
        if (cardH < S(86)) continue;
        WCHAR who[120];
        RECT share = { card.left + S(16), card.top + S(62), card.right - S(12), card.top + S(82) };
        BOOL hover = g_hoverKind == HIT_SEAT && g_hoverIndex == seat;
        if (seat == 0) lstrcpynW(who, L"The main Windows desktop", 120);
        else share_summary(seat, who, 120);
        text(dc, hover ? g_fBold : g_fText, seat > 0 && !(g_share[seat] & SHARE_ON) ? COL_WARN : COL_TEXT, share,
             DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, who);

        if (cardH < S(112)) continue;
        RECT tip = { card.left + S(16), card.top + S(88), card.right - S(12), card.bottom - S(6) };
        WCHAR problem[200];
        if (state == WS_ERROR) text(dc, g_fSmall, stateColor[WS_ERROR], tip, DT_SINGLELINE | DT_END_ELLIPSIS, why);
        else if (state == WS_NOT_STARTED && seat_problem(seat, problem, 200))
            text(dc, g_fSmall, COL_WARN, tip, DT_SINGLELINE | DT_END_ELLIPSIS, problem);
        else
            text(dc, g_fSmall, COL_MUTED, tip, DT_SINGLELINE | DT_END_ELLIPSIS,
                 state == WS_RUNNING ? L"Its windows and mouse stay on its own screen." : L"Click the card to rename it or change its settings.");
    }
}

static void paint(HDC dc)
{
    RECT rc;
    GetClientRect(g_wnd, &rc);
    fill(dc, &rc, COL_BG);

    RECT head = { 0, 0, rc.right, S(78) };
    fill(dc, &head, COL_PANEL);
    /* The logo, then the name and what this window is for. */
    static HICON logo;
    if (!logo) logo = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, S(52), S(52), 0);
    if (logo) DrawIconEx(dc, S(18), S(13), logo, S(52), S(52), 0, NULL, DI_NORMAL);
    RECT title = { S(82), S(10), rc.right - S(20), S(44) };
    RECT sub = { S(82), S(46), rc.right - S(430), S(70) }; /* stop before the two buttons on the right */
    text(dc, g_fTitle, COL_TEXT, title, DT_VCENTER | DT_SINGLELINE, g_demo ? L"VISION  \x00B7  Set up workplaces  (demo)" : L"VISION  \x00B7  Set up workplaces");
    text(dc, g_fText, COL_MUTED, sub, DT_SINGLELINE | DT_END_ELLIPSIS,
         L"Version " TEXT(VISION_VERSION) L"   \x00B7   Several people share this PC: each one gets a screen, a keyboard and a mouse.");

    RECT note = { 0, S(78), rc.right, S(110) };
    g_rcNote = note;
    if (g_update != UP_NONE) {
        const UpdateInfo *u = update_found();
        WCHAR line[500];
        BOOL bad = g_update == UP_FAILED, hover = g_hoverKind == HIT_UPDATE;
        COLORREF bg = bad ? RGB(253, 228, 228)
                    : g_update == UP_FOUND && g_updateBlink ? RGB(30, 150, 80)   /* blinks until clicked */
                    : hover && g_update == UP_FOUND ? RGB(196, 236, 208) : RGB(214, 243, 222);
        fill(dc, &note, bg);
        if (g_update == UP_FOUND && g_updateCountdown > 0)
            _snwprintf(line, 500, L"\x2191  VISION %hs is here. Updating by itself in %d s\x2026   Click here to update now.",
                       u->version, g_updateCountdown);
        else if (g_update == UP_FOUND)
            _snwprintf(line, 500, L"\x2191  Update available: VISION %hs (you have %hs).%ls%ls   Click here to update.", u->version,
                       VISION_VERSION, u->notes[0] ? L"  " : L"", u->notes);
        else if (g_update == UP_DOWNLOADING)
            _snwprintf(line, 500, L"Downloading the update\x2026 %d%%   VISION restarts by itself when it is ready.", g_updatePct);
        else if (g_update == UP_INSTALLING)
            _snwprintf(line, 500, L"Installing the update. VISION closes now and opens again in a moment\x2026");
        else
            _snwprintf(line, 500, L"The update did not work: %ls. Click here to try again.", g_updateWhy);
        line[499] = 0;
        if (g_update == UP_DOWNLOADING) { /* progress bar along the bottom of the strip */
            RECT bar = { 0, note.bottom - S(4), MulDiv(rc.right, g_updatePct, 100), note.bottom };
            fill(dc, &bar, COL_OK);
        }
        RECT t = { S(20), note.top, rc.right - S(20), note.bottom };
        text(dc, g_fBold, bad ? RGB(160, 30, 30) : g_update == UP_FOUND && g_updateBlink ? RGB(255, 255, 255) : RGB(20, 100, 50), t,
             DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, line);
        goto after_note;
    }
    fill(dc, &note, COL_NOTE_BG);
    RECT noteText = { S(20), S(78), rc.right - S(20), S(110) };
    text(dc, g_fText, COL_NOTE_TX, noteText, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
         g_demo ? L"Demo: these screens and devices are examples, not your real hardware."
         : is_server_os() ? L"Windows Server: every workplace gets its own Windows sign-in on this PC, like a separate PC. It starts with Windows."
                          : L"Windows 10/11: workplaces share one Windows sign-in; VISION keeps each one's windows and mouse on its own screen.");

after_note:
    paint_screens(dc);
    paint_devices(dc);
    paint_seats(dc);

    fill(dc, &g_rcBar, COL_PANEL);
    RECT line = { 0, g_rcBar.top, rc.right, g_rcBar.top + 1 };
    fill(dc, &line, COL_LINE);
    RECT status = { S(20) + S(446), g_rcBar.top, rc.right - S(20) - S(310), g_rcBar.bottom };
    text(dc, g_savedOk ? g_fBold : g_fText, g_savedOk ? COL_OK : g_dirty && !g_status[0] ? COL_WARN : COL_MUTED, status, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
         g_status[0] ? g_status : g_dirty ? L"You have changes that are not saved." : L"");
}

/* ---- interaction --------------------------------------------------------- */

static int hit_test(POINT pt, int *index)
{
    if ((g_update == UP_FOUND || g_update == UP_FAILED) && PtInRect(&g_rcNote, pt)) { *index = 0; return HIT_UPDATE; }
    BOOL inList = pt.y >= g_rcScreens.top + S(64) && pt.y < g_rcScreens.bottom - S(8); /* rows scrolled out of view do not count */
    for (int i = 0; i < g_idleCount; i++)
        if (inList && PtInRect(&g_idleRow[i], pt)) { *index = i; return HIT_IDLE; }
    for (int i = 0; i < g_screenCount; i++)
        if (PtInRect(&g_screens[i].draw, pt) || (inList && PtInRect(&g_screenRow[i], pt))) { *index = i; return HIT_SCREEN; }
    RECT list = { g_rcDevices.left, g_rcDevices.top + S(59), g_rcDevices.right, g_rcDevices.bottom - S(3) };
    if (PtInRect(&list, pt))
        for (int i = 0; i < g_deviceCount; i++)
            if (PtInRect(&g_devices[i].draw, pt)) { *index = i; return HIT_DEVICE; }
    for (int s = 0; s < g_seatCount; s++)
        if (PtInRect(&g_startPill[s], pt)) { *index = s; return HIT_SEAT_START; }
    for (int s = 0; s < g_seatCount; s++)
        if (PtInRect(&g_seatDraw[s], pt)) { *index = s; return HIT_SEAT; }
    *index = -1;
    return HIT_NONE;
}

static void choose_seat(int kind, int index, POINT screenPt)
{
    HMENU menu = CreatePopupMenu();
    int current = kind == HIT_SCREEN ? g_screens[index].seat : g_devices[index].seat;
    if (kind == HIT_DEVICE) {
        WCHAR title[200];
        _snwprintf(title, 200, L"Move \x201C%ls\x201D to:", g_devices[index].name);
        title[199] = 0;
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, title);
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    }
    for (int seat = 0; seat < g_seatCount; seat++) {
        WCHAR label[160];
        int other = kind == HIT_SCREEN ? seat_first_screen(seat) : -1;
        seat_label(seat, label, 80);
        int scr = seat_first_screen(seat);
        if (kind == HIT_DEVICE && scr >= 0) {
            if (g_screens[scr].name[0])
                _snwprintf(label + wcslen(label), 160 - wcslen(label), L"   \x00B7  screen %d (%ls)", scr + 1, g_screens[scr].name);
            else
                _snwprintf(label + wcslen(label), 160 - wcslen(label), L"   \x00B7  screen %d", scr + 1);
            label[159] = 0;
        }
        if (kind == HIT_SCREEN && seat != current && other >= 0) /* one screen per workplace: this swaps them */
            _snwprintf(label + wcslen(label), 160 - wcslen(label), L"   (swap with screen %d)", other + 1);
        AppendMenuW(menu, MF_STRING | (seat == current ? MF_CHECKED : 0), ID_MENU_SEAT + seat, label);
    }
    if (g_seatCount < MAX_SEATS) AppendMenuW(menu, MF_STRING, ID_MENU_SEAT + g_seatCount, L"New workplace");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (current == NOT_USED ? MF_CHECKED : 0), ID_MENU_NONE, L"Not used");
    g_menuKind = kind;
    g_menuIndex = index;
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, screenPt.x, screenPt.y, 0, g_wnd, NULL);
    DestroyMenu(menu);
}

static void assign(int kind, int index, int seat)
{
    WCHAR label[40], msg[300];
    if (seat >= g_seatCount && seat < MAX_SEATS) g_seatCount = seat + 1;
    seat_label(seat, label, 40);
    if (kind == HIT_SCREEN && index < g_screenCount) {
        /* One screen per workplace: a screen already there swaps places with this one. */
        int old = g_screens[index].seat, swapped = -1;
        for (int i = 0; seat != NOT_USED && i < g_screenCount; i++)
            if (i != index && g_screens[i].seat == seat) { g_screens[i].seat = old; swapped = i; }
        g_screens[index].seat = seat;
        if (swapped >= 0) {
            WCHAR oldLabel[40];
            seat_label(old, oldLabel, 40);
            _snwprintf(msg, 300, L"Screen %d \x2192 %ls, screen %d \x2192 %ls. Not saved yet.", index + 1, label, swapped + 1, oldLabel);
        } else {
            _snwprintf(msg, 300, L"Screen %d \x2192 %ls. Not saved yet.", index + 1, label);
        }
    } else if (kind == HIT_DEVICE && index < g_deviceCount) {
        g_devices[index].seat = seat;
        _snwprintf(msg, 300, L"%ls \x2192 %ls. Not saved yet.", g_devices[index].name, label);
    } else {
        return;
    }
    msg[299] = 0;
    lstrcpynW(g_status, msg, 300);
    changed();
}

/* Give every screen a workplace of its own: screen 1 stays with workplace 1,
 * each further screen gets the next workplace (up to the limit). */
static int one_workplace_per_screen(void)
{
    int changes = 0;
    for (int i = 0; i < g_screenCount; i++) {
        int want = i < MAX_SEATS ? i : NOT_USED;
        if (g_screens[i].seat != want) { g_screens[i].seat = want; changes++; }
    }
    int needed = g_screenCount < MAX_SEATS ? g_screenCount : MAX_SEATS;
    if (needed < 1) needed = 1;
    if (g_seatCount < needed) { g_seatCount = needed; changes++; }
    return changes;
}

/* A newly connected screen gets a new workplace of its own straight away. */
static void give_new_screen_a_workplace(int i)
{
    for (int seat = 0; seat < g_seatCount; seat++)
        if (seat_first_screen(seat) < 0) { g_screens[i].seat = seat; return; } /* reuse a workplace that has no screen */
    if (g_seatCount < MAX_SEATS) {
        g_share[g_seatCount] = SHARE_DEFAULT;
        g_screens[i].seat = g_seatCount++;
    } else {
        g_screens[i].seat = NOT_USED; /* all 10 workplaces are taken */
    }
}

/* Enforce "one screen per workplace": a screen that shares a workplace with
 * an earlier screen (for example one just plugged in) gets its own. */
static int separate_shared_screens(void)
{
    int moved = 0;
    for (int i = 1; i < g_screenCount; i++) {
        if (g_screens[i].seat == NOT_USED) continue;
        for (int j = 0; j < i; j++) {
            if (g_screens[j].seat != g_screens[i].seat) continue;
            give_new_screen_a_workplace(i);
            moved++;
            break;
        }
    }
    return moved;
}

static void remove_last_seat(void)
{
    int last = g_seatCount - 1;
    if (last < 1) return;
    for (int i = 0; i < g_screenCount; i++) if (g_screens[i].seat == last) g_screens[i].seat = 0;
    for (int i = 0; i < g_deviceCount; i++) if (g_devices[i].seat == last) g_devices[i].seat = 0;
    g_share[last] = SHARE_DEFAULT;
    g_names[last][0] = 0;
    forget_running(last); /* a removed workplace stops; a new one later starts fresh */
    g_startError[last][0] = 0;
    g_seatCount--;
    _snwprintf(g_status, 300, L"Workplace %d removed. Its screens and devices went back to Workplace 1.", last + 1);
    changed();
}

/* VISION always watches the keyboards and mice, so the one in use blinks. */
static BOOL watch_devices(void)
{
    RAWINPUTDEVICE want[2] = {
        { 1, 6, RIDEV_INPUTSINK, g_wnd }, /* keyboards */
        { 1, 2, RIDEV_INPUTSINK, g_wnd }, /* mice */
    };
    return RegisterRawInputDevices(want, 2, sizeof(want[0]));
}

static void set_finding(BOOL on)
{
    if (on && !watch_devices()) {
        set_status(L"Windows did not allow watching the devices.");
        return;
    }
    g_finding = on;
    set_status(on ? L"Now press a key or move a mouse. The device you use lights up in the list." : L"");
    update_buttons();
}

static void on_raw_input(HRAWINPUT raw)
{
    RAWINPUT input;
    UINT size = sizeof(input);
    if (GetRawInputData(raw, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    if (g_wizSeat >= 0) { wizard_input(&input); return; }
    if (!input.header.hDevice) return;
    for (int i = 0; i < g_deviceCount; i++) {
        Device *d = &g_devices[i];
        for (int h = 0; h < d->handleCount; h++) {
            if (d->handles[h] != input.header.hDevice) continue;
            BOOL fresh = !d->flashUntil;
            d->flashUntil = GetTickCount() + 1500;
            if (d->flashUntil == 0) d->flashUntil = 1;
            if (fresh && g_finding) { /* "Find a device": also scroll to it and say which it is */
                WCHAR label[40], msg[300];
                show_device(i);
                seat_label(d->seat, label, 40);
                _snwprintf(msg, 300, L"You used: %ls  (%ls)", d->name, label);
                msg[299] = 0;
                lstrcpynW(g_status, msg, 300);
                InvalidateRect(g_wnd, NULL, FALSE);
            }
            if (fresh) InvalidateRect(g_wnd, &g_rcDevices, FALSE);
            SetTimer(g_wnd, 1, 125, NULL); /* blink */
            return;
        }
    }
}

/* ---- automatic setup ------------------------------------------------------ */

static LRESULT CALLBACK wizard_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(w, &ps);
        GetClientRect(w, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(16, 24, 48));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        static HICON logo;
        if (!logo) logo = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, S(96), S(96), 0);
        int mid = rc.bottom / 2;
        if (logo) DrawIconEx(dc, (rc.right - S(96)) / 2, mid - S(200), logo, S(96), S(96), 0, NULL, DI_NORMAL);
        WCHAR title[40];
        _snwprintf(title, 40, L"Workplace %d", g_wizSeat + 1);
        RECT a = { 0, mid - S(96), rc.right, mid - S(20) };
        RECT b = { S(20), mid, rc.right - S(20), mid + S(50) };
        RECT c = { S(20), mid + S(60), rc.right - S(20), mid + S(100) };
        SelectObject(dc, g_fHuge);
        SetTextColor(dc, SEAT_COLORS[g_wizSeat] == SEAT_COLORS[0] ? RGB(120, 165, 255) : tint(SEAT_COLORS[g_wizSeat], 35));
        DrawTextW(dc, title, -1, &a, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, g_fHead);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, g_wizKind == DEV_KEYBOARD ? L"Press any key on the keyboard for this screen."
                                                : L"Now click with the mouse for this screen.", -1, &b,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, g_fText);
        SetTextColor(dc, RGB(170, 180, 205));
        DrawTextW(dc, L"VISION automatic setup  \x00B7  Press Esc to skip this step.", -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(w, &ps);
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { wizard_skip(); return 0; }
    return DefWindowProcW(w, msg, wp, lp);
}

static void wizard_watch(BOOL on) { if (on) watch_devices(); } /* devices are always watched */

static void start_workplaces(void);

static void wizard_show(void)
{
    int scr = seat_first_screen(g_wizSeat);
    RECT a = g_screens[scr].area;
    if (!g_wizPrompt) {
        WNDCLASSW cls;
        ZeroMemory(&cls, sizeof(cls));
        cls.lpfnWndProc = wizard_proc;
        cls.hInstance = GetModuleHandleW(NULL);
        cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
        cls.lpszClassName = L"VISIONAutoSetup";
        RegisterClassW(&cls);
        g_wizPrompt = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"VISIONAutoSetup",
                                      L"VISION automatic setup", WS_POPUP, 0, 0, 10, 10, NULL, NULL, cls.hInstance, NULL);
    }
    SetWindowPos(g_wizPrompt, HWND_TOPMOST, a.left, a.top, a.right - a.left, a.bottom - a.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_wizPrompt, NULL, TRUE);
    _snwprintf(g_status, 300, L"Automatic setup: look at screen %d.", scr + 1);
    InvalidateRect(g_wnd, NULL, FALSE);
}

/* Go to the next question: keyboard, then mouse, for every workplace with a screen. */
static void wizard_next(void)
{
    if (g_wizKind == DEV_KEYBOARD) {
        g_wizKind = DEV_MOUSE;
    } else {
        g_wizKind = DEV_KEYBOARD;
        do g_wizSeat++; while (g_wizSeat < g_seatCount && seat_first_screen(g_wizSeat) < 0);
    }
    if (g_wizSeat < g_seatCount) { wizard_show(); return; }

    g_wizSeat = -1;
    wizard_watch(FALSE);
    if (g_wizPrompt) ShowWindow(g_wizPrompt, SW_HIDE);
    changed();
    if (!save_all()) { set_status(L"Automatic setup finished, but the setup could not be saved."); return; }
    start_workplaces();
    lstrcpynW(g_status, L"Automatic setup finished and saved. The workplaces are starting.", 300);
    InvalidateRect(g_wnd, NULL, FALSE);
}

/* Esc skips a step.  It can arrive as raw input and as a normal key press;
 * count it once. */
static void wizard_skip(void)
{
    DWORD now = GetTickCount();
    if (g_wizSeat < 0 || now - g_wizEscAt < 400) return;
    g_wizEscAt = now;
    wizard_next();
}

static void wizard_input(const RAWINPUT *in)
{
    BOOL keyDown = in->header.dwType == RIM_TYPEKEYBOARD && !(in->data.keyboard.Flags & RI_KEY_BREAK);
    BOOL click = in->header.dwType == RIM_TYPEMOUSE &&
                 (in->data.mouse.usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN));
    if (keyDown && (in->data.keyboard.VKey == VK_ESCAPE || in->data.keyboard.MakeCode == 0x01)) { wizard_skip(); return; }
    if (!(g_wizKind == DEV_KEYBOARD ? keyDown : click)) return;
    for (int i = 0; i < g_deviceCount; i++) {
        Device *d = &g_devices[i];
        for (int h = 0; h < d->handleCount; h++) {
            if (d->handles[h] != in->header.hDevice) continue;
            if (g_wizTaken[i] && d->seat != g_wizSeat) return; /* already given to another workplace in this run */
            d->seat = g_wizSeat;
            g_wizTaken[i] = TRUE;
            wizard_next();
            return;
        }
    }
}

static void wizard_start(void)
{
    if (g_demo) { set_status(L"Demo: automatic setup needs real screens, keyboards and mice."); return; }
    if (g_wizSeat >= 0) return;
    if (g_finding) set_finding(FALSE);
    refresh_hardware(FALSE);
    one_workplace_per_screen();
    if (g_screenCount < 2) {
        MessageBoxW(g_wnd, L"Connect at least two screens first. Each screen becomes one workplace.", L"VISION",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    ZeroMemory(g_wizTaken, sizeof(g_wizTaken));
    for (int i = 0; i < g_deviceCount; i++) g_devices[i].seat = 0; /* each device is asked for again */
    wizard_watch(TRUE);
    g_wizSeat = 0;
    g_wizKind = DEV_KEYBOARD;
    changed();
    wizard_show();
}

static void demo_find_tick(void)
{
    static int next;
    if (!g_finding || g_deviceCount == 0) return;
    Device *d = &g_devices[next++ % g_deviceCount];
    WCHAR label[40], msg[300];
    d->flashUntil = GetTickCount() + 1500;
    SetTimer(g_wnd, 1, 125, NULL);
    seat_label(d->seat, label, 40);
    _snwprintf(msg, 300, L"You used: %ls  (%ls)", d->name, label);
    lstrcpynW(g_status, msg, 300);
    InvalidateRect(g_wnd, NULL, FALSE);
    SetTimer(g_wnd, 1, 200, NULL);
}

static BOOL confirm_close(void)
{
    if (!g_dirty) return TRUE;
    int answer = MessageBoxW(g_wnd, L"Save your changes before closing?", L"VISION", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return FALSE;
    if (answer == IDYES && !save_all()) {
        MessageBoxW(g_wnd, L"The setup could not be saved.", L"VISION", MB_OK | MB_ICONERROR);
        return FALSE;
    }
    return TRUE;
}

static void share_summary(int seat, WCHAR *out, size_t chars)
{
    int f = g_share[seat];
    if (!(f & SHARE_ON)) { lstrcpynW(out, L"Switched off", (int)chars); return; }
    if (!(f & (SHARE_CLIPBOARD | SHARE_FOLDER))) { lstrcpynW(out, L"Shares nothing (private)", (int)chars); return; }
    _snwprintf(out, chars, L"Shares: %ls%ls%ls", (f & SHARE_CLIPBOARD) ? L"copy and paste" : L"",
               (f & SHARE_CLIPBOARD) && (f & SHARE_FOLDER) ? L", " : L"", (f & SHARE_FOLDER) ? L"folder" : L"");
    out[chars - 1] = 0;
}

static INT_PTR CALLBACK sharing_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    if (msg == WM_INITDIALOG) {
        WCHAR folder[MAX_PATH], info[900];
        shared_folder(folder, MAX_PATH);
        _snwprintf(info, 900,
                   L"Switched on: the workplace starts by itself, with VISION and every time Windows starts.\r\n\r\n"
                   L"Copy and paste: text copied there can be pasted in Workplace 1, and the other way round.\r\n\r\n"
                   L"Shared folder: the workplace can open %ls. Put files there to pass them on.\r\n\r\n"
                   L"Sound: the workplace can play sound on this PC's speakers.\r\n\r\n"
                   L"Programs, files and screen content of one workplace are never shown in another.", folder);
        info[899] = 0;
        SetDlgItemTextW(dlg, IDC_SHARE_INFO, info);
        for (int seat = 1; seat < MAX_SEATS; seat++) {
            for (int col = 0; col < 4; col++) {
                HWND box = GetDlgItem(dlg, IDC_SHARE_BASE + seat * 10 + col);
                ShowWindow(box, seat < g_seatCount ? SW_SHOW : SW_HIDE);
                CheckDlgButton(dlg, IDC_SHARE_BASE + seat * 10 + col, (g_share[seat] & (1 << col)) ? BST_CHECKED : BST_UNCHECKED);
            }
            ShowWindow(GetDlgItem(dlg, IDC_SHARE_BASE + seat * 10 + 9), seat < g_seatCount ? SW_SHOW : SW_HIDE);
        }
        /* Close the gap left by the rows that are not shown. */
        RECT unit = { 0, 0, 0, 18 * (MAX_SEATS - g_seatCount) }, wr;
        MapDialogRect(dlg, &unit);
        int ids[3] = { IDC_SHARE_INFO, IDOK, IDCANCEL };
        for (int i = 0; i < 3; i++) {
            HWND c = GetDlgItem(dlg, ids[i]);
            RECT r;
            GetWindowRect(c, &r);
            MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
            SetWindowPos(c, NULL, r.left, r.top - unit.bottom, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        GetWindowRect(dlg, &wr);
        SetWindowPos(dlg, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top - unit.bottom, SWP_NOMOVE | SWP_NOZORDER);
        return TRUE;
    }
    if (msg != WM_COMMAND) return FALSE;
    if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
    if (LOWORD(wp) != IDOK) return FALSE;
    BOOL different = FALSE;
    for (int seat = 1; seat < g_seatCount; seat++) {
        int flags = 0;
        for (int col = 0; col < 4; col++)
            if (IsDlgButtonChecked(dlg, IDC_SHARE_BASE + seat * 10 + col) == BST_CHECKED) flags |= 1 << col;
        if (flags != g_share[seat]) different = TRUE;
        g_share[seat] = flags;
    }
    EndDialog(dlg, different ? IDOK : IDCANCEL);
    return TRUE;
}

static void edit_sharing(void)
{
    if (g_seatCount < 2) { set_status(L"Add a workplace first. Sharing is set for each extra workplace."); return; }
    if (DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_SHARING), g_wnd, sharing_proc, 0) == IDOK) {
        lstrcpynW(g_status, L"Sharing changed. Not saved yet. It applies the next time the workplaces start.", 300);
        changed();
    }
}

/* ---- start one workplace ---------------------------------------------------- */

static BOOL launch_engine(void)
{
    if (engine_running()) return TRUE; /* it picks up newly started workplaces by itself */
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.hwnd = g_wnd;
    info.lpVerb = L"open";
    info.lpFile = self;
    info.lpParameters = L"--keep-apart";
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info);
}

/* Start only this workplace, with exactly the settings the person entered. */
static void start_one(int seat)
{
    WCHAR why[200], label[NAME_CHARS], msg[300];
    seat_label(seat, label, NAME_CHARS);
    if (g_demo) { set_status(L"Demo: workplaces cannot be started in demo mode."); return; }
    int state = seat_state(seat, why, 200);
    if (state == WS_STARTING || state == WS_RUNNING) return; /* a second click does nothing */
    if (seat_problem(seat, why, 200)) {
        lstrcpynW(g_startError[seat], why, 200);
        _snwprintf(msg, 300, L"%ls could not start: %ls", label, why);
        set_status(msg);
        MessageBeep(MB_ICONWARNING);
        InvalidateRect(g_wnd, NULL, FALSE);
        return;
    }
    /* Save what is on screen first, so the workplace runs with these settings. */
    if (g_dirty && !save_all()) { set_status(L"The setup could not be saved, so the workplace was not started."); return; }
    g_startError[seat][0] = 0;
    g_startAt[seat] = GetTickCount();
    report_status(seat, WS_STARTING, NULL);
    set_want_running(seat, TRUE);
    if (!launch_engine()) {
        set_want_running(seat, FALSE);
        lstrcpynW(g_startError[seat], L"VISION could not be started.", 200);
    }
    _snwprintf(msg, 300, L"Starting %ls\x2026", label);
    set_status(msg);
    update_buttons();
    InvalidateRect(g_wnd, NULL, FALSE);
}

static void stop_one(int seat)
{
    WCHAR label[NAME_CHARS], msg[300];
    seat_label(seat, label, NAME_CHARS);
    set_want_running(seat, FALSE);
    g_startError[seat][0] = 0;
    _snwprintf(msg, 300, L"%ls stopped.", label);
    set_status(msg);
    InvalidateRect(g_wnd, NULL, FALSE);
}

/* ---- one workplace's settings --------------------------------------------- */

static INT_PTR CALLBACK workplace_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static int seat;
    static const int boxes[4] = { IDC_WP_ON, IDC_WP_CLIP, IDC_WP_FOLDER, IDC_WP_SOUND };
    if (msg == WM_INITDIALOG) {
        WCHAR title[80], info[300], name[NAME_CHARS];
        seat = (int)lp;
        seat_label(seat, name, NAME_CHARS);
        _snwprintf(title, 80, L"%ls settings", name);
        SetWindowTextW(dlg, title);
        SetDlgItemTextW(dlg, IDC_WP_NAME, g_names[seat]);
        SendDlgItemMessageW(dlg, IDC_WP_NAME, EM_LIMITTEXT, NAME_CHARS - 1, 0);
        int scr = seat_first_screen(seat);
        _snwprintf(info, 300, L"Screen: %ls\r\nKeyboard: %ls   Mouse: %ls",
                   scr >= 0 ? (g_screens[scr].name[0] ? g_screens[scr].name : L"yes") : L"none yet",
                   seat_has(seat, DEV_KEYBOARD) ? L"yes" : L"none yet", seat_has(seat, DEV_MOUSE) ? L"yes" : L"none yet");
        SetDlgItemTextW(dlg, IDC_WP_INFO, info);
        for (int i = 0; i < 4; i++) {
            CheckDlgButton(dlg, boxes[i], (g_share[seat] & (1 << i)) ? BST_CHECKED : BST_UNCHECKED);
            ShowWindow(GetDlgItem(dlg, boxes[i]), seat > 0 ? SW_SHOW : SW_HIDE); /* workplace 1 is the main desktop */
        }
        ShowWindow(GetDlgItem(dlg, IDC_WP_SHARE_LABEL), seat > 0 ? SW_SHOW : SW_HIDE);
        return TRUE;
    }
    if (msg != WM_COMMAND) return FALSE;
    if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
    if (LOWORD(wp) != IDOK) return FALSE;
    WCHAR name[NAME_CHARS];
    GetDlgItemTextW(dlg, IDC_WP_NAME, name, NAME_CHARS);
    for (WCHAR *c = name; *c; c++) if (*c < 32 || *c == L'|' || *c == L'[' || *c == L']') *c = L' ';
    lstrcpynW(g_names[seat], name, NAME_CHARS);
    if (seat > 0) {
        int flags = 0;
        for (int i = 0; i < 4; i++) if (IsDlgButtonChecked(dlg, boxes[i]) == BST_CHECKED) flags |= 1 << i;
        g_share[seat] = flags;
    }
    EndDialog(dlg, IDOK);
    return TRUE;
}

static void edit_workplace(int seat)
{
    if (DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_WORKPLACE), g_wnd, workplace_proc, (LPARAM)seat) == IDOK) {
        g_startError[seat][0] = 0;
        lstrcpynW(g_status, want_running(seat) ? L"Settings changed. Not saved yet; Save setup applies them to the running workplace."
                                               : L"Settings changed. Not saved yet.", 300);
        changed();
    }
}

/* ---- named saved setups ------------------------------------------------------ */

static WCHAR g_nameBuf[80];

static INT_PTR CALLBACK name_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INITDIALOG) {
        SetWindowTextW(dlg, lp ? L"Rename setup" : L"Save setup");
        SetDlgItemTextW(dlg, IDC_NAME_PROMPT, lp ? L"New name for this saved setup:"
                                                 : L"Name for this setup. It is saved as a new setup; older ones are kept.");
        SetDlgItemTextW(dlg, IDOK, lp ? L"Rename" : L"Save");
        SetDlgItemTextW(dlg, IDC_NAME_EDIT, g_nameBuf);
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_LIMITTEXT, 79, 0);
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_SETSEL, 0, -1);
        return TRUE;
    }
    if (msg != WM_COMMAND) return FALSE;
    if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
    if (LOWORD(wp) != IDOK) return FALSE;
    GetDlgItemTextW(dlg, IDC_NAME_EDIT, g_nameBuf, 80);
    for (WCHAR *c = g_nameBuf; *c; c++) if (*c < 32) *c = L' ';
    WCHAR *t = g_nameBuf;
    while (*t == L' ') t++;
    if (!*t) { MessageBoxW(dlg, L"Please type a name.", L"VISION", MB_OK | MB_ICONINFORMATION); return TRUE; }
    memmove(g_nameBuf, t, (wcslen(t) + 1) * sizeof(WCHAR));
    EndDialog(dlg, IDOK);
    return TRUE;
}

/* Save setup: keeps the current state of every workplace as a new named setup. */
static void save_named(void)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf(g_nameBuf, 80, L"Setup %04u-%02u-%02u %02u.%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    if (DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_NAME), g_wnd, name_proc, 0) != IDOK) return;
    if (!save_all()) { MessageBoxW(g_wnd, L"The setup could not be saved.", L"VISION", MB_OK | MB_ICONERROR); return; }
    int id = save_snapshot(g_nameBuf);
    if (!id) { MessageBoxW(g_wnd, L"The setup could not be saved.", L"VISION", MB_OK | MB_ICONERROR); return; }
    _snwprintf(g_status, 300, L"Setup saved successfully: \"%ls\" (ID %d).", g_nameBuf, id);
    g_status[299] = 0;
    g_savedOk = TRUE;
    update_buttons();
    InvalidateRect(g_wnd, NULL, FALSE);
}

static void start_workplaces(void);
static Snapshot *g_snaps;
static int g_snapCount;

static void fill_setup_list(HWND dlg, int selectId)
{
    HWND list = GetDlgItem(dlg, IDC_SETUP_LIST);
    ListView_DeleteAllItems(list);
    g_snapCount = list_snapshots(g_snaps, MAX_SNAPSHOTS);
    int select = 0;
    for (int i = 0; i < g_snapCount; i++) {
        WCHAR id[16], count[8];
        _snwprintf(id, 16, L"%d", g_snaps[i].id);
        _snwprintf(count, 8, L"%d", g_snaps[i].workplaces);
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = id;
        it.lParam = g_snaps[i].id;
        ListView_InsertItem(list, &it);
        ListView_SetItemText(list, i, 1, g_snaps[i].name);
        ListView_SetItemText(list, i, 2, g_snaps[i].saved);
        ListView_SetItemText(list, i, 3, count);
        if (g_snaps[i].id == selectId) select = i;
    }
    ShowWindow(GetDlgItem(dlg, IDC_SETUP_EMPTY), g_snapCount ? SW_HIDE : SW_SHOW);
    for (int b = IDC_SETUP_OPEN; b <= IDC_SETUP_START; b++)
        if (b != IDC_SETUP_EMPTY) EnableWindow(GetDlgItem(dlg, b), g_snapCount > 0);
    SetDlgItemTextW(dlg, IDC_SETUP_DETAILS, L"");
    if (g_snapCount) ListView_SetItemState(list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

static int selected_setup(HWND dlg, int *index)
{
    int i = ListView_GetNextItem(GetDlgItem(dlg, IDC_SETUP_LIST), -1, LVNI_SELECTED);
    if (index) *index = i;
    return (i >= 0 && i < g_snapCount) ? g_snaps[i].id : 0;
}

static INT_PTR CALLBACK setups_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INITDIALOG) {
        HWND list = GetDlgItem(dlg, IDC_SETUP_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);
        static const WCHAR *cols[] = { L"ID", L"Name", L"Saved", L"Workplaces" };
        static const int widths[] = { 34, 150, 118, 70 };
        for (int c = 0; c < 4; c++) {
            LVCOLUMNW col;
            ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPWSTR)cols[c];
            col.cx = MulDiv(widths[c], g_dpi, 96);
            ListView_InsertColumn(list, c, &col);
        }
        fill_setup_list(dlg, 0);
        return TRUE;
    }
    if (msg == WM_NOTIFY) {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == IDC_SETUP_LIST && n->code == LVN_ITEMCHANGED) {
            int id = selected_setup(dlg, NULL);
            WCHAR *text = calloc(16384, sizeof(WCHAR));
            if (text) {
                if (id) describe_snapshot(id, text, 16384);
                SetDlgItemTextW(dlg, IDC_SETUP_DETAILS, text);
                free(text);
            }
        } else if (n->idFrom == IDC_SETUP_LIST && n->code == NM_DBLCLK) {
            PostMessageW(dlg, WM_COMMAND, IDC_SETUP_START, 0); /* double-click: open and start */
        }
        return FALSE;
    }
    if (msg != WM_COMMAND) return FALSE;
    int index, id = selected_setup(dlg, &index);
    switch (LOWORD(wp)) {
    case IDCANCEL:
        EndDialog(dlg, 0);
        return TRUE;
    case IDC_SETUP_OPEN:
    case IDC_SETUP_START:
        if (!id) return TRUE;
        if (g_dirty && MessageBoxW(dlg, L"The changes you have not saved will be replaced by this setup. Open it anyway?",
                                   L"VISION", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return TRUE;
        EndDialog(dlg, LOWORD(wp) == IDC_SETUP_START ? id | 0x100000 : id); /* flag: start it as well */
        return TRUE;
    case IDC_SETUP_RENAME:
        if (!id) return TRUE;
        lstrcpynW(g_nameBuf, g_snaps[index].name, 80);
        if (DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_NAME), dlg, name_proc, 1) == IDOK) {
            rename_snapshot(id, g_nameBuf);
            fill_setup_list(dlg, id);
        }
        return TRUE;
    case IDC_SETUP_DELETE: {
        if (!id) return TRUE;
        WCHAR q[200];
        _snwprintf(q, 200, L"Delete the saved setup \"%ls\" (ID %d)? This cannot be undone.", g_snaps[index].name, id);
        if (MessageBoxW(dlg, q, L"VISION", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            delete_snapshot(id);
            fill_setup_list(dlg, 0);
        }
        return TRUE;
    }
    }
    return FALSE;
}

static void show_saved_setups(void)
{
    Snapshot *all = calloc(MAX_SNAPSHOTS, sizeof(Snapshot));
    if (!all) return;
    g_snaps = all;
    int id = (int)DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_SETUPS), g_wnd, setups_proc, 0);
    BOOL andStart = id > 0 && (id & 0x100000);
    id &= 0xFFFFF;
    WCHAR name[80] = L"";
    for (int i = 0; i < g_snapCount; i++) if (g_snaps[i].id == id) lstrcpynW(name, g_snaps[i].name, 80);
    g_snaps = NULL;
    free(all);
    if (id <= 0) return;
    if (!open_snapshot(id)) { MessageBoxW(g_wnd, L"That setup could not be opened.", L"VISION", MB_OK | MB_ICONERROR); return; }
    /* Use it exactly as saved: no automatic changes on top. */
    for (int s = 0; s < MAX_SEATS; s++) g_startError[s][0] = 0;
    g_dirty = FALSE;
    layout();
    update_buttons();
    _snwprintf(g_status, 300, L"Opened \"%ls\" (ID %d) exactly as it was saved.", name, id);
    g_status[299] = 0;
    InvalidateRect(g_wnd, NULL, FALSE);
    if (andStart) start_workplaces(); /* start its workplaces straight away, with its saved settings */
}

static void start_workplaces(void)
{
    WCHAR msg[300];
    for (int s = 1; s < g_seatCount; s++) {
        msg[0] = 0;
        if ((g_share[s] & SHARE_ON) && seat_screens(s) == 0)
            _snwprintf(msg, 300, L"Workplace %d needs a screen first. Click a screen and choose Workplace %d.", s + 1, s + 1);
        if (msg[0]) { set_status(msg); MessageBeep(MB_ICONWARNING); return; }
    }
    if (g_dirty && !save_all()) {
        MessageBoxW(g_wnd, L"The setup could not be saved.", L"VISION", MB_OK | MB_ICONERROR);
        return;
    }
    if (!is_server_os()) { /* Windows 10/11: start every workplace that has a screen, one by one */
        int started = 0, refused = 0;
        for (int s = 0; s < g_seatCount; s++) {
            WCHAR why[200];
            if (seat_first_screen(s) < 0) continue;
            if (want_running(s)) { started++; continue; }
            if (seat_problem(s, why, 200)) { lstrcpynW(g_startError[s], why, 200); refused++; continue; }
            start_one(s);
            started++;
        }
        if (refused) set_status(L"Some workplaces could not start. Their cards say why.");
        InvalidateRect(g_wnd, NULL, FALSE);
        return;
    }
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.hwnd = g_wnd;
    /* Windows Server: a real desktop per workplace.  Windows 10/11 have one
     * desktop for the whole PC, so VISION keeps each workplace's windows on its
     * own screen instead. */
    /* Windows Server: every workplace signs in to this PC's Windows by itself,
     * like a separate PC.  Windows 10/11 allow one sign-in at a time, so there
     * all workplaces share it and VISION keeps them apart. */
    BOOL server = is_server_os();
    info.lpVerb = server ? L"runas" : L"open";
    info.lpFile = self;
    info.lpParameters = server ? L"--start" : L"--keep-apart";
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) {
        set_status(L"The workplaces are running. Changes you save are used straight away.");
    } else {
        set_status(GetLastError() == ERROR_CANCELLED ? L"Start was cancelled: VISION needs administrator permission."
                                                     : L"Could not start the workplaces.");
    }
    update_buttons();
    InvalidateRect(g_wnd, NULL, FALSE);
}

/* Look for screens and devices again, keeping the choices made so far. */
static void refresh_hardware(BOOL automatic)
{
    Screen *oldScreens = malloc(sizeof(g_screens));
    Device *oldDevices = malloc(sizeof(g_devices));
    int oldS = g_screenCount, oldD = g_deviceCount, moreScreens = 0;
    if (!oldScreens || !oldDevices) { free(oldScreens); free(oldDevices); return; }
    memcpy(oldScreens, g_screens, sizeof(g_screens));
    memcpy(oldDevices, g_devices, sizeof(g_devices));
    find_hardware();
    for (int i = 0; i < g_screenCount; i++) {
        BOOL known = FALSE;
        g_screens[i].seat = 0;
        for (int j = 0; j < oldS; j++)
            if (lstrcmpiW(g_screens[i].key, oldScreens[j].key) == 0) { g_screens[i].seat = oldScreens[j].seat; known = TRUE; }
        if (!known) moreScreens++;
    }
    for (int i = 0; i < g_deviceCount; i++) {
        g_devices[i].seat = 0;
        for (int j = 0; j < oldD; j++)
            if (lstrcmpiW(g_devices[i].key, oldDevices[j].key) == 0) g_devices[i].seat = oldDevices[j].seat;
    }
    free(oldScreens);
    free(oldDevices);
    int separated = separate_shared_screens();
    if (separated) g_dirty = TRUE;
    layout();
    update_buttons();
    if (separated)
        _snwprintf(g_status, 300, L"New screen connected and given its own workplace. Press Save setup to keep it.");
    else if (automatic && g_idleCount > 0 && !moreScreens)
        _snwprintf(g_status, 300, L"A screen was connected. Press Turn on in the screen list.");
    else if (automatic && moreScreens)
        _snwprintf(g_status, 300, L"New screen connected. Click it above to give it to a workplace.");
    else if (automatic && g_screenCount < oldS)
        _snwprintf(g_status, 300, L"A screen was disconnected.");
    else if (!automatic)
        _snwprintf(g_status, 300, L"Found %d screen(s) and %d keyboard/mouse device(s).", g_screenCount, g_deviceCount);
    InvalidateRect(g_wnd, NULL, FALSE);
}

/* Download and install the update; VISION restarts afterwards. */
static void start_update(void)
{
    if (g_dirty) {
        int answer = MessageBoxW(g_wnd, L"VISION restarts to update. Save your changes first?", L"VISION",
                                 MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) return;
        if (answer == IDYES && !save_all()) { MessageBoxW(g_wnd, L"The setup could not be saved.", L"VISION", MB_OK | MB_ICONERROR); return; }
        if (answer == IDNO) g_dirty = FALSE;
    }
    g_update = UP_DOWNLOADING;
    g_updatePct = 0;
    g_updateCountdown = 0;
    KillTimer(g_wnd, 7);
    g_updateBlink = FALSE;
    update_button();
    InvalidateRect(g_wnd, NULL, FALSE);
    update_download(g_wnd);
}

static void update_button(void)
{
    const UpdateInfo *u = update_found();
    WCHAR label[80];
    if (g_update == UP_FOUND && u) _snwprintf(label, 80, L"%ls Update to %hs", g_updateBlink ? L"\x25CF" : L"\x25CB", u->version);
    else if (g_update == UP_DOWNLOADING || g_update == UP_INSTALLING) lstrcpynW(label, L"Updating\x2026", 80);
    else if (g_checkingByHand) lstrcpynW(label, L"Checking\x2026", 80);
    else lstrcpynW(label, L"Check for updates", 80);
    label[79] = 0;
    SetWindowTextW(g_btnUpdate, label);
    EnableWindow(g_btnUpdate, !g_demo && g_update != UP_DOWNLOADING && g_update != UP_INSTALLING && !g_checkingByHand);
}

static HFONT make_font(int points, int weight)
{
    LOGFONTW lf = { 0 };
    lf.lfHeight = -MulDiv(points, g_dpi, 72);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

static HWND make_button(const WCHAR *label, int id, BOOL primary)
{
    HWND b = CreateWindowW(L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                           0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_fText, TRUE);
    return b;
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_wnd = wnd;
        HDC dc = GetDC(wnd);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(wnd, dc);
        g_fTitle = make_font(17, FW_SEMIBOLD);
        g_fHead = make_font(12, FW_SEMIBOLD);
        g_fText = make_font(10, FW_NORMAL);
        g_fBold = make_font(10, FW_SEMIBOLD);
        g_fSmall = make_font(9, FW_NORMAL);
        g_fHuge = make_font(26, FW_BOLD);
        g_btnFind = make_button(L"Find a device", ID_FIND, FALSE);
        g_btnAuto = make_button(L"Automatic setup", ID_AUTO, FALSE);
        g_btnUpdate = make_button(L"Check for updates", ID_UPDATE, FALSE);
        g_chkAutoUpdate = CreateWindowW(L"BUTTON", L"Update automatically", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                        0, 0, 10, 10, wnd, (HMENU)(INT_PTR)ID_AUTOUPDATE, GetModuleHandleW(NULL), NULL);
        SendMessageW(g_chkAutoUpdate, WM_SETFONT, (WPARAM)g_fSmall, TRUE);
        SendMessageW(g_chkAutoUpdate, BM_SETCHECK, update_auto() ? BST_CHECKED : BST_UNCHECKED, 0);
        g_btnSetups = make_button(L"Saved setups", ID_SETUPS, FALSE);
        SetTimer(wnd, 4, 700, NULL); /* keep each workplace's status up to date */
        if (!g_demo) {
            watch_devices();
            update_check(wnd);                       /* look for a newer VISION now ... */
            SetTimer(wnd, 5, 6 * 60 * 60 * 1000, NULL); /* ... and every six hours */
            /* Carry on with workplaces that were running before (e.g. after an update). */
            BOOL wanted = FALSE;
            for (int s = 0; s < g_seatCount; s++) wanted |= want_running(s);
            if (wanted && !is_server_os() && !engine_running()) launch_engine();
        }
        g_btnRefresh = make_button(L"Refresh", ID_REFRESH, FALSE);
        g_btnSave = make_button(L"Save setup", ID_SAVE, FALSE); /* not the default button: Enter must not save by accident */
        g_btnAdd = make_button(L"Add a workplace", ID_ADD, FALSE);
        g_btnRemove = make_button(L"Remove last", ID_REMOVE, FALSE);
        g_btnStart = make_button(L"Start workplaces", ID_START, FALSE);
        g_btnSharing = make_button(L"Connections and sharing", ID_SHARING, FALSE);
        find_hardware();
        load_setup();
        {
            WCHAR path[MAX_PATH];
            setup_file(g_demo ? L"demo-setup.ini" : L"setup.ini", path, MAX_PATH);
            BOOL firstRun = GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES;
            int changes = firstRun ? one_workplace_per_screen() : separate_shared_screens();
            /* First time with several screens: offer to set everything up in one go. */
            if (firstRun && !g_demo && g_screenCount >= 2) PostMessageW(wnd, WM_FIRST_RUN, 0, 0);
            if (changes && firstRun && !g_demo && save_all()) { /* the starting layout is kept straight away */
                _snwprintf(g_status, 300, L"Each screen has its own workplace.");
            } else if (changes) {
                g_dirty = TRUE;
                _snwprintf(g_status, 300, L"Each screen has its own workplace. Press Save setup to keep this.");
            }
        }
        update_buttons();
        return 0;
    }
    case WM_SIZE:
        layout();
        InvalidateRect(wnd, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(980);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(700);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(wnd, &ps);
        GetClientRect(wnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int index, kind = hit_test(pt, &index);
        if (kind != g_hoverKind || index != g_hoverIndex) {
            g_hoverKind = kind;
            g_hoverIndex = index;
            InvalidateRect(wnd, NULL, FALSE);
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, wnd, 0 };
            TrackMouseEvent(&track);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hoverKind = HIT_NONE;
        InvalidateRect(wnd, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == wnd && g_hoverKind != HIT_NONE && !(g_hoverKind == HIT_UPDATE && g_update != UP_FOUND && g_update != UP_FAILED)) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int index, kind = hit_test(pt, &index);
        if (kind == HIT_UPDATE) { start_update(); return 0; }
        if (kind == HIT_SEAT_START) {
            WCHAR why[200];
            int state = seat_state(index, why, 200);
            if (state == WS_STARTING) return 0;                      /* already on its way */
            if (want_running(index)) stop_one(index); else start_one(index);
            return 0;
        }
        if (kind == HIT_IDLE) {
            if (extend_displays()) {
                set_status(L"Turning the screen on\x2026 It appears above with its own workplace.");
                SetTimer(wnd, 3, 1500, NULL);
            } else {
                set_status(L"Windows could not turn it on. Press the Windows key + P and choose Extend.");
            }
        } else if (kind == HIT_SEAT) {
            edit_workplace(index);
        } else if (kind != HIT_NONE) {
            BOOL inRow = kind == HIT_SCREEN && PtInRect(&g_screenRow[index], pt);
            RECT r = inRow ? g_screenRow[index] : kind == HIT_SCREEN ? g_screens[index].draw : g_devices[index].draw;
            POINT at = { inRow ? r.right - S(128) : r.left,
                         r.bottom - (kind == HIT_SCREEN && !inRow ? 0 : S(inRow ? 5 : 8)) };
            ClientToScreen(wnd, &at);
            choose_seat(kind, index, at);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT at = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(wnd, &at);
        if (PtInRect(&g_rcScreens, at)) {
            g_screenScroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(SCREEN_ROW_H) / WHEEL_DELTA;
            InvalidateRect(wnd, &g_rcScreens, FALSE);
            return 0;
        }
        g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * (S(TILE_H) + S(TILE_GAP)) / WHEEL_DELTA;
        layout();
        InvalidateRect(wnd, &g_rcDevices, FALSE);
        return 0;
    }
    case WM_INPUT:
        on_raw_input((HRAWINPUT)lp);
        break; /* DefWindowProc must still clean up */
    case WM_TIMER: {
        BOOL any = FALSE;
        DWORD now = GetTickCount();
        if (wp == 2) { demo_find_tick(); return 0; }
        if (wp == 3) { KillTimer(wnd, 3); if (!g_demo) refresh_hardware(TRUE); return 0; }
        if (wp == 4) { InvalidateRect(wnd, &g_rcSeats, FALSE); return 0; }
        if (wp == 5) { if (g_update == UP_NONE) update_check(wnd); return 0; }
        if (wp == 7) {
            if (g_update != UP_FOUND || g_dirty) { g_updateCountdown = 0; KillTimer(wnd, 7); InvalidateRect(wnd, &g_rcNote, FALSE); return 0; }
            if (--g_updateCountdown <= 0) { KillTimer(wnd, 7); start_update(); return 0; }
            InvalidateRect(wnd, &g_rcNote, FALSE);
            return 0;
        }
        if (wp == 6) {
            if (g_update != UP_FOUND) { KillTimer(wnd, 6); g_updateBlink = FALSE; update_button(); return 0; }
            /* Waiting because of unsaved changes: once saved, update by itself after all. */
            if (update_auto() && !g_dirty && !g_updateCountdown) { g_updateCountdown = 10; SetTimer(wnd, 7, 1000, NULL); }
            g_updateBlink = !g_updateBlink;
            update_button();
            InvalidateRect(wnd, &g_rcNote, FALSE);
            return 0;
        }
        for (int i = 0; i < g_deviceCount; i++) {
            if (!g_devices[i].flashUntil) continue;
            if ((int)(g_devices[i].flashUntil - now) <= 0) g_devices[i].flashUntil = 0;
            else any = TRUE;
        }
        if (!any) KillTimer(wnd, 1);
        InvalidateRect(wnd, &g_rcDevices, FALSE); /* redraw: the device in use blinks */
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDCANCEL && g_wizSeat >= 0) { wizard_skip(); return 0; } /* Esc during automatic setup */
        if (id >= ID_MENU_SEAT && id < ID_MENU_SEAT + MAX_SEATS) { assign(g_menuKind, g_menuIndex, id - ID_MENU_SEAT); return 0; }
        switch (id) {
        case ID_MENU_NONE:
            assign(g_menuKind, g_menuIndex, NOT_USED);
            return 0;
        case ID_ADD:
            if (g_seatCount < MAX_SEATS) {
                g_seatCount++;
                _snwprintf(g_status, 300, L"Workplace %d added. Now click a screen, a keyboard and a mouse to give them to it.", g_seatCount);
                changed();
            }
            return 0;
        case ID_REMOVE:
            remove_last_seat();
            return 0;
        case ID_AUTOUPDATE: {
            BOOL on = SendMessageW(g_chkAutoUpdate, BM_GETCHECK, 0, 0) == BST_CHECKED;
            update_set_auto(on);
            if (!on && g_updateCountdown) { g_updateCountdown = 0; KillTimer(wnd, 7); InvalidateRect(wnd, &g_rcNote, FALSE); }
            set_status(on ? L"Updates will install by themselves." : L"Updates wait until you click Update.");
            return 0;
        }
        case ID_UPDATE:
            if (g_update == UP_FOUND || g_update == UP_FAILED) { start_update(); update_button(); }
            else if (g_update == UP_NONE) { g_checkingByHand = TRUE; update_button(); update_check(wnd); }
            return 0;
        case ID_AUTO:
            wizard_start();
            return 0;
        case ID_FIND:
            if (g_demo) {
                g_finding = !g_finding;
                if (g_finding) SetTimer(wnd, 2, 1800, NULL); else KillTimer(wnd, 2);
                set_status(g_finding ? L"Demo: pretending someone uses each device in turn." : L"");
                update_buttons();
            } else {
                set_finding(!g_finding);
            }
            return 0;
        case ID_REFRESH:
            refresh_hardware(FALSE);
            return 0;
        case ID_SHARING:
            edit_sharing();
            return 0;
        case ID_START:
            start_workplaces();
            return 0;
        case ID_SAVE:
            save_named();
            return 0;
        case ID_SETUPS:
            show_saved_setups();
            return 0;
        }
        break;
    }
    case WM_DISPLAYCHANGE:
    case WM_DEVICECHANGE:
        /* A screen, keyboard or mouse was plugged in or removed.  Windows sends
         * several of these in a row, so wait a moment and look once. */
        if (msg == WM_DISPLAYCHANGE || wp == DBT_DEVNODES_CHANGED) SetTimer(wnd, 3, 900, NULL);
        break;
    case WM_FIRST_RUN: {
        WCHAR q[400];
        _snwprintf(q, 400,
                   L"VISION found %d screens.\n\n"
                   L"Set up a workplace for every screen now? Each screen gets the PC's desktop icons and files, and "
                   L"you press a key and click on each screen to give it its own keyboard and mouse. The workplaces "
                   L"start straight away and every time Windows starts.",
                   g_screenCount);
        if (MessageBoxW(wnd, q, L"Welcome to VISION", MB_YESNO | MB_ICONQUESTION) == IDYES) wizard_start();
        else set_status(L"You can set the workplaces up at any time with Automatic setup.");
        return 0;
    }
    case WM_UPDATE_FOUND:
        g_checkingByHand = FALSE;
        if (g_update == UP_NONE || g_update == UP_FAILED) {
            g_update = UP_FOUND;
            SetTimer(wnd, 6, 600, NULL); /* blink the update bar and button */
            /* Update automatically: count down, unless there are unsaved changes to ask about. */
            if (update_auto() && !g_dirty) { g_updateCountdown = 10; SetTimer(wnd, 7, 1000, NULL); }
            InvalidateRect(wnd, NULL, FALSE);
        }
        update_button();
        return 0;
    case WM_UPDATE_NONE:
        if (g_checkingByHand) {
            g_checkingByHand = FALSE;
            set_status(wp ? L"Could not reach the update site. Check the internet connection and try again."
                          : L"VISION is up to date (version " TEXT(VISION_VERSION) L").");
        }
        update_button();
        return 0;
    case WM_UPDATE_PROGRESS:
        g_updatePct = (int)wp;
        InvalidateRect(wnd, &g_rcNote, FALSE);
        return 0;
    case WM_UPDATE_FAILED:
        g_update = UP_FAILED;
        update_button();
        lstrcpynW(g_updateWhy, (const WCHAR *)wp, 200);
        InvalidateRect(wnd, NULL, FALSE);
        return 0;
    case WM_UPDATE_READY:
        g_update = UP_INSTALLING;
        InvalidateRect(wnd, NULL, FALSE);
        UpdateWindow(wnd);
        if (update_run((const WCHAR *)wp, NULL)) {
            DestroyWindow(wnd); /* the setup replaces VISION and opens it again */
        } else {
            g_update = UP_FAILED;
            lstrcpynW(g_updateWhy, GetLastError() == ERROR_CANCELLED ? L"Windows was not allowed to install it"
                                                                      : L"the setup could not be started", 200);
            InvalidateRect(wnd, NULL, FALSE);
        }
        return 0;
    case WM_CLOSE:
        if (confirm_close()) DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    BOOL test = cmd && wcsstr(cmd, L"--test") != NULL;
    if (cmd && wcsstr(cmd, L"--relaunch")) return update_relaunch(cmd);
    if (cmd && wcsstr(cmd, L"--agent")) return run_agent(inst, test);
    if (cmd && wcsstr(cmd, L"--start")) return run_controller(inst, test, wcsstr(cmd, L"--auto") != NULL);
    if (cmd && wcsstr(cmd, L"--keep-apart")) return run_keep_apart(inst, test, wcsstr(cmd, L"--auto") != NULL);
    g_demo = cmd && wcsstr(cmd, L"--demo") != NULL;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW cls;
    ZeroMemory(&cls, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = wnd_proc;
    cls.hInstance = inst;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    cls.lpszClassName = L"VISIONMain";
    if (!RegisterClassExW(&cls)) return 1;

    HDC screen = GetDC(NULL);
    int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    HWND wnd = CreateWindowExW(0, cls.lpszClassName, L"VISION", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1060, dpi, 96), MulDiv(760, dpi, 96),
                               NULL, NULL, inst, NULL);
    if (!wnd) return 1;
    ShowWindow(wnd, show);
    UpdateWindow(wnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
