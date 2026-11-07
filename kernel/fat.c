#include "fat.h"
#include "blkdev.h"
#include "klog.h"
#include "rtc.h"
#include "util.h"

#define ATTR_RO   0x01
#define ATTR_HID  0x02
#define ATTR_SYS  0x04
#define ATTR_VOL  0x08
#define ATTR_DIR  0x10
#define ATTR_LFN  0x0F

#define EOC12 0x0FF8
#define EOC16 0xFFF8
#define EOC32 0x0FFFFFF8

#define DIR_MAX_SECTORS 512

typedef struct PACKED {
    char name[11];
    u8  attr;
    u8  ntres;
    u8  crt_tenth;
    u16 crt_time;
    u16 crt_date;
    u16 acc_date;
    u16 cls_hi;
    u16 wrt_time;
    u16 wrt_date;
    u16 cls_lo;
    u32 size;
} fat_ent_t;

typedef struct {
    u32 c;
    int is_root;
} fat_dir_t;

static fat_state_t *cur;
static u32 dir_sectors[DIR_MAX_SECTORS];

static int rd(u32 lba, void *buf, u32 count)
{
    return blk_read(cur->dev, lba, count, buf);
}

static int wr(u32 lba, const void *buf, u32 count)
{
    return blk_write(cur->dev, lba, count, buf);
}

static int pread(u32 lba, u32 off, void *out, u32 n)
{
    u8 buf[512];
    while (n) {
        u32 so = off % 512;
        u32 take = 512 - so;
        if (take > n)
            take = n;
        if (rd(lba + off / 512, buf, 1))
            return -1;
        memcpy(out, buf + so, take);
        off += take;
        out = (u8 *)out + take;
        n -= take;
    }
    return 0;
}

static int pwrite(u32 lba, u32 off, const void *in, u32 n)
{
    u8 buf[512];
    while (n) {
        u32 so = off % 512;
        u32 take = 512 - so;
        if (take > n)
            take = n;
        if (rd(lba + off / 512, buf, 1))
            return -1;
        memcpy(buf + so, in, take);
        if (wr(lba + off / 512, buf, 1))
            return -1;
        off += take;
        in = (const u8 *)in + take;
        n -= take;
    }
    return 0;
}

static u32 eoc_val(void)
{
    if (cur->type == FAT_TYPE_12)
        return 0xFFF;
    if (cur->type == FAT_TYPE_16)
        return 0xFFFF;
    return 0x0FFFFFFF;
}

static int is_eoc(u32 v)
{
    if (cur->type == FAT_TYPE_12)
        return (v & 0xFFF) >= EOC12;
    if (cur->type == FAT_TYPE_16)
        return v >= EOC16;
    return v >= EOC32;
}

static int fat_get(u32 c, u32 *v)
{
    u32 off;
    if (c < 2)
        return -1;
    if (c >= cur->clusters + 2)
        return -1;
    if (cur->type == FAT_TYPE_12) {
        u16 raw;
        off = c + (c >> 1);
        if (pread(cur->base + cur->rsvd, off, &raw, 2))
            return -1;
        *v = (c & 1) ? (raw >> 4) : (raw & 0xFFF);
    } else if (cur->type == FAT_TYPE_16) {
        u16 raw;
        if (pread(cur->base + cur->rsvd, c * 2, &raw, 2))
            return -1;
        *v = raw;
    } else {
        u32 raw;
        if (pread(cur->base + cur->rsvd, c * 4, &raw, 4))
            return -1;
        *v = raw & 0x0FFFFFFF;
    }
    return 0;
}

static int fat_set(u32 c, u32 v)
{
    u32 off;
    if (c < 2)
        return -1;
    for (int f = 0; f < cur->nfat; f++) {
        u32 fbase = cur->base + cur->rsvd + (u32)f * cur->fat_sz;
        if (cur->type == FAT_TYPE_12) {
            u16 raw;
            off = c + (c >> 1);
            if (pread(fbase, off, &raw, 2))
                return -1;
            if (c & 1)
                raw = (u16)((raw & 0x000F) | (v << 4));
            else
                raw = (u16)((raw & 0xF000) | v);
            if (pwrite(fbase, off, &raw, 2))
                return -1;
        } else if (cur->type == FAT_TYPE_16) {
            u16 raw = (u16)v;
            if (pwrite(fbase, c * 2, &raw, 2))
                return -1;
        } else {
            u32 raw = v;
            if (pwrite(fbase, c * 4, &raw, 4))
                return -1;
        }
    }
    return 0;
}

static int cl_lba(u32 c, u32 *lba)
{
    if (c < 2 || c >= cur->clusters + 2)
        return -1;
    *lba = cur->first_data + (c - 2) * (u32)cur->spc;
    return 0;
}

static int next_cluster(u32 c, u32 *next)
{
    u32 v;
    if (fat_get(c, &v))
        return -1;
    if (is_eoc(v))
        return 0;
    if (v == 0 || v == 1)
        return -1;
    *next = v;
    return 1;
}

static int dir_expand(u32 c, int is_root, u32 *count)
{
    u32 n = 0;
    if (is_root && cur->type != FAT_TYPE_32) {
        u32 root_lba = cur->base + cur->rsvd + (u32)cur->nfat * cur->fat_sz;
        for (u32 i = 0; i < cur->root_sectors && n < DIR_MAX_SECTORS; i++)
            dir_sectors[n++] = root_lba + i;
        *count = n;
        return 0;
    }
    if (is_root)
        c = cur->root_cluster;
    while (c) {
        u32 lba;
        int r;
        if (cl_lba(c, &lba))
            return -1;
        for (u32 i = 0; i < (u32)cur->spc && n < DIR_MAX_SECTORS; i++)
            dir_sectors[n++] = lba + i;
        r = next_cluster(c, &c);
        if (r != 1)
            break;
    }
    *count = n;
    return 0;
}

static void fat_to_name(const char f[11], char *out)
{
    int i, j = 0;
    int has_ext = 0;
    for (i = 8; i < 11; i++)
        if (f[i] && f[i] != ' ')
            has_ext = 1;
    for (i = 0; i < 8 && f[i] && f[i] != ' '; i++)
        out[j++] = f[i];
    if (has_ext) {
        out[j++] = '.';
        for (i = 8; i < 11 && f[i] && f[i] != ' '; i++)
            out[j++] = f[i];
    }
    out[j] = 0;
}

static int u_toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 'A';
    return c;
}

static int name_to_fat(const char *name, char f[11])
{
    int i, bl = 0, el = 0;
    const char *dot = strchr(name, '.');
    char base[9], ext[4];

    memset(f, ' ', 11);
    base[0] = 0;
    ext[0] = 0;
    for (i = 0; i < 8 && name[i] && name[i] != '.'; i++)
        base[bl++] = name[i];
    if (dot)
        for (i = 0; i < 3 && dot[i + 1]; i++)
            ext[el++] = dot[i + 1];
    if (!bl)
        return -1;
    for (i = 0; i < bl; i++)
        f[i] = (char)u_toupper(base[i]);
    for (i = 0; i < el; i++)
        f[8 + i] = (char)u_toupper(ext[i]);
    return 0;
}

static void ent_to_dirent(const fat_ent_t *e, fs_dirent_t *d)
{
    u32 cl;
    memset(d, 0, sizeof(*d));
    fat_to_name(e->name, d->name);
    cl = e->cls_lo | ((u32)e->cls_hi << 16);
    d->first_block = cl;
    d->flags = (e->attr & ATTR_DIR) ? FS_FLAG_DIR : 0;
    d->size = e->size;
    d->mdate = e->wrt_date;
    d->mtime = e->wrt_time;
}

static int lookup_in(const char *name, u32 c, int is_root, fat_ent_t *e,
                     u32 *secidx, u32 *slot)
{
    u32 n;
    if (dir_expand(c, is_root, &n))
        return -1;
    for (u32 s = 0; s < n; s++) {
        u8 buf[512];
        if (rd(dir_sectors[s], buf, 1))
            return -1;
        for (int i = 0; i < 16; i++) {
            fat_ent_t *en = (fat_ent_t *)(buf + i * 32);
            char disp[40];
            if (en->name[0] == 0)
                return 0;
            if ((u8)en->name[0] == 0xE5)
                continue;
            if (en->attr == ATTR_LFN)
                continue;
            if (en->attr & ATTR_VOL)
                continue;
            if (en->name[0] == '.' && (en->name[1] == ' ' || en->name[1] == '.'))
                continue;
            fat_to_name(en->name, disp);
            if (strcasecmp(disp, name) == 0) {
                if (e)
                    *e = *en;
                if (secidx)
                    *secidx = s;
                if (slot)
                    *slot = (u32)i;
                return 1;
            }
        }
    }
    return 0;
}

static int resolve_entry(const char *path, fat_ent_t *e, fat_dir_t *dctx,
                         u32 *secidx, u32 *slot)
{
    const char *p = path;
    fat_dir_t ctx;
    char comp[FS_MAX_NAME + 1];

    while (*p == '/')
        p++;
    if (!*p) {
        if (dctx) {
            dctx->c = 0;
            dctx->is_root = 1;
        }
        return -2;
    }
    ctx.c = 0;
    ctx.is_root = 1;
    for (;;) {
        int n = 0;
        fat_ent_t ent;
        u32 s2, sl;
        int r;
        while (*p == '/')
            p++;
        while (*p && *p != '/') {
            if (n < FS_MAX_NAME)
                comp[n++] = *p;
            p++;
        }
        comp[n] = 0;
        if (!n)
            break;
        r = lookup_in(comp, ctx.c, ctx.is_root, &ent, &s2, &sl);
        if (r <= 0) {
            if (dctx) {
                dctx->c = ctx.c;
                dctx->is_root = ctx.is_root;
            }
            return -1;
        }
        if (!*p) {
            if (e)
                *e = ent;
            if (dctx) {
                dctx->c = ctx.c;
                dctx->is_root = ctx.is_root;
            }
            if (secidx)
                *secidx = s2;
            if (slot)
                *slot = sl;
            return 0;
        }
        if (!(ent.attr & ATTR_DIR))
            return -1;
        ctx.c = ent.cls_lo | ((u32)ent.cls_hi << 16);
        ctx.is_root = 0;
    }
    return -1;
}

static int dirent_at(fat_dir_t *d, u32 secidx, u32 slot, fat_ent_t *e)
{
    u32 n;
    u8 buf[512];
    if (dir_expand(d->c, d->is_root, &n))
        return -1;
    if (secidx >= n)
        return -1;
    if (rd(dir_sectors[secidx], buf, 1))
        return -1;
    memcpy(e, buf + slot * 32, 32);
    return 0;
}

static int dirent_set(fat_dir_t *d, u32 secidx, u32 slot, const fat_ent_t *e)
{
    u32 n;
    u8 buf[512];
    if (dir_expand(d->c, d->is_root, &n))
        return -1;
    if (secidx >= n)
        return -1;
    if (rd(dir_sectors[secidx], buf, 1))
        return -1;
    memcpy(buf + slot * 32, e, 32);
    if (wr(dir_sectors[secidx], buf, 1))
        return -1;
    return 0;
}

static int find_slot(fat_dir_t *d, u32 *secidx, u32 *slot)
{
    u32 n;
    if (dir_expand(d->c, d->is_root, &n))
        return -1;
    for (u32 s = 0; s < n; s++) {
        u8 buf[512];
        if (rd(dir_sectors[s], buf, 1))
            return -1;
        for (int i = 0; i < 16; i++) {
            u8 b0 = buf[i * 32];
            if (b0 == 0 || b0 == 0xE5) {
                *secidx = s;
                *slot = (u32)i;
                return 0;
            }
        }
    }
    return -1;
}

static int alloc_chain(u32 n, u32 *first)
{
    u32 prev = 0;
    *first = 0;
    for (u32 c = 2; c < cur->clusters + 2 && n; c++) {
        u32 v;
        if (fat_get(c, &v))
            return -1;
        if (v != 0)
            continue;
        if (!*first)
            *first = c;
        if (prev)
            fat_set(prev, c);
        prev = c;
        n--;
    }
    if (!*first)
        return -1;
    if (prev)
        fat_set(prev, eoc_val());
    return 0;
}

static void free_chain(u32 c)
{
    while (c) {
        u32 nxt;
        int r = next_cluster(c, &nxt);
        if (fat_set(c, 0) != 0)
            break;
        if (r == 1) {
            c = nxt;
        } else {
            break;
        }
    }
}

static void fat_now(u16 *date, u16 *time)
{
    rtc_time_t t;
    rtc_read(&t);
    if (t.year < 1980)
        t.year = 1980;
    if (t.year > 2107)
        t.year = 2107;
    if (t.month < 1 || t.month > 12)
        t.month = 1;
    if (t.day < 1 || t.day > 31)
        t.day = 1;
    *date = (u16)(((t.year - 1980) << 9) | (t.month << 5) | t.day);
    *time = (u16)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
}

static int read_chain(u32 c, u32 size, u8 *dst, u32 max)
{
    u32 got = 0;
    while (c && got < max) {
        u32 lba, per, take, secs, done = 0;
        if (cl_lba(c, &lba))
            return -1;
        per = (u32)cur->spc * 512;
        take = size - got;
        if (take > per)
            take = per;
        if (take > max - got)
            take = max - got;
        secs = (take + 511) / 512;
        while (secs) {
            u32 n = secs > 128 ? 128 : secs;
            if (rd(lba + done, dst + got + done * 512, n))
                return -1;
            done += n;
            secs -= n;
        }
        got += take;
        if (take < per || got >= size)
            break;
        if (next_cluster(c, &c) != 1)
            break;
    }
    return (int)got;
}

int fat_resolve(fat_state_t *st, const char *path, fs_dirent_t *out)
{
    fat_ent_t e;
    cur = st;
    if (resolve_entry(path, &e, NULL, NULL, NULL))
        return -1;
    if (out)
        ent_to_dirent(&e, out);
    return 0;
}

int fat_list(fat_state_t *st, const char *path, int index, fs_dirent_t *out)
{
    fat_dir_t d;
    u32 n;
    int seen = 0;
    cur = st;
    if (!path || !path[0] || strcmp(path, "/") == 0) {
        d.c = 0;
        d.is_root = 1;
    } else {
        fat_ent_t e;
        if (resolve_entry(path, &e, &d, NULL, NULL))
            return -1;
        if (!(e.attr & ATTR_DIR))
            return -1;
        d.c = e.cls_lo | ((u32)e.cls_hi << 16);
        d.is_root = 0;
    }
    if (dir_expand(d.c, d.is_root, &n))
        return -1;
    for (u32 s = 0; s < n; s++) {
        u8 buf[512];
        if (rd(dir_sectors[s], buf, 1))
            return -1;
        for (int i = 0; i < 16; i++) {
            fat_ent_t *en = (fat_ent_t *)(buf + i * 32);
            if (en->name[0] == 0)
                return -1;
            if ((u8)en->name[0] == 0xE5)
                continue;
            if (en->attr == ATTR_LFN)
                continue;
            if (en->attr & ATTR_VOL)
                continue;
            if (en->name[0] == '.' && (en->name[1] == ' ' || en->name[1] == '.'))
                continue;
            if (seen == index) {
                if (out)
                    ent_to_dirent(en, out);
                return 0;
            }
            seen++;
        }
    }
    return -1;
}

int fat_read(fat_state_t *st, const char *path, void *buf, u32 max)
{
    fat_ent_t e;
    u32 cl, size;
    cur = st;
    if (resolve_entry(path, &e, NULL, NULL, NULL))
        return -1;
    if (e.attr & ATTR_DIR)
        return -1;
    cl = e.cls_lo | ((u32)e.cls_hi << 16);
    size = e.size;
    if (max > size)
        max = size;
    if (!size)
        return 0;
    return read_chain(cl, size, buf, max);
}

int fat_write(fat_state_t *st, const char *path, const void *buf, u32 size)
{
    fat_dir_t dctx;
    fat_ent_t e;
    u32 secidx, slot;
    char name[FS_MAX_NAME + 1];
    u32 ncl, first = 0, off = 0, left;
    int r;
    cur = st;
    strncpy(name, fs_basename(path), FS_MAX_NAME);
    name[FS_MAX_NAME] = 0;
    r = resolve_entry(path, &e, &dctx, &secidx, &slot);
    if (r == 0) {
        if (e.attr & ATTR_DIR)
            return -1;
        free_chain(e.cls_lo | ((u32)e.cls_hi << 16));
    } else if (r == -1) {
        if (find_slot(&dctx, &secidx, &slot))
            return -2;
    } else {
        return -1;
    }
    ncl = (size + (u32)cur->spc * 512 - 1) / ((u32)cur->spc * 512);
    if (size == 0)
        ncl = 0;
    if (ncl) {
        if (alloc_chain(ncl, &first))
            return -3;
        left = size;
        while (left && first) {
            u32 lba, take, secs, done = 0;
            u32 c = first;
            if (cl_lba(first, &lba)) {
                free_chain(first);
                return -1;
            }
            take = left;
            if (take > (u32)cur->spc * 512)
                take = (u32)cur->spc * 512;
            secs = (take + 511) / 512;
            while (secs) {
                u32 n = secs > 128 ? 128 : secs;
                if (wr(lba + done, (const u8 *)buf + off + done * 512, n)) {
                    free_chain(first);
                    return -1;
                }
                done += n;
                secs -= n;
            }
            off += take;
            left -= take;
            if (left) {
                if (next_cluster(first, &c) != 1) {
                    free_chain(first);
                    return -1;
                }
                first = c;
            }
        }
    }
    memset(&e, 0, sizeof(e));
    if (name_to_fat(name, e.name))
        return -1;
    e.attr = 0x20;
    fat_now(&e.crt_date, &e.crt_time);
    e.wrt_date = e.crt_date;
    e.wrt_time = e.crt_time;
    e.cls_lo = (u16)(first & 0xFFFF);
    e.cls_hi = (u16)((first >> 16) & 0xFFFF);
    e.size = size;
    if (dirent_set(&dctx, secidx, slot, &e))
        return -1;
    return 0;
}

int fat_mkdir(fat_state_t *st, const char *path)
{
    fat_dir_t dctx;
    fat_ent_t e;
    u32 secidx, slot, first, lba;
    char name[FS_MAX_NAME + 1];
    int r;
    cur = st;
    strncpy(name, fs_basename(path), FS_MAX_NAME);
    name[FS_MAX_NAME] = 0;
    r = resolve_entry(path, &e, &dctx, &secidx, &slot);
    if (r == 0)
        return -1;
    if (r == -1) {
        if (find_slot(&dctx, &secidx, &slot))
            return -2;
    } else {
        return -1;
    }
    if (alloc_chain(1, &first))
        return -3;
    if (cl_lba(first, &lba))
        return -1;
    {
        u8 z[512];
        u8 sbuf[512];
        fat_ent_t dot, ddot;
        u32 pc;
        memset(z, 0, sizeof(z));
        for (u32 i = 0; i < (u32)cur->spc; i++)
            if (wr(lba + i, z, 1))
                return -1;
        memset(&dot, 0, sizeof(dot));
        memcpy(dot.name, ".          ", 11);
        dot.attr = ATTR_DIR;
        fat_now(&dot.crt_date, &dot.crt_time);
        dot.wrt_date = dot.crt_date;
        dot.wrt_time = dot.crt_time;
        dot.cls_lo = (u16)(first & 0xFFFF);
        dot.cls_hi = (u16)((first >> 16) & 0xFFFF);
        ddot = dot;
        memcpy(ddot.name, "..         ", 11);
        if (dctx.is_root && cur->type != FAT_TYPE_32)
            pc = 0;
        else if (dctx.is_root)
            pc = cur->root_cluster;
        else
            pc = dctx.c;
        ddot.cls_lo = (u16)(pc & 0xFFFF);
        ddot.cls_hi = (u16)((pc >> 16) & 0xFFFF);
        if (rd(lba, sbuf, 1))
            return -1;
        memcpy(sbuf, &dot, 32);
        memcpy(sbuf + 32, &ddot, 32);
        if (wr(lba, sbuf, 1))
            return -1;
    }
    memset(&e, 0, sizeof(e));
    if (name_to_fat(name, e.name))
        return -1;
    e.attr = ATTR_DIR;
    fat_now(&e.crt_date, &e.crt_time);
    e.wrt_date = e.crt_date;
    e.wrt_time = e.crt_time;
    e.cls_lo = (u16)(first & 0xFFFF);
    e.cls_hi = (u16)((first >> 16) & 0xFFFF);
    if (dirent_set(&dctx, secidx, slot, &e))
        return -1;
    return 0;
}

int fat_delete(fat_state_t *st, const char *path)
{
    fat_dir_t dctx;
    fat_ent_t e;
    u32 secidx, slot, cl;
    u8 buf[512];
    cur = st;
    if (resolve_entry(path, &e, &dctx, &secidx, &slot))
        return -1;
    if (e.attr & ATTR_DIR) {
        fat_dir_t sub;
        u32 n;
        sub.c = e.cls_lo | ((u32)e.cls_hi << 16);
        sub.is_root = 0;
        if (dir_expand(sub.c, 0, &n))
            return -1;
        for (u32 s = 0; s < n; s++) {
            if (rd(dir_sectors[s], buf, 1))
                return -1;
            for (int i = 0; i < 16; i++) {
                fat_ent_t *en = (fat_ent_t *)(buf + i * 32);
                if (en->name[0] == 0)
                    break;
                if ((u8)en->name[0] == 0xE5)
                    continue;
                if (en->attr == ATTR_LFN || (en->attr & ATTR_VOL))
                    continue;
                if (en->name[0] == '.' && (en->name[1] == ' ' || en->name[1] == '.'))
                    continue;
                return -1;
            }
        }
    }
    cl = e.cls_lo | ((u32)e.cls_hi << 16);
    free_chain(cl);
    {
        u32 n;
        if (dir_expand(dctx.c, dctx.is_root, &n))
            return -1;
        if (secidx >= n)
            return -1;
        if (rd(dir_sectors[secidx], buf, 1))
            return -1;
        buf[slot * 32] = 0xE5;
        if (wr(dir_sectors[secidx], buf, 1))
            return -1;
    }
    return 0;
}

int fat_rename(fat_state_t *st, const char *from, const char *to)
{
    fat_dir_t dctx;
    fat_ent_t e;
    u32 secidx, slot;
    char nb[FS_MAX_NAME + 1];
    cur = st;
    if (resolve_entry(from, &e, &dctx, &secidx, &slot))
        return -1;
    strncpy(nb, fs_basename(to), FS_MAX_NAME);
    nb[FS_MAX_NAME] = 0;
    if (strcasecmp(nb, fs_basename(from)) == 0)
        return 0;
    if (name_to_fat(nb, e.name))
        return -1;
    return dirent_set(&dctx, secidx, slot, &e);
}

void fat_stat(fat_state_t *st, fs_stat_t *out)
{
    u32 freec = 0;
    fat_dir_t d;
    u32 n;
    cur = st;
    memset(out, 0, sizeof(*out));
    for (u32 c = 2; c < cur->clusters + 2; c++) {
        u32 v;
        if (fat_get(c, &v))
            break;
        if (v == 0)
            freec++;
    }
    out->total_kb = (u32)((u64)cur->clusters * (u32)cur->spc * 512 / 1024);
    out->free_kb = (u32)((u64)freec * (u32)cur->spc * 512 / 1024);
    d.c = 0;
    d.is_root = 1;
    if (dir_expand(d.c, d.is_root, &n) == 0) {
        for (u32 s = 0; s < n; s++) {
            u8 buf[512];
            if (rd(dir_sectors[s], buf, 1))
                break;
            for (int i = 0; i < 16; i++) {
                fat_ent_t *en = (fat_ent_t *)(buf + i * 32);
                if (en->name[0] == 0) {
                    s = n;
                    break;
                }
                if ((u8)en->name[0] == 0xE5)
                    continue;
                if (en->attr == ATTR_LFN || (en->attr & ATTR_VOL))
                    continue;
                if (en->name[0] == '.' && (en->name[1] == ' ' || en->name[1] == '.'))
                    continue;
                if (en->attr & ATTR_DIR)
                    out->dirs++;
                else
                    out->files++;
            }
        }
    }
}

static int bpb_type(const u8 *b)
{
    u16 bps, root_ent, fatsz16, rsvd;
    u8 spc, nfat;
    u32 total, fatsz32, fatsz, rootsecs, datas, clus;
    if (b[0] != 0xEB && b[0] != 0xE9)
        return FAT_TYPE_NONE;
    bps = b[0x0B] | (b[0x0C] << 8);
    if (bps != 512)
        return FAT_TYPE_NONE;
    if (memcmp(b + 0x36, "FAT12", 5) == 0)
        return FAT_TYPE_12;
    if (memcmp(b + 0x36, "FAT16", 5) == 0)
        return FAT_TYPE_16;
    if (memcmp(b + 0x52, "FAT32", 5) == 0)
        return FAT_TYPE_32;
    root_ent = b[0x11] | (b[0x12] << 8);
    fatsz16 = b[0x16] | (b[0x17] << 8);
    {
        u16 t16 = b[0x13] | (b[0x14] << 8);
        total = t16 ? t16 : (b[0x20] | (b[0x21] << 8) | ((u32)b[0x22] << 16) |
                             ((u32)b[0x23] << 24));
    }
    fatsz32 = b[0x24] | (b[0x25] << 8) | ((u32)b[0x26] << 16) | ((u32)b[0x27] << 24);
    spc = b[0x0D];
    rsvd = b[0x0E] | (b[0x0F] << 8);
    nfat = b[0x10];
    fatsz = fatsz16 ? fatsz16 : fatsz32;
    if (!total || !fatsz || !spc || !nfat)
        return FAT_TYPE_NONE;
    rootsecs = (root_ent * 32 + 511) / 512;
    if (total < rsvd + nfat * fatsz + rootsecs)
        return FAT_TYPE_NONE;
    datas = total - rsvd - nfat * fatsz - rootsecs;
    clus = datas / spc;
    if (clus < 4085)
        return FAT_TYPE_12;
    if (clus < 65525)
        return FAT_TYPE_16;
    return FAT_TYPE_32;
}

int fat_probe(int dev, u32 base)
{
    u8 b[512];
    int t;
    if (blk_read(dev, base, 1, b))
        return FAT_TYPE_NONE;
    t = bpb_type(b);
    return t;
}

int fat_bind(fat_state_t *st, int dev, u32 base)
{
    u8 b[512];
    int t;
    u16 t16;
    u32 total;
    if (blk_read(dev, base, 1, b))
        return -1;
    t = bpb_type(b);
    if (!t) {
        kprintf("[fat] bind: bpb_type NONE (oem=%.8s b16=%02x b17=%02x t32=%02x%02x%02x%02x)\n",
                b + 3, b[0x16], b[0x17], b[0x20], b[0x21], b[0x22], b[0x23]);
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->dev = dev;
    st->base = base;
    st->type = t;
    st->bps = 512;
    st->spc = b[0x0D];
    st->rsvd = b[0x0E] | (b[0x0F] << 8);
    st->nfat = b[0x10];
    st->root_ent = b[0x11] | (b[0x12] << 8);
    if (!st->spc || (st->spc & (st->spc - 1)) || st->nfat < 1 || st->nfat > 4)
        return -1;
    if (t == FAT_TYPE_32) {
        st->fat_sz = b[0x24] | (b[0x25] << 8) | ((u32)b[0x26] << 16) | ((u32)b[0x27] << 24);
        st->root_cluster = b[0x2C] | (b[0x2D] << 8) | ((u32)b[0x2E] << 16) | ((u32)b[0x2F] << 24);
        st->root_sectors = 0;
        st->first_data = base + st->rsvd + (u32)st->nfat * st->fat_sz;
        memcpy(st->label, b + 0x47, 11);
    } else {
        st->fat_sz = b[0x16] | (b[0x17] << 8);
        st->root_sectors = ((u32)st->root_ent * 32 + 511) / 512;
        st->root_cluster = 0;
        st->first_data = base + st->rsvd + (u32)st->nfat * st->fat_sz + st->root_sectors;
        memcpy(st->label, b + 0x2B, 11);
    }
    if (!st->fat_sz)
        return -1;
    t16 = b[0x13] | (b[0x14] << 8);
    if (t16)
        total = t16;
    else
        total = b[0x20] | (b[0x21] << 8) | ((u32)b[0x22] << 16) | ((u32)b[0x23] << 24);
    if (!total || total > 0x7FFFFFFF)
        return -1;
    st->clusters = (total - (st->first_data - base)) / (u32)st->spc;
    if (st->clusters == 0 || st->clusters > 0x0FFFFF00)
        return -1;
    st->label[11] = 0;
    for (int i = 10; i >= 0 && st->label[i] == ' '; i--)
        st->label[i] = 0;
    st->mounted = 1;
    return 0;
}

static void label_to_bpb(const char *label, char out[11])
{
    int i;
    memset(out, ' ', 11);
    if (!label)
        return;
    for (i = 0; i < 11 && label[i]; i++)
        out[i] = (char)u_toupper(label[i]);
}

static int choose_type(u32 sectors, int want)
{
    u32 clusters;
    if (want)
        return want;
    clusters = sectors - 64;
    if (clusters < 4085)
        return FAT_TYPE_12;
    if (clusters < 65525)
        return FAT_TYPE_16;
    return FAT_TYPE_32;
}

int fat_format(int dev, u32 base, const char *label, u32 sectors, int want_type)
{
    int type;
    u8 b[512];
    u16 rsvd;
    u16 root_ent;
    u32 fatsz = 1, clusters = 0, rootsecs = 0;
    char lbl[11];
    u32 i, f, s;
    u8 fbuf[512];
    u8 rbuf[512];
    u32 volid = 0x20260101;

    if (sectors < 64)
        return -1;
    type = choose_type(sectors, want_type);
    if (type != FAT_TYPE_12 && type != FAT_TYPE_16 && type != FAT_TYPE_32)
        return -1;
    rsvd = (type == FAT_TYPE_32) ? 32 : 1;
    root_ent = (type == FAT_TYPE_32) ? 0 : ((type == FAT_TYPE_12) ? 224 : 512);
    label_to_bpb(label, lbl);

    for (int it = 0; it < 4; it++) {
        u32 data_start, need;
        rootsecs = (type == FAT_TYPE_32) ? 0 : ((u32)root_ent * 32 + 511) / 512;
        data_start = rsvd + 2 * fatsz + rootsecs;
        if (data_start >= sectors) {
            fatsz = 1;
            continue;
        }
        clusters = sectors - data_start;
        if (type == FAT_TYPE_12)
            need = (clusters * 3 / 2 + 511) / 512;
        else if (type == FAT_TYPE_16)
            need = (clusters * 2 + 511) / 512;
        else
            need = (clusters * 4 + 511) / 512;
        if (need == fatsz)
            break;
        fatsz = need ? need : 1;
    }
    if (!clusters)
        return -1;
    if (type == FAT_TYPE_12 && clusters > 4084)
        return -1;
    if (type == FAT_TYPE_16 && clusters > 65524)
        return -1;

    memset(b, 0, sizeof(b));
    b[0] = 0xEB;
    b[1] = 0x3C;
    b[2] = 0x90;
    memcpy(b + 3, "aptos   ", 8);
    b[0x0B] = 0x00;
    b[0x0C] = 0x02;
    b[0x0D] = 1;
    b[0x0E] = (u8)rsvd;
    b[0x0F] = (u8)(rsvd >> 8);
    b[0x10] = 2;
    b[0x11] = (u8)(root_ent & 0xFF);
    b[0x12] = (u8)(root_ent >> 8);
    b[0x15] = 0xF8;
    if (sectors <= 0xFFFF) {
        b[0x13] = (u8)(sectors & 0xFF);
        b[0x14] = (u8)(sectors >> 8);
        b[0x20] = 0;
        b[0x21] = 0;
        b[0x22] = 0;
        b[0x23] = 0;
    } else {
        b[0x13] = 0;
        b[0x14] = 0;
        b[0x20] = (u8)(sectors & 0xFF);
        b[0x21] = (u8)((sectors >> 8) & 0xFF);
        b[0x22] = (u8)((sectors >> 16) & 0xFF);
        b[0x23] = (u8)((sectors >> 24) & 0xFF);
    }
    b[0x18] = 1;
    b[0x1A] = 0x02;
    b[0x1B] = 0x00;
    if (type == FAT_TYPE_32) {
        b[0x24] = (u8)(fatsz & 0xFF);
        b[0x25] = (u8)((fatsz >> 8) & 0xFF);
        b[0x26] = (u8)((fatsz >> 16) & 0xFF);
        b[0x27] = (u8)((fatsz >> 24) & 0xFF);
        b[0x2C] = 0x02;
        b[0x2D] = 0x00;
        b[0x2E] = 0x00;
        b[0x2F] = 0x00;
        b[0x36] = 0x29;
        b[0x37] = (u8)(volid & 0xFF);
        b[0x38] = (u8)((volid >> 8) & 0xFF);
        b[0x39] = (u8)((volid >> 16) & 0xFF);
        b[0x3A] = (u8)((volid >> 24) & 0xFF);
        memcpy(b + 0x3B, lbl, 11);
        memcpy(b + 0x46, "FAT32   ", 8);
        memcpy(b + 0x52, "FAT32   ", 8);
    } else {
        b[0x16] = (u8)(fatsz & 0xFF);
        b[0x17] = (u8)((fatsz >> 8) & 0xFF);
        b[0x26] = 0x29;
        b[0x27] = (u8)(volid & 0xFF);
        b[0x28] = (u8)((volid >> 8) & 0xFF);
        b[0x29] = (u8)((volid >> 16) & 0xFF);
        b[0x2A] = (u8)((volid >> 24) & 0xFF);
        memcpy(b + 0x2B, lbl, 11);
        memcpy(b + 0x36, (type == FAT_TYPE_12) ? "FAT12   " : "FAT16   ", 8);
    }
    if (blk_write(dev, base, 1, b))
        return -1;

    memset(fbuf, 0, sizeof(fbuf));
    if (type == FAT_TYPE_12) {
        fbuf[0] = 0xF8;
        fbuf[1] = 0xFF;
        fbuf[2] = 0xFF;
    } else if (type == FAT_TYPE_16) {
        fbuf[0] = 0xF8;
        fbuf[1] = 0xFF;
        fbuf[2] = 0xFF;
        fbuf[3] = 0xFF;
    } else {
        fbuf[0] = 0xF8;
        fbuf[1] = 0xFF;
        fbuf[2] = 0xFF;
        fbuf[3] = 0x0F;
        fbuf[4] = 0xFF;
        fbuf[5] = 0xFF;
        fbuf[6] = 0xFF;
        fbuf[7] = 0x0F;
        fbuf[8] = 0xFF;
        fbuf[9] = 0xFF;
        fbuf[10] = 0xFF;
        fbuf[11] = 0x0F;
    }
    {
        u8 z[512];
        memset(z, 0, sizeof(z));
        for (f = 0; f < 2; f++) {
            if (blk_write(dev, base + rsvd + f * fatsz, 1, fbuf))
                return -1;
            for (s = 1; s < fatsz; s++)
                if (blk_write(dev, base + rsvd + f * fatsz + s, 1, z))
                    return -1;
        }
    }

    memset(rbuf, 0, sizeof(rbuf));
    if (type != FAT_TYPE_32) {
        fat_ent_t vol;
        memset(&vol, 0, sizeof(vol));
        memcpy(vol.name, lbl, 11);
        vol.attr = ATTR_VOL;
        memcpy(rbuf, &vol, 32);
        for (s = 0; s < rootsecs; s++)
            if (blk_write(dev, base + rsvd + 2 * fatsz + s, 1, rbuf))
                return -1;
    } else {
        fat_ent_t dot, ddot;
        u32 root_lba = base + rsvd + 2 * fatsz;
        memset(&dot, 0, sizeof(dot));
        memcpy(dot.name, ".          ", 11);
        dot.attr = ATTR_DIR;
        dot.wrt_date = (u16)(((2026 - 1980) << 9) | (1 << 5) | 1);
        dot.cls_lo = 2;
        ddot = dot;
        memcpy(ddot.name, "..         ", 11);
        ddot.cls_lo = 0;
        ddot.cls_hi = 0;
        memcpy(rbuf, &dot, 32);
        memcpy(rbuf + 32, &ddot, 32);
        for (s = 0; s < 1; s++)
            if (blk_write(dev, root_lba + s, 1, rbuf))
                return -1;
    }
    return 0;
}
