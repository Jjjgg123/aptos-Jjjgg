#ifndef APTOS_LST_H
#define APTOS_LST_H

#include "types.h"

#define LST_MAGIC "APTOSLST"
#define LST_HEADER_SIZE 128
#define LST_ENTRY_SIZE  64
#define LST_FLAG_APP    1
#define LST_FLAG_DATA   2

typedef struct PACKED {
    char magic[8];
    u32  version;
    u32  count;
    u32  installer;
    u32  flags;
    char name[32];
    char vendor[32];
    char description[24];
} lst_header_t;

typedef struct PACKED {
    char name[32];
    u32  offset;
    u32  size;
    u32  flags;
    u8   reserved[LST_ENTRY_SIZE - 44];
} lst_entry_t;

int  lst_validate(const void *data, u32 size);
const lst_header_t *lst_header(const void *data);
int  lst_count(const void *data);
const lst_entry_t *lst_entry(const void *data, u32 size, u32 index);
const u8 *lst_payload(const void *data, u32 size, u32 index, u32 *len);

#endif
