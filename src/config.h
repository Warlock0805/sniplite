/* Settings (sniplite.ini next to the exe) and hotkey text <-> key codes. */
#pragma once
#include "common.h"

enum { HK_CAPTURE = 1, HK_QUICK, HK_PIN, HK_TOGGLE, HK_OCR, HK_OCRCOPY, HK_PIN_LIST, HK_PIN_UNDO, HK_COUNT };
enum {
    PK_CLOSE, PK_COMPACT, PK_COPY, PK_CUT, PK_SAVE, PK_PASTE, PK_ROTATE, PK_FLIP_H, PK_FLIP_V, PK_CROP, PK_BORDER,
    PK_ZOOM_IN, PK_ZOOM_OUT, PK_ALPHA_MORE, PK_ALPHA_LESS, PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT,
    PK_Z50, PK_Z60, PK_Z70, PK_Z80, PK_Z90, PK_Z100, PK_COUNT
};
enum { PIN_TRASH_MAX = 50 };
enum { THEME_AUTO, THEME_LIGHT, THEME_DARK }; /* the capture's bars: as Windows is set, or always one */
enum { NAME_COUNT = 7, NAME_CAP = 32 };       /* the file names to choose from; room for the longest, with its 0 */

typedef struct { UINT mod, vk; } KeyBinding;

typedef struct {
    KeyBinding key[PK_COUNT];
    int trash_limit;          /* 0 disables the recycle bin */
    int inactive, hover;      /* opacity in percent; 0 uses the pin's own opacity */
    bool compact_on_double_click;
    bool restore_session;
    bool inactive_outline;
} PinPrefs;

extern const int pin_key_text[PK_COUNT]; /* localized names, indexed by PK_* */

typedef struct {
    UINT mod[HK_COUNT], vk[HK_COUNT]; /* index HK_*; vk 0 = no hotkey */
    PinPrefs pins;
    bool gdi;                         /* GDI capture instead of DXGI */
    bool tray;                        /* show the notification-area icon */
    bool pin_top;                     /* every pin stays above other windows */
    bool pin_text;                    /* a clipboard that holds text and no picture can be pinned too */
    bool auto_update;                 /* look for a newer release after start-up, then daily */
    bool exit_ask;                    /* ask before a capture with a selection is left (Esc, the close button) */
    bool also_save;                   /* a capture that is copied or pinned goes into a file as well */
    bool block_fullscreen;            /* no hotkeys while the window in front fills its screen */
    int theme;                        /* THEME_* */
    int detect;                       /* DETECT_* (ov_layout.inc) */
    int name;                         /* which of the NAME_COUNT file names (name_format) */
    WCHAR save_dir[MAX_PATH];         /* default save location; empty: the desktop */
    WCHAR last_dir[MAX_PATH];         /* the folder of the file last saved through the save dialog */
    WCHAR block_apps[256];            /* no hotkeys while a program of these is in front: file names, | between them */
} Config;

void config_defaults(Config *c);             /* Snipaste-compatible: F1 / F3 / Shift+F3; Shift+F1 quick; for text F4 / Shift+F4 */
bool config_load(Config *c);                 /* defaults + sniplite.ini if present */
bool config_save(const Config *c);
void config_set_pin_top(bool on);            /* that one setting, changed from a pin's menu: into the file */
void config_set_exit_ask(bool on);           /* and that one, from the question itself */
void config_set_last_dir(const WCHAR *dir);  /* and the save dialog's folder, after it closed */
void config_path(WCHAR *out, int cap);
/* The name of a file made at that time, without its ending. out: NAME_CAP characters; returns its length. */
int name_format(int name, const SYSTEMTIME *t, WCHAR *out);

bool hotkey_parse(const WCHAR *s, UINT *mod, UINT *vk); /* "Ctrl+Alt+A", "F1", "none" */
void hotkey_format(UINT mod, UINT vk, WCHAR *out);      /* out: >= 40 chars; "none" if vk == 0 */
