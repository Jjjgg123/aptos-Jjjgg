#ifndef APTOS_ELF_H
#define APTOS_ELF_H

#include "types.h"

typedef struct {
    u32 entry;
    u32 base;
    u32 size;
    u32 pages;
} elf_image_t;

int elf_load(const void *image, u32 image_size, elf_image_t *out);
void elf_unload(const elf_image_t *img);
int elf_valid(const void *image, u32 image_size);

#endif
