/* Pinned images: borderless topmost windows. */
#pragma once
#include "common.h"
#include "config.h"

#define PIN_CACHE_FILE L"sniplite.pins"
#define PIN_CACHE_TEMP L"sniplite.pins.tmp"

typedef struct { HMENU list, trash; } PinMenus;

bool pin_init(HINSTANCE inst, HWND daemon);
void pin_configure(const Config *c);
void pin_start_session(void);
void pin_flush_session(void);
void pin_shutdown(void);
/* Takes ownership of img. at_cursor: center on the cursor and shrink to fit the monitor;
 * otherwise the top-left goes to (x, y) at 100%. */
bool pin_add(Img *img, int x, int y, bool at_cursor);
void pin_also_save(void); /* right after pin_add pinned a capture: also_save for its picture */
void pin_toggle_all(void);
bool pin_any_visible(void);
void pin_close_all(void);
int pin_count(void);
PinMenus pin_menus_add(HMENU parent);
bool pin_menus_run(PinMenus menus, UINT cmd);
void pin_menus_dispose(PinMenus menus);
void pin_list_menu(void);
void pin_restore_last(void);
