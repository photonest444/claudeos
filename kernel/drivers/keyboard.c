/* PS/2 keyboard (scan code set 1, US layout). Translates scan codes to ASCII
 * or KEY_* codes and routes them to the CLI console or the GUI.
 * Hotkeys: F12 toggles CLI <-> GUI, Ctrl+Alt+Del reboots. */
#include "../include/drivers.h"
#include "../include/io.h"
#include "../include/arch.h"
#include "../include/tty.h"
#include "../include/gui.h"
#include "../include/clipboard.h"

static const char map_normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ',
};
static const char map_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ',
};

static bool shift, ctrl, alt, caps, e0;

void keyboard_route(int key) {
    if (display_mode == DISPLAY_GUI) { gui_post_key(key); return; }
    if (!console_tty || key == KEY_ALT_TAB || key == KEY_SUPER) return;
    if (key == KEY_CTRL_V) tty_paste(console_tty);       /* paste into the CLI */
    else tty_input(console_tty, key & ~KEY_SHIFT);
}

static void handle_scancode(uint8_t sc) {
    if (sc == 0xE0) { e0 = true; return; }
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;
    bool ext = e0;
    e0 = false;

    switch (code) {                       /* modifiers */
    case 0x2A: case 0x36: shift = !release; return;
    case 0x1D: ctrl = !release; return;
    case 0x38: alt = !release; return;
    case 0x3A: if (!release) caps = !caps; return;
    }
    if (release) return;

    int key = 0;
    if (ext) {
        switch (code) {
        case 0x48: key = KEY_UP; break;     case 0x50: key = KEY_DOWN; break;
        case 0x4B: key = KEY_LEFT; break;   case 0x4D: key = KEY_RIGHT; break;
        case 0x47: key = KEY_HOME; break;   case 0x4F: key = KEY_END; break;
        case 0x49: key = KEY_PGUP; break;   case 0x51: key = KEY_PGDN; break;
        case 0x52: key = KEY_INSERT; break; case 0x53: key = KEY_DELETE; break;
        case 0x1C: key = '\n'; break;       /* keypad enter */
        case 0x35: key = '/'; break;
        case 0x5B: case 0x5C: key = KEY_SUPER; break;   /* Windows/Super key */
        default: return;
        }
        if (shift && key >= KEY_UP && key <= KEY_DELETE) key |= KEY_SHIFT;
    } else if (code >= 0x3B && code <= 0x44) {
        key = KEY_F1 + (code - 0x3B);
    } else if (code == 0x57) {
        key = KEY_F11;
    } else if (code == 0x58) {
        key = KEY_F12;
    } else {
        char c = shift ? map_shift[code] : map_normal[code];
        if (!c) return;
        if (caps && c >= 'a' && c <= 'z') c -= 32;
        else if (caps && c >= 'A' && c <= 'Z') c += 32;
        if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            c = (c & 0x1F);               /* Ctrl+C -> 3, Ctrl+L -> 12, ... */
        key = (uint8_t)c;
    }

    if (ctrl && alt && key == KEY_DELETE) power_reboot();
    if (alt && key == '\t') key = KEY_ALT_TAB;
    if (key == KEY_F12) { display_toggle(); return; }
    keyboard_route(key);
}

static void keyboard_irq(regs_t *r) {
    UNUSED(r);
    /* The controller shares port 0x60 with the mouse: bit 5 = aux data. */
    while (inb(0x64) & 1) {
        uint8_t status = inb(0x64);
        if (status & 0x20) return;        /* mouse byte, leave it for IRQ12 */
        handle_scancode(inb(0x60));
    }
}

void keyboard_init(void) {
    while (inb(0x64) & 1) inb(0x60);      /* flush stale bytes */
    irq_install(1, keyboard_irq);
}
