/*
 * OpenSeat for Windows - plan who uses which screen, keyboard and mouse.
 *
 * This program lets you divide the screens, keyboards and mice of one PC
 * between several "workplaces" and saves that plan.  It does NOT yet run the
 * extra workplaces: that needs kernel drivers (see ../DESIGN.md).
 *
 * Build: see build.sh (MinGW-w64) in this folder.
 */

#include "common.h"
#include "resource.h"

#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wincred.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int run_controller(HINSTANCE inst, BOOL test);
int run_agent(HINSTANCE inst, BOOL test);

enum { ID_ADD = 1, ID_REMOVE, ID_FIND, ID_REFRESH, ID_SAVE, ID_START, ID_MENU_SEAT = 1000, ID_MENU_NONE = 1099 };
enum { HIT_NONE, HIT_SCREEN, HIT_DEVICE, HIT_SEAT };

static const COLORREF SEAT_COLORS[MAX_SEATS] = {
    RGB(37, 99, 205), RGB(214, 110, 20), RGB(34, 139, 84), RGB(140, 72, 190),
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
static HWND g_wnd, g_btnAdd, g_btnRemove, g_btnFind, g_btnRefresh, g_btnSave, g_btnStart;
static HFONT g_fTitle, g_fHead, g_fText, g_fBold, g_fSmall, g_fHuge;
static WCHAR g_status[300] = L"";
static int g_hoverKind, g_hoverIndex, g_menuKind, g_menuIndex, g_scroll;
static RECT g_rcScreens, g_rcDevices, g_rcSeats, g_rcBar;
static RECT g_seatDraw[MAX_SEATS];

static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ---- what each workplace has -------------------------------------------- */

static BOOL seat_ready(int seat)
{
    if (seat == 0) return seat_screens(0) > 0 && seat_has(0, DEV_KEYBOARD) && seat_has(0, DEV_MOUSE);
    /* An extra workplace's keyboard and mouse can also be picked at start by pressing them. */
    return seat_screens(seat) > 0 && g_accounts[seat][0];
}

static BOOL save_all(void)
{
    if (!save_setup()) return FALSE;
    g_dirty = FALSE;
    return TRUE;
}

static void seat_label(int seat, WCHAR *out, size_t chars)
{
    if (seat == NOT_USED) lstrcpynW(out, L"Not used", (int)chars);
    else _snwprintf(out, chars, L"Workplace %d", seat + 1);
}

static COLORREF seat_color(int seat) { return seat == NOT_USED ? COL_UNUSED : SEAT_COLORS[seat]; }

static COLORREF tint(COLORREF c, int percentWhite)
{
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    return RGB(r + (255 - r) * percentWhite / 100, g + (255 - g) * percentWhite / 100, b + (255 - b) * percentWhite / 100);
}

static void set_status(const WCHAR *text)
{
    lstrcpynW(g_status, text, 300);
    InvalidateRect(g_wnd, &g_rcBar, FALSE);
}

static void update_buttons(void)
{
    EnableWindow(g_btnAdd, g_seatCount < MAX_SEATS);
    EnableWindow(g_btnRemove, g_seatCount > 1);
    EnableWindow(g_btnSave, g_dirty);
    EnableWindow(g_btnStart, !g_demo && g_seatCount > 1);
    SetWindowTextW(g_btnFind, g_finding ? L"Stop finding" : L"Find a device");
}

static void changed(void)
{
    g_dirty = TRUE;
    update_buttons();
    InvalidateRect(g_wnd, NULL, FALSE);
}

/* ---- layout -------------------------------------------------------------- */

#define ROW_H 46

static int device_rows_height(void) { return g_deviceCount * S(ROW_H); }

static void layout(void)
{
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int margin = S(20), top = S(128), barH = S(60), sideW = S(310);
    int bottom = rc.bottom - barH - S(14);
    int leftW = rc.right - sideW - margin * 3;
    int screensH = (bottom - top) * 37 / 100;

    SetRect(&g_rcBar, 0, rc.bottom - barH, rc.right, rc.bottom);
    SetRect(&g_rcScreens, margin, top, margin + leftW, top + screensH);
    SetRect(&g_rcDevices, margin, top + screensH + S(14), margin + leftW, bottom);
    SetRect(&g_rcSeats, rc.right - margin - sideW, top, rc.right - margin, bottom);

    int by = rc.bottom - barH + S(14), bh = S(32);
    MoveWindow(g_btnFind, margin, by, S(130), bh, TRUE);
    MoveWindow(g_btnRefresh, margin + S(140), by, S(100), bh, TRUE);
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

static void paint_screens(HDC dc)
{
    RECT area = g_rcScreens, stage;
    section_title(dc, area, 1, L"Screens", L"Click a screen to choose which workplace uses it.");
    SetRect(&stage, area.left, area.top + S(56), area.right, area.bottom);
    box(dc, stage, S(10), COL_PANEL, COL_LINE, 1);
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
        text(dc, g_fHuge, color, big, DT_CENTER | DT_VCENTER | DT_SINGLELINE, line);
        seat_label(s->seat, label, 32);
        text(dc, g_fBold, color, mid, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, label);
        _snwprintf(line, 64, L"%ld \x00D7 %ld%ls", s->area.right - s->area.left, s->area.bottom - s->area.top,
                   s->primary ? L"  \x00B7  main" : L"");
        text(dc, g_fSmall, COL_MUTED, low, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS, line);
    }
}

static void paint_device_icon(HDC dc, RECT rc, int kinds, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, S(2), color);
    HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
    if (kinds & DEV_KEYBOARD) {
        int w = S(kinds & DEV_MOUSE ? 18 : 26), x = (kinds & DEV_MOUSE) ? rc.left : cx - w / 2;
        RoundRect(dc, x, cy - S(8), x + w, cy + S(8), S(4), S(4));
        MoveToEx(dc, x + S(5), cy + S(3), NULL);
        LineTo(dc, x + w - S(5), cy + S(3));
        MoveToEx(dc, x + S(5), cy - S(3), NULL);
        LineTo(dc, x + w - S(5), cy - S(3));
    }
    if (kinds & DEV_MOUSE) {
        int x = (kinds & DEV_KEYBOARD) ? rc.right - S(10) : cx - S(7), w = (kinds & DEV_KEYBOARD) ? S(10) : S(14);
        RoundRect(dc, x, cy - S(11), x + w, cy + S(11), w, w);
        MoveToEx(dc, x + w / 2, cy - S(7), NULL);
        LineTo(dc, x + w / 2, cy - S(2));
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

static void paint_devices(HDC dc)
{
    RECT area = g_rcDevices, list;
    section_title(dc, area, 2, L"Keyboards and mice",
                  L"Click a device to choose its workplace. Not sure which one it is? Use \x201C" L"Find a device\x201D.");
    SetRect(&list, area.left, area.top + S(56), area.right, area.bottom);
    box(dc, list, S(10), COL_PANEL, COL_LINE, 1);
    if (g_deviceCount == 0) {
        text(dc, g_fText, COL_MUTED, list, DT_CENTER | DT_VCENTER | DT_SINGLELINE, L"No keyboards or mice found.");
        return;
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, list.left + 1, list.top + S(3), list.right - 1, list.bottom - S(3));
    DWORD now = GetTickCount();
    for (int i = 0; i < g_deviceCount; i++) {
        Device *d = &g_devices[i];
        WCHAR label[32];
        COLORREF color = seat_color(d->seat);
        BOOL hover = g_hoverKind == HIT_DEVICE && g_hoverIndex == i;
        BOOL flash = d->flashUntil && (int)(d->flashUntil - now) > 0;
        int y = list.top + S(3) + i * S(ROW_H) - g_scroll;
        SetRect(&d->draw, list.left + 1, y, list.right - 1, y + S(ROW_H));
        if (flash || hover) fill(dc, &d->draw, flash ? COL_FLASH : RGB(240, 244, 250));

        RECT bar = { d->draw.left + S(8), y + S(8), d->draw.left + S(13), y + S(ROW_H) - S(8) };
        fill(dc, &bar, color);
        RECT icon = { d->draw.left + S(24), y, d->draw.left + S(56), y + S(ROW_H) };
        paint_device_icon(dc, icon, d->kinds, COL_MUTED);

        int pillW = S(132);
        RECT pill = { d->draw.right - pillW - S(12), y + S(9), d->draw.right - S(12), y + S(ROW_H) - S(9) };
        RECT name = { d->draw.left + S(68), y + S(5), pill.left - S(10), y + S(25) };
        RECT kind = { name.left, y + S(24), name.right, y + S(ROW_H) - S(3) };
        text(dc, g_fBold, COL_TEXT, name, DT_SINGLELINE | DT_END_ELLIPSIS, d->name);
        text(dc, g_fSmall, COL_MUTED, kind, DT_SINGLELINE | DT_END_ELLIPSIS,
             flash ? L"This is the device you just used" : kind_name(d->kinds));

        box(dc, pill, S(8), tint(color, 86), color, 1);
        RECT pillText = { pill.left + S(10), pill.top, pill.right - S(22), pill.bottom };
        seat_label(d->seat, label, 32);
        text(dc, g_fText, color, pillText, DT_VCENTER | DT_SINGLELINE, label);
        POINT arrow[3] = { { pill.right - S(18), (pill.top + pill.bottom) / 2 - S(2) },
                           { pill.right - S(10), (pill.top + pill.bottom) / 2 - S(2) },
                           { pill.right - S(14), (pill.top + pill.bottom) / 2 + S(3) } };
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, arrow, 3);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);

        if (i < g_deviceCount - 1) {
            RECT line = { d->draw.left + S(8), y + S(ROW_H) - 1, d->draw.right - S(8), y + S(ROW_H) };
            fill(dc, &line, RGB(236, 238, 241));
        }
    }
    RestoreDC(dc, saved);
}

static void paint_seats(HDC dc)
{
    RECT area = g_rcSeats;
    RECT title = { area.left, area.top, area.right, area.top + S(26) };
    RECT hint = { area.left, area.top + S(28), area.right, area.top + S(48) };
    text(dc, g_fHead, COL_TEXT, title, DT_VCENTER | DT_SINGLELINE, L"Workplaces");
    text(dc, g_fText, COL_MUTED, hint, DT_SINGLELINE, L"One workplace = one person.");

    int cardH = S(116), y = area.top + S(56);
    for (int seat = 0; seat < MAX_SEATS; seat++) SetRectEmpty(&g_seatDraw[seat]);
    for (int seat = 0; seat < g_seatCount; seat++, y += cardH + S(8)) {
        WCHAR label[40];
        COLORREF color = SEAT_COLORS[seat];
        BOOL ready = seat_ready(seat);
        RECT card = { area.left, y, area.right, y + cardH };
        g_seatDraw[seat] = card;
        box(dc, card, S(10), COL_PANEL, COL_LINE, 1);
        RECT stripe = { card.left + 1, card.top + S(10), card.left + S(6), card.bottom - S(10) };
        fill(dc, &stripe, color);

        RECT name = { card.left + S(16), card.top + S(8), card.right - S(110), card.top + S(32) };
        RECT state = { card.right - S(120), card.top + S(8), card.right - S(12), card.top + S(32) };
        seat_label(seat, label, 40);
        text(dc, g_fHead, color, name, DT_VCENTER | DT_SINGLELINE, label);
        text(dc, g_fBold, ready ? COL_OK : COL_WARN, state, DT_RIGHT | DT_VCENTER | DT_SINGLELINE,
             ready ? L"Ready" : L"Not complete");

        struct { const WCHAR *word; BOOL yes; } parts[3] = {
            { L"Screen", seat_screens(seat) > 0 },
            { L"Keyboard", seat_has(seat, DEV_KEYBOARD) },
            { L"Mouse", seat_has(seat, DEV_MOUSE) },
        };
        int x = card.left + S(16), partW = (card.right - card.left - S(28)) / 3;
        const WCHAR *missing = NULL;
        for (int p = 0; p < 3; p++, x += partW) {
            RECT word = { x + S(22), card.top + S(38), x + partW, card.top + S(58) };
            badge(dc, x, card.top + S(40), S(16), parts[p].yes);
            text(dc, g_fText, COL_TEXT, word, DT_VCENTER | DT_SINGLELINE, parts[p].word);
            if (!parts[p].yes && !missing) missing = parts[p].word;
        }
        WCHAR who[120];
        BOOL hasAccount = seat == 0 || g_accounts[seat][0];
        RECT acct = { card.left + S(38), card.top + S(62), card.right - S(12), card.top + S(82) };
        badge(dc, card.left + S(16), card.top + S(64), S(16), hasAccount);
        if (seat == 0) lstrcpynW(who, L"Signs in as: you", 120);
        else if (hasAccount) _snwprintf(who, 120, L"Signs in as: %ls  (click to change)", g_accounts[seat]);
        else lstrcpynW(who, L"Signs in as: \x2014  click to choose", 120);
        BOOL hover = g_hoverKind == HIT_SEAT && g_hoverIndex == seat;
        text(dc, hover ? g_fBold : g_fText, hasAccount ? COL_TEXT : COL_WARN, acct, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, who);

        RECT tip = { card.left + S(16), card.top + S(88), card.right - S(12), card.bottom - S(6) };
        WCHAR tipText[90];
        if (ready && seat > 0 && missing)
            lstrcpynW(tipText, L"Keyboard and mouse: picked by pressing them at start.", 90);
        else if (ready)
            lstrcpynW(tipText, seat == 0 ? L"This is the workplace you use now." : L"Has everything it needs.", 90);
        else if (missing && (seat == 0 || missing == parts[0].word))
            _snwprintf(tipText, 90, L"Still needs a %ls. Click one on the left.",
                       missing == parts[0].word ? L"screen" : missing == parts[1].word ? L"keyboard" : L"mouse");
        else
            lstrcpynW(tipText, L"Still needs a Windows account. Click the line above.", 90);
        text(dc, g_fSmall, COL_MUTED, tip, DT_SINGLELINE | DT_END_ELLIPSIS, tipText);
    }
}

static void paint(HDC dc)
{
    RECT rc;
    GetClientRect(g_wnd, &rc);
    fill(dc, &rc, COL_BG);

    RECT head = { 0, 0, rc.right, S(78) };
    fill(dc, &head, COL_PANEL);
    RECT title = { S(20), S(10), rc.right - S(20), S(44) };
    RECT sub = { S(20), S(46), rc.right - S(20), S(70) };
    text(dc, g_fTitle, COL_TEXT, title, DT_VCENTER | DT_SINGLELINE, g_demo ? L"Set up workplaces  (demo)" : L"Set up workplaces");
    text(dc, g_fText, COL_MUTED, sub, DT_SINGLELINE | DT_END_ELLIPSIS,
         L"Several people can share this PC. Give each person a screen, a keyboard and a mouse.");

    RECT note = { 0, S(78), rc.right, S(110) };
    fill(dc, &note, COL_NOTE_BG);
    RECT noteText = { S(20), S(78), rc.right - S(20), S(110) };
    text(dc, g_fText, COL_NOTE_TX, noteText, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
         g_demo ? L"Demo: these screens and devices are examples, not your real hardware."
         : is_server_os() ? L"Each extra workplace signs in to its own Windows account. When everything is ready, press Start workplaces."
                          : L"Note: Windows 10 and 11 allow only one person signed in at a time. Several people at once needs Windows Server.");

    paint_screens(dc);
    paint_devices(dc);
    paint_seats(dc);

    fill(dc, &g_rcBar, COL_PANEL);
    RECT line = { 0, g_rcBar.top, rc.right, g_rcBar.top + 1 };
    fill(dc, &line, COL_LINE);
    RECT status = { S(20) + S(256), g_rcBar.top, rc.right - S(20) - S(310), g_rcBar.bottom };
    text(dc, g_fText, g_dirty && !g_status[0] ? COL_WARN : COL_MUTED, status, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
         g_status[0] ? g_status : g_dirty ? L"You have changes that are not saved." : L"");
}

/* ---- interaction --------------------------------------------------------- */

static int hit_test(POINT pt, int *index)
{
    for (int i = 0; i < g_screenCount; i++)
        if (PtInRect(&g_screens[i].draw, pt)) { *index = i; return HIT_SCREEN; }
    RECT list = { g_rcDevices.left, g_rcDevices.top + S(59), g_rcDevices.right, g_rcDevices.bottom - S(3) };
    if (PtInRect(&list, pt))
        for (int i = 0; i < g_deviceCount; i++)
            if (PtInRect(&g_devices[i].draw, pt)) { *index = i; return HIT_DEVICE; }
    for (int s = 1; s < g_seatCount; s++)
        if (PtInRect(&g_seatDraw[s], pt)) { *index = s; return HIT_SEAT; }
    *index = -1;
    return HIT_NONE;
}

static void choose_seat(int kind, int index, POINT screenPt)
{
    HMENU menu = CreatePopupMenu();
    int current = kind == HIT_SCREEN ? g_screens[index].seat : g_devices[index].seat;
    for (int seat = 0; seat < g_seatCount; seat++) {
        WCHAR label[40];
        seat_label(seat, label, 40);
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
        g_screens[index].seat = seat;
        _snwprintf(msg, 300, L"Screen %d \x2192 %ls. Not saved yet.", index + 1, label);
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

static void remove_last_seat(void)
{
    int last = g_seatCount - 1;
    if (last < 1) return;
    for (int i = 0; i < g_screenCount; i++) if (g_screens[i].seat == last) g_screens[i].seat = 0;
    for (int i = 0; i < g_deviceCount; i++) if (g_devices[i].seat == last) g_devices[i].seat = 0;
    g_accounts[last][0] = 0;
    g_seatCount--;
    _snwprintf(g_status, 300, L"Workplace %d removed. Its screens and devices went back to Workplace 1.", last + 1);
    changed();
}

static void set_finding(BOOL on)
{
    RAWINPUTDEVICE want[2] = {
        { 1, 6, on ? RIDEV_INPUTSINK : RIDEV_REMOVE, on ? g_wnd : NULL }, /* keyboards */
        { 1, 2, on ? RIDEV_INPUTSINK : RIDEV_REMOVE, on ? g_wnd : NULL }, /* mice */
    };
    if (!RegisterRawInputDevices(want, 2, sizeof(want[0])) && on) {
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
    if (!g_finding || GetRawInputData(raw, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    if (!input.header.hDevice) return;
    for (int i = 0; i < g_deviceCount; i++) {
        Device *d = &g_devices[i];
        for (int h = 0; h < d->handleCount; h++) {
            if (d->handles[h] != input.header.hDevice) continue;
            BOOL fresh = !d->flashUntil;
            d->flashUntil = GetTickCount() + 1500;
            if (d->flashUntil == 0) d->flashUntil = 1;
            if (fresh) {
                WCHAR label[40], msg[300];
                int viewH = g_rcDevices.bottom - g_rcDevices.top - S(62), rowTop = i * S(ROW_H);
                if (rowTop < g_scroll) g_scroll = rowTop;
                if (rowTop + S(ROW_H) > g_scroll + viewH) g_scroll = rowTop + S(ROW_H) - viewH;
                if (g_scroll < 0) g_scroll = 0;
                seat_label(d->seat, label, 40);
                _snwprintf(msg, 300, L"You used: %ls  (%ls)", d->name, label);
                msg[299] = 0;
                lstrcpynW(g_status, msg, 300);
                InvalidateRect(g_wnd, NULL, FALSE);
            }
            SetTimer(g_wnd, 1, 200, NULL);
            return;
        }
    }
}

static void demo_find_tick(void)
{
    static int next;
    if (!g_finding || g_deviceCount == 0) return;
    Device *d = &g_devices[next++ % g_deviceCount];
    WCHAR label[40], msg[300];
    d->flashUntil = GetTickCount() + 1500;
    seat_label(d->seat, label, 40);
    _snwprintf(msg, 300, L"You used: %ls  (%ls)", d->name, label);
    lstrcpynW(g_status, msg, 300);
    InvalidateRect(g_wnd, NULL, FALSE);
    SetTimer(g_wnd, 1, 200, NULL);
}

static BOOL confirm_close(void)
{
    if (!g_dirty) return TRUE;
    int answer = MessageBoxW(g_wnd, L"Save your changes before closing?", L"OpenSeat", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return FALSE;
    if (answer == IDYES && !save_all()) {
        MessageBoxW(g_wnd, L"The setup could not be saved.", L"OpenSeat", MB_OK | MB_ICONERROR);
        return FALSE;
    }
    return TRUE;
}

static BOOL store_password(int seat, const WCHAR *account, const WCHAR *password)
{
    WCHAR target[64], user[MAX_COMPUTERNAME_LENGTH + ACCOUNT_CHARS + 2], comp[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(comp, &n)) lstrcpynW(comp, L".", MAX_COMPUTERNAME_LENGTH + 1);
    _snwprintf(user, MAX_COMPUTERNAME_LENGTH + ACCOUNT_CHARS + 2, L"%ls\\%ls", comp, account);
    seat_cred_target(seat, target, 64);
    CREDENTIALW c;
    ZeroMemory(&c, sizeof(c));
    c.Type = CRED_TYPE_GENERIC; /* the Remote Desktop client reads TERMSRV/<host> */
    c.TargetName = target;
    c.UserName = user;
    c.CredentialBlob = (LPBYTE)password;
    c.CredentialBlobSize = (DWORD)(wcslen(password) * sizeof(WCHAR));
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&c, 0);
}

static INT_PTR CALLBACK account_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static int seat;
    if (msg == WM_INITDIALOG) {
        WCHAR info[200];
        seat = (int)lp;
        _snwprintf(info, 200, L"Workplace %d signs in to this Windows account. Every workplace needs its own account, "
                              L"different from yours.", seat + 1);
        SetDlgItemTextW(dlg, IDC_ACC_INFO, info);
        SetDlgItemTextW(dlg, IDC_ACC_NAME, g_accounts[seat]);
        SendDlgItemMessageW(dlg, IDC_ACC_NAME, EM_LIMITTEXT, 20, 0);
        SendDlgItemMessageW(dlg, IDC_ACC_PASS, EM_LIMITTEXT, 127, 0);
        SendDlgItemMessageW(dlg, IDC_ACC_PASS2, EM_LIMITTEXT, 127, 0);
        return TRUE;
    }
    if (msg != WM_COMMAND) return FALSE;
    if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
    if (LOWORD(wp) != IDOK) return FALSE;

    WCHAR name[ACCOUNT_CHARS], pass[128], pass2[128], me[ACCOUNT_CHARS + 200];
    DWORD meLen = ACCOUNT_CHARS + 200;
    const WCHAR *problem = NULL;
    GetDlgItemTextW(dlg, IDC_ACC_NAME, name, ACCOUNT_CHARS);
    GetDlgItemTextW(dlg, IDC_ACC_PASS, pass, 128);
    GetDlgItemTextW(dlg, IDC_ACC_PASS2, pass2, 128);
    if (!GetUserNameW(me, &meLen)) me[0] = 0;
    if (!valid_account_name(name)) problem = L"Use an account name of up to 20 letters or digits, without symbols like \\ / : * ? \" < > |.";
    else if (lstrcmpiW(name, me) == 0) problem = L"This is your own account. The extra workplace needs a different one.";
    else if (!pass[0]) problem = L"Enter the password. Windows does not allow Remote Desktop sign-in without one.";
    else if (lstrcmpW(pass, pass2) != 0) problem = L"The two passwords are different.";
    for (int s = 1; s < g_seatCount && !problem; s++)
        if (s != seat && lstrcmpiW(name, g_accounts[s]) == 0) problem = L"Another workplace already uses this account.";
    if (!problem && !store_password(seat, name, pass)) problem = L"Windows did not let OpenSeat save the password.";
    SecureZeroMemory(pass, sizeof(pass));
    SecureZeroMemory(pass2, sizeof(pass2));
    if (problem) { MessageBoxW(dlg, problem, L"OpenSeat", MB_OK | MB_ICONWARNING); return TRUE; }
    lstrcpynW(g_accounts[seat], name, ACCOUNT_CHARS);
    EndDialog(dlg, IDOK);
    return TRUE;
}

static void edit_account(int seat)
{
    if (g_demo) { set_status(L"Demo: accounts cannot be set in demo mode."); return; }
    if (DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_ACCOUNT), g_wnd, account_proc, (LPARAM)seat) == IDOK) {
        _snwprintf(g_status, 300, L"Workplace %d signs in as %ls. Not saved yet.", seat + 1, g_accounts[seat]);
        changed();
    }
}

static void start_workplaces(void)
{
    WCHAR msg[300];
    for (int s = 1; s < g_seatCount; s++) {
        msg[0] = 0;
        if (seat_screens(s) == 0) _snwprintf(msg, 300, L"Workplace %d needs a screen first.", s + 1);
        else if (!g_accounts[s][0]) _snwprintf(msg, 300, L"Workplace %d needs a Windows account first. Click its card.", s + 1);
        if (msg[0]) { set_status(msg); MessageBeep(MB_ICONWARNING); return; }
    }
    if (g_dirty && !save_all()) {
        MessageBoxW(g_wnd, L"The setup could not be saved.", L"OpenSeat", MB_OK | MB_ICONERROR);
        return;
    }
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.hwnd = g_wnd;
    info.lpVerb = L"runas";
    info.lpFile = self;
    info.lpParameters = L"--start";
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) {
        set_status(L"Starting the workplaces\x2026 Follow the instructions on each screen.");
    } else {
        set_status(GetLastError() == ERROR_CANCELLED ? L"Start was cancelled: OpenSeat needs administrator permission."
                                                     : L"Could not start the workplaces.");
    }
    update_buttons();
    InvalidateRect(g_wnd, NULL, FALSE);
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
        g_btnRefresh = make_button(L"Refresh", ID_REFRESH, FALSE);
        g_btnSave = make_button(L"Save setup", ID_SAVE, TRUE);
        g_btnAdd = make_button(L"Add a workplace", ID_ADD, FALSE);
        g_btnRemove = make_button(L"Remove last", ID_REMOVE, FALSE);
        g_btnStart = make_button(L"Start workplaces", ID_START, FALSE);
        find_hardware();
        load_setup();
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
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == wnd && g_hoverKind != HIT_NONE) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int index, kind = hit_test(pt, &index);
        if (kind == HIT_SEAT) {
            edit_account(index);
        } else if (kind != HIT_NONE) {
            RECT r = kind == HIT_SCREEN ? g_screens[index].draw : g_devices[index].draw;
            POINT at = { kind == HIT_SCREEN ? r.left : r.right - S(144), r.bottom - (kind == HIT_SCREEN ? 0 : S(8)) };
            ClientToScreen(wnd, &at);
            choose_seat(kind, index, at);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(ROW_H) / WHEEL_DELTA;
        layout();
        InvalidateRect(wnd, &g_rcDevices, FALSE);
        return 0;
    case WM_INPUT:
        on_raw_input((HRAWINPUT)lp);
        break; /* DefWindowProc must still clean up */
    case WM_TIMER: {
        BOOL any = FALSE;
        DWORD now = GetTickCount();
        if (wp == 2) { demo_find_tick(); return 0; }
        for (int i = 0; i < g_deviceCount; i++) {
            if (!g_devices[i].flashUntil) continue;
            if ((int)(g_devices[i].flashUntil - now) <= 0) g_devices[i].flashUntil = 0;
            else any = TRUE;
        }
        if (!any) KillTimer(wnd, 1);
        InvalidateRect(wnd, &g_rcDevices, FALSE);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
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
        case ID_REFRESH: {
            /* Keep choices made so far for hardware that is still there. */
            Screen *oldScreens = malloc(sizeof(g_screens));
            Device *oldDevices = malloc(sizeof(g_devices));
            int oldS = g_screenCount, oldD = g_deviceCount;
            if (!oldScreens || !oldDevices) { free(oldScreens); free(oldDevices); return 0; }
            memcpy(oldScreens, g_screens, sizeof(g_screens));
            memcpy(oldDevices, g_devices, sizeof(g_devices));
            find_hardware();
            for (int i = 0; i < g_screenCount; i++) {
                g_screens[i].seat = 0;
                for (int j = 0; j < oldS; j++)
                    if (lstrcmpiW(g_screens[i].key, oldScreens[j].key) == 0) g_screens[i].seat = oldScreens[j].seat;
            }
            for (int i = 0; i < g_deviceCount; i++) {
                g_devices[i].seat = 0;
                for (int j = 0; j < oldD; j++)
                    if (lstrcmpiW(g_devices[i].key, oldDevices[j].key) == 0) g_devices[i].seat = oldDevices[j].seat;
            }
            free(oldScreens);
            free(oldDevices);
            layout();
            _snwprintf(g_status, 300, L"Found %d screen(s) and %d keyboard/mouse device(s).", g_screenCount, g_deviceCount);
            InvalidateRect(wnd, NULL, FALSE);
            return 0;
        }
        case ID_START:
            start_workplaces();
            return 0;
        case ID_SAVE:
            if (save_all()) set_status(L"Saved.");
            else MessageBoxW(wnd, L"The setup could not be saved.", L"OpenSeat", MB_OK | MB_ICONERROR);
            update_buttons();
            InvalidateRect(wnd, NULL, FALSE);
            return 0;
        }
        break;
    }
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
    if (cmd && wcsstr(cmd, L"--agent")) return run_agent(inst, test);
    if (cmd && wcsstr(cmd, L"--start")) return run_controller(inst, test);
    g_demo = cmd && wcsstr(cmd, L"--demo") != NULL;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW cls;
    ZeroMemory(&cls, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = wnd_proc;
    cls.hInstance = inst;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    cls.lpszClassName = L"OpenSeatMain";
    if (!RegisterClassExW(&cls)) return 1;

    HDC screen = GetDC(NULL);
    int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    HWND wnd = CreateWindowExW(0, cls.lpszClassName, L"OpenSeat", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
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
