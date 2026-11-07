/* sound.c —— PC Speaker 驱动（8254 通道 2 方波）
 * 硬件：PIT ch2 (0x42) + 端口 0x61 闸门
 * VBox 不输出 PC Speaker 声音（静音），但 I/O 路径与真实硬件一致 */
#include "sound.h"
#include "io.h"
#include "timer.h"

#define PIT_FREQ 1193182u
#define SND_CMD  0x43
#define SND_CH2  0x42
#define SND_GATE 0x61

void speaker_on(u32 freq)
{
    if (freq == 0 || freq > 20000)
        freq = 1000;
    u32 divisor = PIT_FREQ / freq;
    if (divisor == 0) divisor = 1;
    outb(SND_CMD, 0xB6);                       /* ch2, 方波, LSB then MSB */
    outb(SND_CH2, (u8)(divisor & 0xFF));
    outb(SND_CH2, (u8)((divisor >> 8) & 0xFF));
    u8 v = inb(SND_GATE);
    outb(SND_GATE, (u8)((v | 3) & 0xFF));      /* gate + speaker enable */
}

void speaker_off(void)
{
    u8 v = inb(SND_GATE);
    outb(SND_GATE, (u8)(v & ~3));
}

void beep(u32 freq, u32 ms)
{
    speaker_on(freq);
    timer_sleep_ms(ms);
    speaker_off();
}
