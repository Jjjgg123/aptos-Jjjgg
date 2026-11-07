#include "keyboard.h"
#include "idt.h"
#include "io.h"
#include "klog.h"
#include "util.h"

#define KBD_BUF_SIZE 128

static volatile int  buf[KBD_BUF_SIZE];
static volatile u32  buf_head, buf_tail;
static u32 mods;
static int extended;

static const char map_normal[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,  'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static const char map_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,  'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,  '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static void push(int key)
{
    u32 next = (buf_head + 1) % KBD_BUF_SIZE;
    if (next == buf_tail)
        return;
    buf[buf_head] = key;
    buf_head = next;
}

static void keyboard_byte(u8 sc)
{

    if (sc == 0xE0) {
        extended = 1;
        return;
    }
    if (sc == 0xE1) {
        extended = 2;
        return;
    }
    if (extended == 2) {
        if (sc == 0x1D)
            extended = 3;
        else if (sc == 0x9D)
            extended = 0;
        return;
    }
    if (extended == 3) {
        extended = 0;
        return;
    }

    int release = sc & 0x80;
    u8 code = sc & 0x7F;

    if (extended) {
        extended = 0;
        if (!release) {
            switch (code) {
            case 0x48: push(KEY_UP); break;
            case 0x50: push(KEY_DOWN); break;
            case 0x4B: push(KEY_LEFT); break;
            case 0x4D: push(KEY_RIGHT); break;
            case 0x47: push(KEY_HOME); break;
            case 0x4F: push(KEY_END); break;
            case 0x53: push(KEY_DELETE); break;
            case 0x49: push(KEY_PGUP); break;
            case 0x51: push(KEY_PGDN); break;
            case 0x1D: mods |= KMOD_CTRL; break;
            case 0x38: mods |= KMOD_ALT; break;
            default: break;
            }
        } else {
            if (code == 0x1D) mods &= ~KMOD_CTRL;
            if (code == 0x38) mods &= ~KMOD_ALT;
        }
        return;
    }

    switch (code) {
    case 0x2A: case 0x36:
        if (release) mods &= ~KMOD_SHIFT; else mods |= KMOD_SHIFT;
        return;
    case 0x1D:
        if (release) mods &= ~KMOD_CTRL; else mods |= KMOD_CTRL;
        return;
    case 0x38:
        if (release) mods &= ~KMOD_ALT; else mods |= KMOD_ALT;
        return;
    case 0x3A:
        if (!release) mods ^= KMOD_CAPS;
        return;
    default:
        break;
    }

    if (release || code >= 128)
        return;

    char c = (mods & KMOD_SHIFT) ? map_shift[code] : map_normal[code];
    if ((mods & KMOD_CAPS) && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if ((mods & KMOD_CAPS) && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');

    if (mods & KMOD_CTRL) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);
    }

    if (c)
        push((int)(u8)c);
}

static void keyboard_irq(regs_t *r)
{
    (void)r;
    for (int i = 0; i < 64; i++) {
        u8 status = inb(0x64);
        if (!(status & 0x01))
            break;
        if (status & 0x20)
            break;
        keyboard_byte(inb(0x60));
    }
}

void keyboard_init(void)
{
    buf_head = buf_tail = 0;
    mods = 0;
    extended = 0;

    while (inb(0x64) & 0x01)
        (void)inb(0x60);

    irq_register(1, keyboard_irq);
    irq_unmask(1);

    {
        u8 st = inb(0x64);
        if (st == 0xFF)
            kprintf("[kbd] WARNING: no PS/2 controller (0x64 reads 0xFF)\n");
        else
            kprintf("[kbd] PS/2 keyboard ready (scancode set 1)\n");
    }
}

u32 keyboard_mods(void) { return mods; }

int keyboard_has_key(void) { return buf_head != buf_tail; }

int keyboard_getkey(void)
{
    if (buf_head == buf_tail)
        return -1;
    int k = buf[buf_tail];
    buf_tail = (buf_tail + 1) % KBD_BUF_SIZE;
    return k;
}
