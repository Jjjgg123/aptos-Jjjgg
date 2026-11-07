#include "elf.h"
#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "util.h"

typedef struct PACKED {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u32 entry;
    u32 phoff;
    u32 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf32_hdr_t;

typedef struct PACKED {
    u32 type;
    u32 offset;
    u32 vaddr;
    u32 paddr;
    u32 filesz;
    u32 memsz;
    u32 flags;
    u32 align;
} elf32_phdr_t;

#define PT_LOAD 1

typedef struct {
    u32 vaddr;
    u32 pages;
} elf_region_t;

#define ELF_MAX_REGIONS 8
static elf_region_t regions[ELF_MAX_REGIONS];
static int region_count;

int elf_valid(const void *image, u32 image_size)
{
    const elf32_hdr_t *h = (const elf32_hdr_t *)image;

    if (image_size < sizeof(elf32_hdr_t))
        return 0;
    if (h->ident[0] != 0x7F || h->ident[1] != 'E' || h->ident[2] != 'L' ||
        h->ident[3] != 'F')
        return 0;
    if (h->ident[4] != 1 || h->ident[5] != 1)
        return 0;
    if (h->machine != 3 || h->type != 2)
        return 0;
    return 1;
}

int elf_load(const void *image, u32 image_size, elf_image_t *out)
{
    const elf32_hdr_t *h = (const elf32_hdr_t *)image;
    const elf32_phdr_t *ph;
    u32 lo = 0xFFFFFFFFu, hi = 0;

    if (!elf_valid(image, image_size))
        return -1;

    ph = (const elf32_phdr_t *)((const u8 *)image + h->phoff);
    region_count = 0;

    for (int i = 0; i < h->phnum; i++) {
        u32 vstart, vend, pages, phys;
        u32 foff, fsize;

        if (ph[i].type != PT_LOAD)
            continue;
        if (ph[i].memsz == 0)
            continue;

        foff = ph[i].offset;
        fsize = ph[i].filesz;
        if (foff + fsize > image_size)
            return -2;

        vstart = ph[i].vaddr & ~0xFFFu;
        vend = (ph[i].vaddr + ph[i].memsz + 0xFFFu) & ~0xFFFu;
        pages = (vend - vstart) / PAGE_SIZE;
        if (pages == 0)
            continue;
        if (region_count >= ELF_MAX_REGIONS)
            return -3;

        phys = (u32)kmalloc_a(pages * PAGE_SIZE, PAGE_SIZE);
        if (!phys)
            return -4;
        memset((void *)phys, 0, pages * PAGE_SIZE);
        memcpy((void *)(phys + (ph[i].vaddr - vstart)),
               (const u8 *)image + foff, fsize);

        if (paging_map_user(vstart, phys, pages * PAGE_SIZE, 1) != 0)
            return -5;

        regions[region_count].vaddr = vstart;
        regions[region_count].pages = pages;
        region_count++;

        if (vstart < lo)
            lo = vstart;
        if (vend > hi)
            hi = vend;
    }

    if (hi == 0)
        return -6;

    out->entry = h->entry;
    out->base = lo;
    out->size = hi - lo;
    out->pages = (hi - lo) / PAGE_SIZE;
    return 0;
}

void elf_unload(const elf_image_t *img)
{
    (void)img;
    for (int i = 0; i < region_count; i++)
        paging_unmap_user(regions[i].vaddr, regions[i].pages * PAGE_SIZE);
    region_count = 0;
}
