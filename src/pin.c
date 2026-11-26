/* Pin windows and their session. Collapse geometry, the recycle queue and
 * restore order follow SETUNA2 (Copyright (c) 2019 tylearymf, MIT). */
#include "pin.h"
#include "image.h"
#include "lang.h"
#include "ocr.h"
#include "overlay.h"
#include "config.h"
#include "settings.h"
#include <dwmapi.h>
#include <math.h>
#include <windowsx.h>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#define TIMER_LABEL 1
#define TIMER_REST 2 /* the wheel has stopped zooming */
#define TIMER_PIN_SESSION 5 /* on the daemon window */
#define WM_APP_OUTLINE (WM_APP + 7)
enum { CM_OCR = PK_COUNT + 1, CM_RESET, CM_TOP_ALL, CM_TOP, CM_CLOSE_ALL, CM_SETTINGS, CM_UNDO, CM_TRASH_CLEAR,
       CM_PIN_FIRST = 1000 };
enum { COMPACT_SIZE = 50 };

typedef struct Pin {
    HWND hwnd; /* NULL: the window is gone, a save still holds the picture */
    Img img;
    float zoom;
    int alpha, shown_alpha; /* percentages; the preference can override the shown value */
    bool top;  /* this pin above other windows, whatever the setting for all of them says */
    bool drag, hovered, compact, no_border, recycled, cropping, restore_hidden;
    POINT drag_pt, win_pt;
    POINT compact_pt, crop_start;
    RECT crop;
    bool rough;    /* shrunk while the wheel turns: stretched the fast way until it rests */
    int saving;    /* save dialogs open for this pin */
    RECT label_rc; /* what the label covers; empty: none shows */
    float label_s; /* the scale label_rc was measured at */
    WCHAR label[96];
    UINT_PTR id;
    struct Pin *next;
} Pin;

static struct {
    HINSTANCE inst;
    HWND daemon;
    Pin *list, *trash; /* newest closed first; eviction removes the oldest */
    int count;
    UINT_PTR next_id;
    PinPrefs prefs;
    KeyBinding list_key, undo_key;
    bool text, started, dirty, cache_failed;
    bool top_all; /* the setting: every pin above other windows */
} P;

static void pin_dirty(void);
static void pin_close(Pin *p);
static void pin_trim_trash(int limit);

/* Paint while cloaked. WM_PAINT can finish before DWM picks up the GDI
 * surface, leaving a white frame if the cloak comes off immediately. */
static void pin_reveal(Pin *first, Pin *end, bool from_cache)
{
    if (first == end)
        return;
    BOOL on = TRUE, off = FALSE;
    for (Pin *p = first; p != end; p = p->next) {
        if (from_cache && p->restore_hidden)
            continue;
        DwmSetWindowAttribute(p->hwnd, DWMWA_CLOAK, &on, sizeof on);
        ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
        RedrawWindow(p->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    GdiFlush();
    DwmFlush(); /* one composition wait even when restoring several pins */
    for (Pin *p = first; p != end; p = p->next)
        if (!from_cache || !p->restore_hidden)
            DwmSetWindowAttribute(p->hwnd, DWMWA_CLOAK, &off, sizeof off);
}

/* A pin is above other windows when all pins are, or when it was told to be itself. */
static void pin_layer(Pin *p)
{
    SetWindowPos(p->hwnd, P.top_all || p->top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void pin_top_all(bool on)
{
    if (P.top_all == on)
        return;
    P.top_all = on;
    for (Pin *p = P.list; p; p = p->next)
        pin_layer(p);
}

static Pin *pin_of(HWND h) { return (Pin *)GetWindowLongPtrW(h, GWLP_USERDATA); }

static float pin_scale(Pin *p) { return (float)GetDpiForWindow(p->hwnd) / 96.f; }

static SIZE pin_size(Pin *p)
{
    if (p->zoom == 1.f)
        return (SIZE){p->img.w, p->img.h};
    return (SIZE){MAXI(8, (int)lroundf((float)p->img.w * p->zoom)),
                  MAXI(8, (int)lroundf((float)p->img.h * p->zoom))};
}

static POINT pin_visible_at(RECT r)
{
    MONITORINFO mi = {sizeof mi};
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL) &&
        GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) {
        int w = r.right - r.left, h = r.bottom - r.top;
        r.left = CLAMPI(r.left, mi.rcWork.left, MAXI(mi.rcWork.left, mi.rcWork.right - w));
        r.top = CLAMPI(r.top, mi.rcWork.top, MAXI(mi.rcWork.top, mi.rcWork.bottom - h));
    }
    return (POINT){r.left, r.top};
}

static POINT pin_center(Pin *p)
{
    RECT r;
    GetWindowRect(p->hwnd, &r);
    return (POINT){r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2};
}

static void clear_label(Pin *p)
{
    KillTimer(p->hwnd, TIMER_LABEL);
    p->label_rc = (RECT){0};
}

static HFONT label_font(float s)
{
    return CreateFontW(-(int)lroundf(12.f * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, T(FONT));
}

/* Stretched the fast way, which gives other pixels straight onto the window than into a bitmap. */
static bool enlarged(Pin *p, const RECT *rc)
{
    return rc->right >= p->img.w && (rc->right != p->img.w || rc->bottom != p->img.h);
}

static void label_changed(Pin *p, RECT place)
{
    RECT rc;
    GetClientRect(p->hwnd, &rc);
    InvalidateRect(p->hwnd, enlarged(p, &rc) ? NULL : &place, FALSE);
}

static void flash_label(Pin *p, const WCHAR *text)
{
    lstrcpynW(p->label, text, COUNTOF(p->label));
    float s = pin_scale(p);
    int pad = (int)lroundf(6.f * s);
    HDC dc = GetDC(p->hwnd);
    HFONT f = label_font(s);
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, p->label, lstrlenW(p->label), &sz);
    SelectObject(dc, of);
    DeleteObject(f);
    ReleaseDC(p->hwnd, dc);
    RECT was = p->label_rc;
    p->label_rc = (RECT){0, 0, sz.cx + pad * 2, sz.cy + pad};
    p->label_s = s;
    SetTimer(p->hwnd, TIMER_LABEL, 900, NULL);
    label_changed(p, rect_union(was, p->label_rc));
}

static void set_zoom(Pin *p, float z, POINT anchor)
{
    float maxz = fminf(16.f, 16000.f / (float)MAXI(p->img.w, p->img.h));
    z = fmaxf(0.05f, fminf(z, maxz));
    RECT wr;
    GetWindowRect(p->hwnd, &wr);
    float fx = (float)(anchor.x - wr.left) / (float)MAXI(1, wr.right - wr.left);
    float fy = (float)(anchor.y - wr.top) / (float)MAXI(1, wr.bottom - wr.top);
    p->zoom = z;
    SIZE size = pin_size(p);
    int nw = size.cx, nh = size.cy;
    int nx = anchor.x - (int)lroundf(fx * (float)nw), ny = anchor.y - (int)lroundf(fy * (float)nh);
    SetWindowPos(p->hwnd, NULL, nx, ny, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
    if (nw != wr.right - wr.left || nh != wr.bottom - wr.top) {
        p->rough = nw < p->img.w && SetTimer(p->hwnd, TIMER_REST, 120, NULL);
        InvalidateRect(p->hwnd, NULL, FALSE);
    }
    WCHAR t[16];
    wsprintfW(t, L"%d%%", (int)lroundf(z * 100.f));
    flash_label(p, t);
    pin_dirty();
}

static void apply_alpha(Pin *p)
{
    int pct = p->alpha;
    if (p->compact)
        pct = 80;
    else if (!p->cropping && GetForegroundWindow() != p->hwnd) {
        int inactive = p->hovered ? P.prefs.hover : P.prefs.inactive;
        if (inactive)
            pct = inactive;
    }
    if (p->shown_alpha == pct)
        return;
    p->shown_alpha = pct;
    LONG ex = GetWindowLongW(p->hwnd, GWL_EXSTYLE);
    if (pct < 100) {
        if (!(ex & WS_EX_LAYERED))
            SetWindowLongW(p->hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(p->hwnd, 0, (BYTE)(pct * 255 / 100), LWA_ALPHA);
    } else if (ex & WS_EX_LAYERED) {
        /* fully opaque again: drop the layered style, it costs composition work */
        SetWindowLongW(p->hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
        RedrawWindow(p->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_FRAME);
    }
}

static void set_alpha(Pin *p, int pct)
{
    p->alpha = CLAMPI(pct, 1, 100);
    apply_alpha(p);
    WCHAR t[48];
    wsprintfW(t, T(PIN_OPACITY), p->alpha);
    flash_label(p, t);
    pin_dirty();
}

static bool pin_copy(Pin *p)
{
    bool ok = clip_put_image(P.daemon, p->img.px, p->img.w, p->img.w, p->img.h);
    flash_label(p, ok ? T(PIN_COPIED) : T(PM_COPY_FAILED));
    return ok;
}

static void pin_free(Pin *p)
{
    img_destroy(&p->img);
    mem_free(p);
}

static void pin_save(Pin *p)
{
    /* The dialog runs a nested message loop in which this pin may be closed: while a save
     * is under way, WM_NCDESTROY leaves the picture and the Pin to it. */
    WCHAR path[MAX_PATH * 2];
    p->saving++;
    bool named = save_dialog(path, COUNTOF(path));
    bool ok = named && image_save(path, p->img.px, p->img.w, p->img.w, p->img.h);
    p->saving--;
    if (!p->hwnd) {
        if (!p->saving)
            pin_free(p);
    } else if (named) {
        flash_label(p, ok ? T(PIN_SAVED) : T(PIN_SAVE_FAILED));
    }
}

/* The small window shows the pixels around the point that was double-clicked.
 * Moving its center moves the expanded window by the same amount. */
static void set_compact(Pin *p, POINT pt)
{
    if (GetCapture() == p->hwnd)
        ReleaseCapture();
    RECT r;
    GetWindowRect(p->hwnd, &r);
    SIZE sz = {COMPACT_SIZE, COMPACT_SIZE};
    int x, y;
    if (p->compact) {
        sz = pin_size(p);
        x = r.left + COMPACT_SIZE / 2 - p->compact_pt.x;
        y = r.top + COMPACT_SIZE / 2 - p->compact_pt.y;
    } else {
        p->compact_pt.x = CLAMPI(pt.x - r.left, 0, r.right - r.left - 1);
        p->compact_pt.y = CLAMPI(pt.y - r.top, 0, r.bottom - r.top - 1);
        x = r.left + p->compact_pt.x - COMPACT_SIZE / 2;
        y = r.top + p->compact_pt.y - COMPACT_SIZE / 2;
    }
    p->compact = !p->compact;
    clear_label(p);
    SetWindowPos(p->hwnd, NULL, x, y, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    apply_alpha(p);
    InvalidateRect(p->hwnd, NULL, FALSE);
    pin_dirty();
}

static void pin_move(Pin *p, int x, int y)
{
    SetWindowPos(p->hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void pin_replace(Pin *p, Img *image, int x, int y)
{
    img_destroy(&p->img);
    p->img = *image;
    clear_label(p);
    SIZE size = pin_size(p);
    SetWindowPos(p->hwnd, NULL, x, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(p->hwnd, NULL, FALSE);
    pin_dirty();
}

static void pin_transform(Pin *p, int action)
{
    const Img *src = &p->img;
    RECT r;
    GetWindowRect(p->hwnd, &r);
    bool turn = action == PK_ROTATE;
    Img dst;
    if (!img_create(&dst, turn ? src->h : src->w, turn ? src->w : src->h)) {
        flash_label(p, T(PM_EDIT_FAILED));
        return;
    }
    for (int y = 0; y < src->h; y++) {
        const uint32_t *row = src->px + (size_t)y * src->w;
        if (turn) {
            for (int x = 0; x < src->w; x++)
                dst.px[(size_t)x * dst.w + src->h - 1 - y] = row[x];
        } else if (action == PK_FLIP_H) {
            uint32_t *out = dst.px + (size_t)y * dst.w;
            for (int x = 0; x < src->w; x++)
                out[dst.w - 1 - x] = row[x];
        } else {
            memcpy(dst.px + (size_t)(dst.h - 1 - y) * dst.w, row, (size_t)dst.w * sizeof *row);
        }
    }
    int offset = turn ? ((r.right - r.left) - (r.bottom - r.top)) / 2 : 0;
    pin_replace(p, &dst, r.left + offset, r.top - offset);
}

static void crop_end(Pin *p, bool commit)
{
    RECT crop = p->crop, r, view;
    p->cropping = false;
    p->crop = (RECT){0};
    clear_label(p);
    if (GetCapture() == p->hwnd)
        ReleaseCapture();
    apply_alpha(p);
    InvalidateRect(p->hwnd, NULL, FALSE);
    if (!commit || rect_empty(&crop))
        return;
    GetWindowRect(p->hwnd, &r);
    GetClientRect(p->hwnd, &view);
    int left = (int)((int64_t)crop.left * p->img.w / view.right);
    int top = (int)((int64_t)crop.top * p->img.h / view.bottom);
    int right = (int)(((int64_t)crop.right * p->img.w + view.right - 1) / view.right);
    int bottom = (int)(((int64_t)crop.bottom * p->img.h + view.bottom - 1) / view.bottom);
    if (left == 0 && top == 0 && right == p->img.w && bottom == p->img.h)
        return;
    Img dst;
    if (!img_create(&dst, right - left, bottom - top)) {
        flash_label(p, T(PM_EDIT_FAILED));
        return;
    }
    for (int y = 0; y < dst.h; y++)
        memcpy(dst.px + (size_t)y * dst.w, p->img.px + (size_t)(top + y) * p->img.w + left,
               (size_t)dst.w * sizeof *dst.px);
    pin_replace(p, &dst, r.left + crop.left, r.top + crop.top);
}

static void crop_update(Pin *p, POINT pt)
{
    RECT rc;
    GetClientRect(p->hwnd, &rc);
    pt.x = CLAMPI(pt.x, 0, rc.right - 1);
    pt.y = CLAMPI(pt.y, 0, rc.bottom - 1);
    p->crop = rect_corners(p->crop_start, pt);
    InvalidateRect(p->hwnd, NULL, FALSE);
}

static bool pin_action_enabled(Pin *p, int action)
{
    if (p->cropping)
        return action == PK_CLOSE || action == PK_CROP;
    return !p->compact || (action != PK_CROP && action != PK_ROTATE && action != PK_FLIP_H &&
                           action != PK_FLIP_V && action != PK_BORDER && action != PK_ZOOM_IN &&
                           action != PK_ZOOM_OUT && action != PK_ALPHA_MORE && action != PK_ALPHA_LESS &&
                           (action < PK_Z50 || action > PK_Z100));
}

static void pin_action(Pin *p, int action, bool fine_or_fast, POINT at)
{
    if (!pin_action_enabled(p, action))
        return;
    switch (action) {
    case PK_CLOSE: pin_close(p); break;
    case PK_COMPACT: set_compact(p, at); break;
    case PK_COPY: pin_copy(p); break;
    case PK_CUT:
        if (pin_copy(p))
            pin_close(p);
        break;
    case PK_SAVE: pin_save(p); break;
    case PK_PASTE: {
        Img image;
        RECT r;
        GetWindowRect(p->hwnd, &r);
        if (clip_get_image(P.daemon, &image, P.text) == EXIT_OK)
            pin_add(&image, r.left, r.top, false);
        break;
    }
    case PK_ROTATE:
    case PK_FLIP_H:
    case PK_FLIP_V:
        pin_transform(p, action);
        break;
    case PK_CROP:
        if (p->cropping) {
            crop_end(p, false);
        } else {
            if (GetCapture() == p->hwnd)
                ReleaseCapture();
            p->cropping = true;
            p->crop = (RECT){0};
            flash_label(p, T(PM_CROP_HINT));
            apply_alpha(p);
        }
        break;
    case PK_BORDER:
        p->no_border = !p->no_border;
        InvalidateRect(p->hwnd, NULL, FALSE);
        pin_dirty();
        break;
    case PK_ZOOM_IN:
    case PK_ZOOM_OUT: {
        float step = fine_or_fast ? 0.01f : 0.1f;
        set_zoom(p, p->zoom + (action == PK_ZOOM_IN ? step : -step), at);
        break;
    }
    case PK_ALPHA_MORE:
    case PK_ALPHA_LESS: {
        int step = fine_or_fast ? 1 : 10;
        set_alpha(p, p->alpha + (action == PK_ALPHA_MORE ? step : -step));
        break;
    }
    case PK_UP:
    case PK_DOWN:
    case PK_LEFT:
    case PK_RIGHT: {
        static const POINT direction[] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
        POINT d = direction[action - PK_UP];
        int step = fine_or_fast ? 50 : 1;
        RECT r;
        GetWindowRect(p->hwnd, &r);
        pin_move(p, r.left + d.x * step, r.top + d.y * step);
        pin_dirty();
        break;
    }
    default:
        if (action >= PK_Z50 && action <= PK_Z100)
            set_zoom(p, (float)(50 + (action - PK_Z50) * 10) / 100.f, at);
        break;
    }
}

static HBITMAP pin_preview(Pin *p)
{
    int w = p->img.w, h = p->img.h;
    float fit = fminf(32.f / w, 24.f / h);
    int nw = MAXI(1, (int)lroundf(w * fit)), nh = MAXI(1, (int)lroundf(h * fit));
    HDC screen = GetDC(NULL);
    if (!screen)
        return NULL;
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = dc ? CreateCompatibleBitmap(screen, nw, nh) : NULL;
    if (bmp) {
        HGDIOBJ old = SelectObject(dc, bmp);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchBlt(dc, 0, 0, nw, nh, p->img.dc, 0, 0, w, h, SRCCOPY);
        SelectObject(dc, old);
    }
    if (dc)
        DeleteDC(dc);
    ReleaseDC(NULL, screen);
    return bmp;
}

static void menu_label(WCHAR *label, const WCHAR *text, KeyBinding key)
{
    if (key.vk) {
        WCHAR k[40];
        hotkey_format(key.mod, key.vk, k);
        wsprintfW(label, L"%s\t%s", text, k);
    } else {
        lstrcpyW(label, text);
    }
}

static HMENU pin_book_menu(Pin *book, UINT first, bool trash)
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return NULL;
    if (!book)
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, T(PM_EMPTY));
    for (UINT i = 0; book; book = book->next, i++) {
        WCHAR name[96];
        const WCHAR *state = trash ? L"" : !IsWindowVisible(book->hwnd) ? T(PM_HIDDEN)
                           : book->compact ? T(PM_COMPACT) : L"";
        wsprintfW(name, L"%u. %d × %d%s%s", i + 1, book->img.w, book->img.h, *state ? L"  " : L"", state);
        MENUITEMINFOW item = {sizeof item};
        item.fMask = MIIM_ID | MIIM_STRING | MIIM_DATA | MIIM_BITMAP;
        item.wID = first + i;
        item.dwTypeData = name;
        item.dwItemData = book->id;
        item.hbmpItem = pin_preview(book);
        if (!InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &item) && item.hbmpItem)
            DeleteObject(item.hbmpItem);
    }
    return menu;
}

static void pin_previews_free(HMENU menu)
{
    for (int i = 0, n = GetMenuItemCount(menu); i < n; i++) {
        MENUITEMINFOW item = {sizeof item};
        item.fMask = MIIM_BITMAP;
        if (GetMenuItemInfoW(menu, i, TRUE, &item) && item.hbmpItem)
            DeleteObject(item.hbmpItem);
    }
}

static void pin_activate(Pin *p)
{
    RECT r;
    if (GetWindowRect(p->hwnd, &r)) {
        POINT at = pin_visible_at(r);
        if (at.x != r.left || at.y != r.top)
            pin_move(p, at.x, at.y);
    }
    if (!IsWindowVisible(p->hwnd))
        pin_reveal(p, p->next, false);
    SetWindowPos(p->hwnd, P.top_all || p->top ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(p->hwnd);
    pin_dirty();
}

static void pin_restore(Pin *p)
{
    Pin **link = &P.trash;
    while (*link != p)
        link = &(*link)->next;
    *link = p->next;
    p->recycled = false;
    p->next = P.list;
    P.list = p;
    P.count++;
    pin_layer(p);
    pin_activate(p);
}

void pin_restore_last(void)
{
    if (P.trash)
        pin_restore(P.trash);
}

PinMenus pin_menus_add(HMENU parent)
{
    PinMenus menus = {pin_book_menu(P.list, CM_PIN_FIRST + PIN_TRASH_MAX, false),
                      pin_book_menu(P.trash, CM_PIN_FIRST, true)};
    WCHAR text[144];
    if (menus.list) {
        menu_label(text, T(PM_LIST), P.list_key);
        AppendMenuW(parent, MF_POPUP, (UINT_PTR)menus.list, text);
    }
    if (menus.trash) {
        AppendMenuW(menus.trash, MF_SEPARATOR, 0, NULL);
        menu_label(text, T(P_GLOBAL_UNDO), P.undo_key);
        AppendMenuW(menus.trash, MF_STRING | (P.trash ? 0 : MF_GRAYED), CM_UNDO, text);
        AppendMenuW(menus.trash, MF_STRING | (P.trash ? 0 : MF_GRAYED), CM_TRASH_CLEAR, T(PM_TRASH_CLEAR));
        AppendMenuW(parent, MF_POPUP, (UINT_PTR)menus.trash, T(PM_TRASH));
    }
    if (P.cache_failed)
        AppendMenuW(parent, MF_STRING | MF_GRAYED, 0, T(PM_PERSIST_FAILED));
    return menus;
}

bool pin_menus_run(PinMenus menus, UINT cmd)
{
    if (cmd == CM_UNDO) {
        pin_restore_last();
        return true;
    }
    if (cmd == CM_TRASH_CLEAR) {
        if (P.trash && MessageBoxW(P.daemon, T(PM_TRASH_CONFIRM), T(PM_TRASH),
                                  MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND) == IDYES)
            pin_trim_trash(0);
        return true;
    }
    if (cmd < CM_PIN_FIRST)
        return false;
    bool trash = cmd < CM_PIN_FIRST + PIN_TRASH_MAX;
    MENUITEMINFOW item = {sizeof item};
    item.fMask = MIIM_DATA;
    if (GetMenuItemInfoW(trash ? menus.trash : menus.list, cmd, FALSE, &item)) {
        for (Pin *p = trash ? P.trash : P.list; p; p = p->next) {
            if (p->id != item.dwItemData)
                continue;
            if (trash)
                pin_restore(p);
            else
                pin_activate(p);
            break;
        }
    }
    return true;
}

void pin_menus_dispose(PinMenus menus)
{
    pin_previews_free(menus.list);
    pin_previews_free(menus.trash);
}

void pin_list_menu(void)
{
    HMENU menu = pin_book_menu(P.list, CM_PIN_FIRST + PIN_TRASH_MAX, false);
    if (!menu)
        return;
    POINT pt = {0};
    GetCursorPos(&pt);
    HWND owner = P.list && IsWindowVisible(P.list->hwnd) ? P.list->hwnd : P.daemon;
    SetForegroundWindow(owner);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, owner, NULL);
    pin_menus_run((PinMenus){.list = menu}, cmd);
    pin_previews_free(menu);
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);
}

static void pin_command_item(HMENU menu, Pin *p, int action)
{
    const WCHAR *label = action == PK_COMPACT && p->compact ? T(PM_EXPAND) : g_tr[pin_key_text[action]];
    WCHAR text[144];
    UINT flags = pin_action_enabled(p, action) ? 0 : MF_GRAYED;
    if (action == PK_BORDER && !p->no_border)
        flags |= MF_CHECKED;
    menu_label(text, label, P.prefs.key[action]);
    AppendMenuW(menu, MF_STRING | flags, action + 1, text);
}

static void pin_menu(Pin *p, POINT at)
{
    HMENU m = CreatePopupMenu();
    if (!m)
        return;
    pin_command_item(m, p, PK_COMPACT);
    HMENU zoom = CreatePopupMenu();
    if (zoom) {
        for (int i = PK_Z50; i <= PK_Z100; i++)
            pin_command_item(zoom, p, i);
        AppendMenuW(zoom, MF_SEPARATOR, 0, NULL);
        pin_command_item(zoom, p, PK_ZOOM_IN);
        pin_command_item(zoom, p, PK_ZOOM_OUT);
        AppendMenuW(m, MF_POPUP | (p->compact ? MF_GRAYED : 0), (UINT_PTR)zoom, T(PM_ZOOM));
    }
    for (int i = PK_COPY; i <= PK_PASTE; i++)
        pin_command_item(m, p, i);
    if (ocr_there)
        AppendMenuW(m, MF_STRING, CM_OCR, T(PM_OCR));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (int i = PK_ROTATE; i <= PK_BORDER; i++)
        pin_command_item(m, p, i);
    bool reset = !p->cropping && !p->compact && (p->zoom != 1.f || p->alpha != 100);
    AppendMenuW(m, MF_STRING | (reset ? 0 : MF_GRAYED), CM_RESET, T(PM_RESET));
    AppendMenuW(m, MF_STRING | (P.top_all ? MF_CHECKED : 0), CM_TOP_ALL, T(PM_TOP_ALL));
    AppendMenuW(m, MF_STRING | (p->top ? MF_CHECKED : 0), CM_TOP, T(PM_TOP));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    PinMenus menus = pin_menus_add(m);
    pin_command_item(m, p, PK_CLOSE);
    AppendMenuW(m, MF_STRING, CM_CLOSE_ALL, T(M_CLOSE_PINS));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CM_SETTINGS, T(M_SETTINGS));
    HWND h = p->hwnd;
    UINT_PTR id = p->id;
    SetForegroundWindow(h);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, 0, h, NULL);
    bool managed = pin_menus_run(menus, cmd);
    pin_menus_dispose(menus);
    DestroyMenu(m);
    PostMessageW(h, WM_NULL, 0, 0);
    p = pin_of(h);
    if (managed || !p || p->id != id || p->recycled)
        return;
    if (cmd >= 1 && cmd <= PK_COUNT) {
        int action = cmd - 1;
        pin_action(p, action, false, action == PK_COMPACT ? at : pin_center(p));
        return;
    }
    switch (cmd) {
    case CM_OCR: {
        RECT r;
        GetWindowRect(h, &r);
        ocr_picture(p->img.px, p->img.w, p->img.h, r, NULL);
        break;
    }
    case CM_RESET: {
        RECT r;
        GetWindowRect(h, &r);
        set_alpha(p, 100);
        set_zoom(p, 1.f, (POINT){r.left, r.top});
        break;
    }
    case CM_TOP_ALL:
        pin_top_all(!P.top_all);
        config_set_pin_top(P.top_all);
        break;
    case CM_TOP:
        p->top = !p->top;
        pin_layer(p);
        pin_dirty();
        break;
    case CM_CLOSE_ALL: pin_close_all(); break;
    case CM_SETTINGS: settings_open(L"pins"); break;
    }
}

static void paint_picture(Pin *p, HDC dc, const RECT *rc)
{
    RECT view = *rc;
    if (p->compact) {
        SIZE size = pin_size(p);
        int x = COMPACT_SIZE / 2 - p->compact_pt.x, y = COMPACT_SIZE / 2 - p->compact_pt.y;
        view = (RECT){x, y, x + size.cx, y + size.cy};
        FillRect(dc, rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    int w = view.right - view.left, h = view.bottom - view.top;
    if (w == p->img.w && h == p->img.h) {
        BitBlt(dc, view.left, view.top, w, h, p->img.dc, 0, 0, SRCCOPY);
    } else {
        SetStretchBltMode(dc, w >= p->img.w || p->rough ? COLORONCOLOR : HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchBlt(dc, view.left, view.top, w, h, p->img.dc, 0, 0, p->img.w, p->img.h, SRCCOPY);
    }
}

/* An outline, so a pin stands out from what is behind it: the accent color on the pin in use. Its last row
 * is where the frame reaches into the window (for the shadow). DWM shows the frame through pixels without
 * alpha, which is how GDI paints, so that row is drawn from a pixel that has it. */
static void paint_outline(Pin *p, HDC dc, const RECT *rc)
{
    bool active = GetForegroundWindow() == p->hwnd;
    if (!p->compact && (p->no_border || (!active && !P.prefs.inactive_outline)))
        return;
    uint32_t c = active ? 0xFF1A8CFF : 0xFF808080;
    HBRUSH edge = CreateSolidBrush(RGB(c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF));
    FrameRect(dc, rc, edge);
    DeleteObject(edge);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), 1, 1, 1, 32, BI_RGB}};
    SetStretchBltMode(dc, COLORONCOLOR); /* HALFTONE drops the alpha */
    StretchDIBits(dc, 0, rc->bottom - 1, rc->right, 1, 0, 0, 1, 1, &c, &bi, DIB_RGB_COLORS, SRCCOPY);
}

/* The label, composed off-screen so it never flickers: by itself, or with all of an enlarged pin.
 * What is painted afterwards goes around what was composed; false when nothing is left for it. */
static bool compose_label(Pin *p, HDC dc, const RECT *rc)
{
    bool all = enlarged(p, rc), more = true;
    RECT part = all ? *rc : p->label_rc;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, part.right, part.bottom);
    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        if (all) {
            paint_picture(p, mem, rc);
            paint_outline(p, mem, rc);
        }
        HFONT f = label_font(p->label_s);
        HGDIOBJ of = SelectObject(mem, f);
        HBRUSH br = CreateSolidBrush(RGB(32, 32, 32));
        FillRect(mem, &p->label_rc, br);
        DeleteObject(br);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 255));
        DrawTextW(mem, p->label, -1, &p->label_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(mem, of);
        DeleteObject(f);
        BitBlt(dc, 0, 0, part.right, part.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        more = ExcludeClipRect(dc, 0, 0, part.right, part.bottom) != NULLREGION;
    }
    if (mem)
        DeleteDC(mem);
    if (bmp)
        DeleteObject(bmp);
    return more;
}

static void paint(Pin *p)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(p->hwnd, &ps);
    RECT rc;
    GetClientRect(p->hwnd, &rc);
    int state = p->cropping ? SaveDC(dc) : 0;
    if (rect_empty(&p->label_rc) || compose_label(p, dc, &rc)) {
        paint_picture(p, dc, &rc);
        paint_outline(p, dc, &rc);
    }
    if (state)
        RestoreDC(dc, state);
    if (p->cropping && !rect_empty(&p->crop)) {
        FrameRect(dc, &p->crop, (HBRUSH)GetStockObject(WHITE_BRUSH));
        RECT outer = rect_inflate(p->crop, 1);
        FrameRect(dc, &outer, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    EndPaint(p->hwnd, &ps);
}

static void pin_front(Pin *p)
{
    Pin **link = &P.list;
    while (*link && *link != p)
        link = &(*link)->next;
    if (*link && p != P.list) {
        *link = p->next;
        p->next = P.list;
        P.list = p;
        pin_dirty();
    }
}

static bool pin_key(Pin *p, UINT vk, bool repeat)
{
    UINT mod = (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) |
               (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) | (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0);
    if (p->cropping && (vk == VK_RETURN || vk == VK_ESCAPE)) {
        crop_end(p, vk == VK_RETURN);
        return true;
    }
    if (vk == VK_ESCAPE || (vk == VK_RETURN && p->compact)) {
        if (!repeat) {
            if (p->compact)
                set_compact(p, pin_center(p));
            else
                pin_close(p);
        }
        return true;
    }
    /* A specific binding takes precedence over Shift for fine or fast steps. */
    for (int i = 0; i < PK_COUNT; i++) {
        KeyBinding k = P.prefs.key[i];
        if (k.vk == vk && k.mod == mod) {
            if (!repeat || (i >= PK_ZOOM_IN && i <= PK_RIGHT))
                pin_action(p, i, false, pin_center(p));
            return true;
        }
    }
    if (mod & MOD_SHIFT) {
        for (int i = PK_ZOOM_IN; i <= PK_RIGHT; i++) {
            KeyBinding k = P.prefs.key[i];
            if (k.vk == vk && k.mod == (mod & ~MOD_SHIFT)) {
                pin_action(p, i, true, pin_center(p));
                return true;
            }
        }
    }
    return false;
}

static LRESULT CALLBACK pin_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_NCCREATE) {
        Pin *np = ((CREATESTRUCTW *)lp)->lpCreateParams;
        np->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)np);
    }
    Pin *p = pin_of(h);
    if (!p)
        return DefWindowProcW(h, m, wp, lp);
    switch (m) {
    case WM_NCCALCSIZE:
        if (wp)
            return 0; /* no non-client area: the image is the whole window */
        break;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SETCURSOR:
        if (p->cropping && LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, IDC_CROSS));
            return TRUE;
        }
        break;
    case WM_NCACTIVATE:
        return DefWindowProcW(h, m, wp, -1); /* do not repaint a frame we do not have */
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = mm->ptMinTrackSize.y = 1;
        mm->ptMaxTrackSize.x = mm->ptMaxTrackSize.y = 32000;
        return 0;
    }
    case WM_SYSCOMMAND: {
        UINT c = (UINT)(wp & 0xFFF0);
        if (c == SC_MAXIMIZE || c == SC_MINIMIZE || c == SC_SIZE || c == SC_RESTORE)
            return 0;
        break;
    }
    case WM_DPICHANGED:
        return 0; /* keep physical pixel size across monitors */
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE: /* the outline changes color; posted, as GetForegroundWindow is right only after the activation */
        PostMessageW(h, WM_APP_OUTLINE, 0, 0);
        break;
    case WM_APP_OUTLINE: {
        if (GetForegroundWindow() == h)
            pin_front(p);
        apply_alpha(p);
        if (p->compact || (!p->no_border && P.prefs.inactive_outline)) {
            RECT r;
            GetClientRect(h, &r);
            HDC dc = GetDC(h);
            ExcludeClipRect(dc, 0, 0, p->label_rc.right, p->label_rc.bottom);
            paint_outline(p, dc, &r);
            ReleaseDC(h, dc);
        } else if (!p->no_border) {
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_PAINT:
        paint(p);
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        p->drag = true;
        if (p->cropping) {
            p->crop_start = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            crop_update(p, p->crop_start);
            return 0;
        }
        GetCursorPos(&p->drag_pt);
        {
            RECT wr;
            GetWindowRect(h, &wr);
            p->win_pt.x = wr.left;
            p->win_pt.y = wr.top;
        }
        return 0;
    case WM_MOUSEMOVE:
        if (!p->hovered) {
            TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, h, 0};
            p->hovered = TrackMouseEvent(&track) != 0;
            apply_alpha(p);
        }
        if (p->drag) {
            if (p->cropping) {
                crop_update(p, (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            } else {
                POINT c;
                GetCursorPos(&c);
                pin_move(p, p->win_pt.x + c.x - p->drag_pt.x, p->win_pt.y + c.y - p->drag_pt.y);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        p->hovered = false;
        apply_alpha(p);
        return 0;
    case WM_LBUTTONUP:
        if (p->drag) {
            if (p->cropping)
                crop_update(p, (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (p->drag && !p->cropping)
            pin_dirty();
        p->drag = false;
        return 0;
    case WM_LBUTTONDBLCLK:
        if (p->cropping) {
            crop_end(p, true);
        } else if (p->compact || P.prefs.compact_on_double_click) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(h, &pt);
            set_compact(p, pt);
        } else {
            pin_close(p);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        UINT mods = GET_KEYSTATE_WPARAM(wp);
        if (!d || p->compact || p->cropping)
            return 0;
        if (mods & MK_CONTROL) {
            int step = mods & MK_SHIFT ? 1 : 10;
            set_alpha(p, p->alpha + (d > 0 ? step : -step));
        } else {
            POINT c = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            float z = p->zoom * (d > 0 ? 1.1f : 1.f / 1.1f);
            if ((p->zoom < 1.f && z > 1.f) || (p->zoom > 1.f && z < 1.f) || fabsf(z - 1.f) < 0.02f)
                z = 1.f; /* always stop at 100% on the way through */
            set_zoom(p, z, c);
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (pin_key(p, (UINT)wp, (lp & (1L << 30)) != 0))
            return 0;
        break;
    case WM_SYSCHAR:
        return 0;
    case WM_CLOSE:
        if (!p->recycled)
            pin_close(p);
        return 0;
    case WM_CONTEXTMENU: {
        POINT at = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (at.x == -1 && at.y == -1) {
            RECT wr;
            GetWindowRect(h, &wr);
            at.x = wr.left;
            at.y = wr.top;
        }
        pin_menu(p, at);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_LABEL) {
            KillTimer(h, TIMER_LABEL);
            label_changed(p, p->label_rc);
            p->label_rc = (RECT){0};
        } else if (wp == TIMER_REST) {
            KillTimer(h, TIMER_REST);
            if (p->rough) {
                p->rough = false;
                InvalidateRect(h, NULL, FALSE);
            }
        }
        return 0;
    case WM_NCDESTROY: {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        Pin **pp = p->recycled ? &P.trash : &P.list;
        while (*pp && *pp != p)
            pp = &(*pp)->next;
        if (*pp) { /* not listed yet = creation failed; pin_add frees it */
            *pp = p->next;
            if (!p->recycled) {
                P.count--;
                pin_dirty();
            }
            p->hwnd = NULL;
            if (!p->saving)
                pin_free(p);
        }
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

bool pin_init(HINSTANCE inst, HWND daemon)
{
    P.inst = inst;
    P.daemon = daemon;
    P.top_all = true; /* until the settings say otherwise */
    WNDCLASSW wc = {0};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = pin_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_SIZEALL);
    wc.lpszClassName = L"sniplite.pin";
    return RegisterClassW(&wc) != 0;
}

static bool pin_create(Pin *p, int x, int y, int w, int h)
{
    /* WS_CAPTION (non-client area removed in WM_NCCALCSIZE) gives the native DWM shadow. */
    HWND hw = CreateWindowExW((P.top_all || p->top ? WS_EX_TOPMOST : 0) | WS_EX_TOOLWINDOW, L"sniplite.pin", L"sniplite",
                              WS_POPUP | WS_CAPTION, x, y, w, h, NULL, NULL, P.inst, p);
    if (!hw)
        return false;
    p->id = ++P.next_id;
    p->next = P.list;
    P.list = p;
    P.count++;
    MARGINS mg = {0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hw, &mg);
    DWORD corner = 1; /* DWMWCP_DONOTROUND */
    DwmSetWindowAttribute(hw, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    BOOL still = TRUE; /* it is there at once: no opening or closing animation */
    DwmSetWindowAttribute(hw, DWMWA_TRANSITIONS_FORCEDISABLED, &still, sizeof still);
    SetWindowPos(hw, NULL, x, y, w, h, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    apply_alpha(p);
    return true;
}

bool pin_add(Img *img, int x, int y, bool at_cursor)
{
    Pin *p = mem_alloc(sizeof *p);
    if (!p) {
        img_destroy(img);
        return false;
    }
    p->img = *img;
    memset(img, 0, sizeof *img);
    p->zoom = 1.f;
    p->alpha = 100;
    int w = p->img.w, h = p->img.h;
    if (at_cursor) {
        POINT c = {0, 0};
        GetCursorPos(&c);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
        RECT wa = mi.rcWork;
        float fit = fminf(0.9f * (float)(wa.right - wa.left) / (float)w, 0.9f * (float)(wa.bottom - wa.top) / (float)h);
        if (fit < 1.f) {
            p->zoom = fit;
            w = MAXI(8, (int)lroundf((float)w * fit));
            h = MAXI(8, (int)lroundf((float)h * fit));
        }
        x = CLAMPI((int)c.x - w / 2, (int)wa.left, (int)wa.right - w);
        y = CLAMPI((int)c.y - h / 2, (int)wa.top, (int)wa.bottom - h);
    }
    if (!pin_create(p, x, y, w, h)) {
        pin_free(p);
        return false;
    }
    pin_reveal(p, p->next, false);
    SetForegroundWindow(p->hwnd);
    pin_dirty();
    return true;
}

void pin_also_save(void) { also_save(P.list->img.px, P.list->img.w, P.list->img.h); } /* pin_add puts the newest first */

bool pin_any_visible(void)
{
    for (Pin *p = P.list; p; p = p->next)
        if (IsWindowVisible(p->hwnd))
            return true;
    return false;
}

void pin_toggle_all(void)
{
    if (pin_any_visible()) {
        for (Pin *p = P.list; p; p = p->next) {
            if (p->cropping)
                crop_end(p, false);
            p->hovered = false;
            apply_alpha(p);
            ShowWindow(p->hwnd, SW_HIDE);
        }
    } else if (P.list) {
        pin_reveal(P.list, NULL, false);
    }
    pin_dirty();
}

/* Like ScrapBook.ScrapClose, hide the closed pin and keep the newest ones up to the limit. */
static void pin_trim_trash(int limit)
{
    Pin **link = &P.trash;
    for (int i = 0; *link && i < limit; i++)
        link = &(*link)->next;
    while (*link)
        DestroyWindow((*link)->hwnd);
}

static void pin_close(Pin *p)
{
    if (p->recycled)
        return;
    if (!P.prefs.trash_limit) {
        DestroyWindow(p->hwnd);
        return;
    }
    Pin **link = &P.list;
    while (*link && *link != p)
        link = &(*link)->next;
    if (!*link)
        return;
    *link = p->next;
    P.count--;
    p->recycled = true;
    p->next = P.trash;
    P.trash = p;
    if (p->cropping)
        crop_end(p, false);
    if (GetCapture() == p->hwnd)
        ReleaseCapture();
    clear_label(p);
    p->hovered = false;
    ShowWindow(p->hwnd, SW_HIDE);
    pin_trim_trash(P.prefs.trash_limit);
    pin_dirty();
}

void pin_close_all(void)
{
    /* Close oldest first so the most recent pins are restored first. */
    Pin *oldest = NULL;
    while (P.list) {
        Pin *p = P.list;
        P.list = p->next;
        p->next = oldest;
        oldest = p;
    }
    P.list = oldest;
    while (P.list)
        pin_close(P.list);
}

int pin_count(void) { return P.count; }

/* The session follows CacheManager: store the open pictures, position and style,
 * then reopen them in layer order. The recycle bin is in memory for this run. */
enum { PIN_CACHE_MAGIC = 0x314e4950, PS_TOP = 1, PS_COMPACT = 2, PS_NO_BORDER = 4, PS_HIDDEN = 8 };
typedef struct {
    int32_t x, y, w, h, view_w, view_h, compact_x, compact_y;
    float zoom;
    uint32_t alpha, flags;
} PinRecord;

static bool cache_path(WCHAR *path, const WCHAR *name)
{
    config_path(path, MAX_PATH);
    WCHAR *slash = wcsrchr(path, L'\\');
    if (!slash || slash + 1 - path + lstrlenW(name) >= MAX_PATH)
        return false;
    lstrcpyW(slash + 1, name);
    return true;
}

static bool cache_remove(const WCHAR *name)
{
    WCHAR path[MAX_PATH];
    return cache_path(path, name) && (DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND);
}

static bool cache_io(HANDLE f, void *data, DWORD bytes, bool write)
{
    DWORD done;
    return (write ? WriteFile(f, data, bytes, &done, NULL) : ReadFile(f, data, bytes, &done, NULL)) && done == bytes;
}

static bool cache_write(void)
{
    if (!P.count)
        return cache_remove(PIN_CACHE_FILE);
    WCHAR path[MAX_PATH], temp[MAX_PATH];
    if (!cache_path(path, PIN_CACHE_FILE) || !cache_path(temp, PIN_CACHE_TEMP))
        return false;
    HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    uint32_t header[] = {PIN_CACHE_MAGIC, (uint32_t)P.count};
    bool ok = cache_io(f, header, sizeof header, true);
    GdiFlush();
    for (Pin *p = P.list; ok && p; p = p->next) {
        RECT r;
        if (!GetWindowRect(p->hwnd, &r)) {
            ok = false;
            break;
        }
        uint32_t flags = (p->top ? PS_TOP : 0) | (p->compact ? PS_COMPACT : 0) | (p->no_border ? PS_NO_BORDER : 0) |
                         (IsWindowVisible(p->hwnd) ? 0 : PS_HIDDEN);
        PinRecord v = {r.left, r.top, p->img.w, p->img.h, r.right - r.left, r.bottom - r.top,
                       p->compact_pt.x, p->compact_pt.y, p->zoom, (uint32_t)p->alpha, flags};
        DWORD bytes = (DWORD)((size_t)v.w * v.h * sizeof *p->img.px);
        ok = cache_io(f, &v, sizeof v, true) && cache_io(f, p->img.px, bytes, true);
    }
    ok = ok && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok)
        ok = MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok)
        DeleteFileW(temp);
    return ok;
}

static bool cache_record_valid(const PinRecord *v, int64_t remaining)
{
    if (v->w <= 0 || v->h <= 0 || (int64_t)v->w * v->h > (1LL << 28) ||
        !(v->zoom > 0.f && v->zoom <= 16.f) || v->w * v->zoom > 65535.f || v->h * v->zoom > 65535.f ||
        v->view_w < 1 || v->view_h < 1 || v->view_w > 65535 || v->view_h > 65535 ||
        v->alpha < 1 || v->alpha > 100 || v->flags & ~(PS_TOP | PS_COMPACT | PS_NO_BORDER | PS_HIDDEN) ||
        v->x < -1000000 || v->x > 1000000 || v->y < -1000000 || v->y > 1000000)
        return false;
    return (int64_t)v->w * v->h * (int64_t)sizeof(uint32_t) <= remaining;
}

static bool cache_add(HANDLE f, const PinRecord *v, DWORD bytes)
{
    Pin *p = mem_alloc(sizeof *p);
    if (!p)
        return false;
    p->zoom = v->zoom;
    p->alpha = (int)v->alpha;
    p->top = (v->flags & PS_TOP) != 0;
    p->compact = (v->flags & PS_COMPACT) != 0;
    p->no_border = (v->flags & PS_NO_BORDER) != 0;
    if (!img_create(&p->img, v->w, v->h) || !cache_io(f, p->img.px, bytes, false)) {
        pin_free(p);
        return false;
    }
    SIZE size = pin_size(p);
    p->compact_pt = (POINT){CLAMPI(v->compact_x, 0, size.cx - 1), CLAMPI(v->compact_y, 0, size.cy - 1)};
    int w = p->compact ? COMPACT_SIZE : v->view_w, h = p->compact ? COMPACT_SIZE : v->view_h;
    POINT at = pin_visible_at((RECT){v->x, v->y, v->x + w, v->y + h});
    if (!pin_create(p, at.x, at.y, w, h)) {
        pin_free(p);
        return false;
    }
    /* Show only after the whole file has been read and checked. */
    p->restore_hidden = (v->flags & PS_HIDDEN) != 0;
    return true;
}

static bool cache_read(void)
{
    WCHAR path[MAX_PATH];
    if (!cache_path(path, PIN_CACHE_FILE))
        return false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    uint32_t header[2];
    LARGE_INTEGER length = {0};
    bool ok = GetFileSizeEx(f, &length) && length.QuadPart >= (int64_t)sizeof header && cache_io(f, header, sizeof header, false) &&
              header[0] == PIN_CACHE_MAGIC;
    int64_t remaining = length.QuadPart - (int64_t)sizeof header;
    for (uint32_t i = 0; ok && i < header[1]; i++) {
        PinRecord v;
        if (remaining < (int64_t)sizeof v || !cache_io(f, &v, sizeof v, false) ||
            !cache_record_valid(&v, remaining - (int64_t)sizeof v)) {
            ok = false;
            break;
        }
        DWORD bytes = (DWORD)((size_t)v.w * v.h * sizeof(uint32_t));
        remaining -= sizeof v + bytes;
        ok = cache_add(f, &v, bytes);
    }
    CloseHandle(f);
    if (!ok || remaining) {
        while (P.list)
            DestroyWindow(P.list->hwnd);
        return false;
    }
    pin_reveal(P.list, NULL, true);
    Pin *first = NULL;
    while (P.list) { /* back to front, then keep the list in its original order */
        Pin *p = P.list;
        P.list = p->next;
        SetWindowPos(p->hwnd, P.top_all || p->top ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        p->next = first;
        first = p;
    }
    P.list = first;
    return true;
}

void pin_flush_session(void)
{
    if (P.daemon)
        KillTimer(P.daemon, TIMER_PIN_SESSION);
    if (P.dirty && P.prefs.restore_session) {
        P.cache_failed = !cache_write();
        P.dirty = P.cache_failed;
    }
}

static void CALLBACK cache_timer(HWND h, UINT msg, UINT_PTR id, DWORD now)
{
    pin_flush_session();
}

static void pin_dirty(void)
{
    if (P.started && P.prefs.restore_session) {
        P.dirty = true;
        SetTimer(P.daemon, TIMER_PIN_SESSION, 450, cache_timer);
    }
}

void pin_configure(const Config *c)
{
    bool restore = P.prefs.restore_session;
    P.prefs = c->pins;
    P.text = c->pin_text;
    P.list_key = (KeyBinding){c->mod[HK_PIN_LIST], c->vk[HK_PIN_LIST]};
    P.undo_key = (KeyBinding){c->mod[HK_PIN_UNDO], c->vk[HK_PIN_UNDO]};
    pin_top_all(c->pin_top);
    pin_trim_trash(P.prefs.trash_limit);
    for (Pin *p = P.list; p; p = p->next) {
        apply_alpha(p);
        InvalidateRect(p->hwnd, NULL, FALSE);
    }
    if (P.started && restore != P.prefs.restore_session) {
        if (P.prefs.restore_session) {
            pin_dirty();
        } else {
            KillTimer(P.daemon, TIMER_PIN_SESSION);
            P.dirty = false;
            P.cache_failed = !cache_remove(PIN_CACHE_FILE);
            cache_remove(PIN_CACHE_TEMP);
        }
    }
}

void pin_start_session(void)
{
    P.cache_failed = P.prefs.restore_session ? !cache_read() : !cache_remove(PIN_CACHE_FILE);
    cache_remove(PIN_CACHE_TEMP);
    P.started = true;
}

void pin_shutdown(void)
{
    pin_flush_session();
    P.started = false;
    while (P.list)
        DestroyWindow(P.list->hwnd);
    pin_trim_trash(0);
}
