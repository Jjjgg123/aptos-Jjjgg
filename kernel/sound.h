#ifndef APTOS_SOUND_H
#define APTOS_SOUND_H

#include "types.h"

void speaker_on(u32 freq);
void speaker_off(void);
void beep(u32 freq, u32 ms);

#endif
