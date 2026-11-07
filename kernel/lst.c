#include "lst.h"
#include "util.h"

int lst_validate(const void *data, u32 size)
{
    const lst_header_t *h = (const lst_header_t *)data;

    if (size < LST_HEADER_SIZE)
        return 0;
    if (memcmp(h->magic, LST_MAGIC, 8) != 0)
        return 0;
    if (h->version != 1)
        return 0;
    if (h->count == 0 || h->count > 64)
        return 0;
    if (h->installer >= h->count)
        return 0;
    if (LST_HEADER_SIZE + h->count * LST_ENTRY_SIZE > size)
        return 0;
    return 1;
}

const lst_header_t *lst_header(const void *data)
{
    return (const lst_header_t *)data;
}

int lst_count(const void *data)
{
    return (int)lst_header(data)->count;
}

const lst_entry_t *lst_entry(const void *data, u32 size, u32 index)
{
    const lst_header_t *h = (const lst_header_t *)data;
    const lst_entry_t *e;

    if (index >= h->count)
        return 0;
    e = (const lst_entry_t *)((const u8 *)data + LST_HEADER_SIZE +
                              index * LST_ENTRY_SIZE);
    if (e->offset + e->size > size)
        return 0;
    return e;
}

const u8 *lst_payload(const void *data, u32 size, u32 index, u32 *len)
{
    const lst_entry_t *e = lst_entry(data, size, index);
    if (!e)
        return 0;
    if (len)
        *len = e->size;
    return (const u8 *)data + e->offset;
}
