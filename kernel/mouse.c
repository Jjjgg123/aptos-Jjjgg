#include "mouse.h"
#include "idt.h"
#include "io.h"
#include "klog.h"
#include "util.h"

static volatile int mx, my;
static volatile int buttons;
static int bound_w = 1024, bound_h = 768;
static int ready;

static u8 packet[3];
static int packet_index;

#define MOUSE_EVQ 32
typedef struct {
    u8  type;
    u8  buttons;
    int x;
    int y;
} mouse_ev_t;

static volatile mouse_ev_t evq[MOUSE_EVQ];
static volatile u32 ev_head, ev_tail;
static int prev_buttons;

static void push_event(int type, int x, int y, int buttons)
{
    u32 next = (ev_head + 1) % MOUSE_EVQ;
    if (next == ev_tail)
        return;
    evq[ev_head].type = (u8)type;
    evq[ev_head].buttons = (u8)buttons;
    evq[ev_head].x = x;
    evq[ev_head].y = y;
    ev_head = next;
}

static void ctrl_wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 0x02))
            return;
}

static void ctrl_wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 0x01)
            return;
}

static void mouse_write(u8 value)
{
    ctrl_wait_write();
    outb(0x64, 0xD4);
    ctrl_wait_write();
    outb(0x60, value);
}

static u8 mouse_read(void)
{
    ctrl_wait_read();
    return inb(0x60);
}

static void mouse_byte(u8 b)
{
    if (packet_index == 0 && !(b & 0x08))
        return;

    packet[packet_index++] = b;
    if (packet_index < 3)
        return;
    packet_index = 0;

    u8 flags = packet[0];
    int dx = (int)packet[1];
    int dy = (int)packet[2];

    if (flags & 0x10) dx -= 256;
    if (flags & 0x20) dy -= 256;

    mx += dx;
    my -= dy;

    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > bound_w - 1) mx = bound_w - 1;
    if (my > bound_h - 1) my = bound_h - 1;

    buttons = 0;
    if (flags & 0x01) buttons |= MOUSE_LEFT;
    if (flags & 0x02) buttons |= MOUSE_RIGHT;
    if (flags & 0x04) buttons |= MOUSE_MIDDLE;

    if ((buttons & MOUSE_LEFT) != (prev_buttons & MOUSE_LEFT))
        push_event((buttons & MOUSE_LEFT) ? MOUSE_EV_DOWN : MOUSE_EV_UP, mx, my, buttons);
    prev_buttons = buttons;
}

int mouse_next_event(int *x, int *y, int *btns)
{
    int type = 0;
    u32 flags = irq_save();

    if (ev_tail != ev_head) {
        type = evq[ev_tail].type;
        if (x) *x = evq[ev_tail].x;
        if (y) *y = evq[ev_tail].y;
        if (btns) *btns = evq[ev_tail].buttons;
        ev_tail = (ev_tail + 1) % MOUSE_EVQ;
    }
    irq_restore(flags);
    return type;
}

static void mouse_irq(regs_t *r)
{
    (void)r;

    for (int i = 0; i < 64; i++) {
        u8 status = inb(0x64);
        if (!(status & 0x01))
            break;
        if (!(status & 0x20))
            break;
        mouse_byte(inb(0x60));
    }
}

void mouse_init(void)
{
    packet_index = 0;
    buttons = 0;
    prev_buttons = 0;
    ev_head = ev_tail = 0;

    ctrl_wait_write();
    outb(0x64, 0xA8);

    ctrl_wait_write();
    outb(0x64, 0x20);
    u8 ccb = mouse_read();

    ccb |= 0x07;
    ccb &= (u8)~0x08;
    ccb &= (u8)~0x30;
    ccb |= 0x40;

    ctrl_wait_write();
    outb(0x64, 0x60);
    ctrl_wait_write();
    outb(0x60, ccb);

    mouse_write(0xF6);
    (void)mouse_read();
    mouse_write(0xF4);
    u8 ack = mouse_read();
    ready = (ack == 0xFA);

    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++)
        (void)inb(0x60);

    irq_register(12, mouse_irq);
    irq_unmask(12);

    if (ready)
        kprintf("[mouse] PS/2 mouse enabled\n");
    else
        kprintf("[mouse] PS/2 mouse did not ACK (got 0x%x)\n", ack);
}

void mouse_set_bounds(int width, int height)
{
    bound_w = width;
    bound_h = height;
    if (mx >= bound_w) mx = bound_w - 1;
    if (my >= bound_h) my = bound_h - 1;
}

void mouse_set_position(int x, int y)
{
    mx = x;
    my = y;
}

void mouse_get(int *x, int *y, int *btns)
{
    if (x) *x = mx;
    if (y) *y = my;
    if (btns) *btns = buttons;
}

int mouse_x(void) { return mx; }
int mouse_y(void) { return my; }
int mouse_buttons(void) { return buttons; }
int mouse_ready(void) { return ready; }
