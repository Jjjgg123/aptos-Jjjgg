#ifndef APTOS_GPU_H
#define APTOS_GPU_H

#include "types.h"

/* 探测显示设备(PCI class 0x03)与可用的加速后端。
   不论探测结果如何都不会改变显示模式,失败时调用方继续用 VBE 帧缓冲。 */
void gpu_init(void);

#endif
