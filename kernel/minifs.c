#include "minifs.h"
#include "blkdev.h"
#include "bootinfo.h"
#include "heap.h"
#include "klog.h"
#include "rtc.h"
#include "util.h"

#define FS_MAGIC MFS_MAGIC
#define FS_ENTRY_SZ 64
#define FS_PER_BLOCK (512 / FS_ENTRY_SZ)
#define FS_MAX_DIR_ENTRIES 4096   /* 翻倍上限；必须 >= 打包时给子目录的初始容量(600) */
#define DIR_BLOCKS 32


typedef struct {
    u32 start;
    u32 entries;
} fs_dir_t;

typedef struct {
    fs_dir_t dir;
    int      is_root;
    fs_dir_t parent;
    u32      parent_idx;
} dirloc_t;

static mfs_state_t *cur;

static int bit_test(u32 i)
{
    return (cur->bitmap[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(u32 i)
{
    cur->bitmap[i >> 3] |= (u8)(1u << (i & 7));
}

static void bit_clear(u32 i)
{
    cur->bitmap[i >> 3] &= (u8)~(1u << (i & 7));
}

static void bitmap_flush(u32 first, u32 count)
{
    u32 a = (first >> 3) / 512;
    u32 b = ((first + count - 1) >> 3) / 512;
    for (u32 s = a; s <= b; s++)
        blk_write(cur->dev, cur->super.bitmap_start + s, 1, cur->bitmap + s * 512);
}

static void super_flush(void)
{
    blk_write(cur->dev, cur->base, 1, &cur->super);
}

static u32 alloc_blocks(u32 count)
{
    u32 first = cur->super.data_start - cur->base;
    u32 limit = cur->super.total_blocks;
    u32 run = 0;

    for (u32 i = first; i < limit; i++) {
        if (!bit_test(i)) {
            if (++run == count) {
                u32 start = i + 1 - count;
                for (u32 j = 0; j < count; j++)
                    bit_set(start + j);
                bitmap_flush(start, count);
                if (cur->super.free_blocks >= count)
                    cur->super.free_blocks -= count;
                return cur->base + start;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

static void free_blocks_raw(u32 first_block, u32 blocks)
{
    if (!first_block || !blocks)
        return;
    for (u32 i = 0; i < blocks; i++)
        bit_clear(first_block - cur->base + i);
    bitmap_flush(first_block - cur->base, blocks);
    cur->super.free_blocks += blocks;
}

static u32 dir_blocks(const fs_dir_t *d)
{
    return (d->entries + FS_PER_BLOCK - 1) / FS_PER_BLOCK;
}

/* 目录项是 64 字节，一个扇区装 8 条。原先每读一个条目就单独读一次
 * 512 字节扇区，扫一遍 4096 项要发 4096 次块读。这里加一个单扇区缓存，
 * 连续条目复用同一次读，把 I/O 次数压到 entries/8。磁盘格式不变。 */
static u8  dcache[512];
static u32 dcache_sec;
static int dcache_ok;

static void dcache_drop(void)
{
    dcache_ok = 0;
}

static int dcache_fill(u32 abs)
{
    if (dcache_ok && dcache_sec == abs)
        return 0;
    if (blk_read(cur->dev, abs, 1, dcache) != 0) {
        dcache_ok = 0;
        return -1;
    }
    dcache_sec = abs;
    dcache_ok = 1;
    return 0;
}

static int dir_read(const fs_dir_t *d, u32 idx, fs_dirent_t *e)
{
    u32 off;

    if (idx >= d->entries)
        return -1;
    off = idx * FS_ENTRY_SZ;
    if (dcache_fill(d->start + off / 512) != 0)
        return -1;
    memcpy(e, dcache + (off % 512), FS_ENTRY_SZ);
    return 0;
}

static int dir_write(const fs_dir_t *d, u32 idx, const fs_dirent_t *e)
{
    u32 off;

    if (idx >= d->entries)
        return -1;
    off = idx * FS_ENTRY_SZ;
    if (dcache_fill(d->start + off / 512) != 0)
        return -1;
    memcpy(dcache + (off % 512), e, FS_ENTRY_SZ);
    return blk_write(cur->dev, dcache_sec, 1, dcache);
}

static int dir_find(const fs_dir_t *d, const char *name, fs_dirent_t *out, u32 *idx)
{
    for (u32 i = 0; i < d->entries; i++) {
        fs_dirent_t cur;
        if (dir_read(d, i, &cur) != 0)
            return -1;
        if (cur.name[0] == 0)
            continue;
        if (strcasecmp(cur.name, name) == 0) {
            if (out)
                *out = cur;
            if (idx)
                *idx = i;
            return 0;
        }
    }
    return -1;
}

static int dir_free_slot(const fs_dir_t *d, u32 *idx)
{
    for (u32 i = 0; i < d->entries; i++) {
        fs_dirent_t cur;
        if (dir_read(d, i, &cur) != 0)
            return -1;
        if (cur.name[0] == 0) {
            *idx = i;
            return 0;
        }
    }
    return -1;
}

static int dir_grow(dirloc_t *d)
{
    u32 old_blocks = dir_blocks(&d->dir);
    u32 new_entries = d->dir.entries * 2;
    u32 new_blocks;
    u32 nb;
    u8 buf[512];

    if (new_entries > FS_MAX_DIR_ENTRIES)
        new_entries = FS_MAX_DIR_ENTRIES;      /* 翻倍会越界时取上限，而不是整条放弃 */
    if (new_entries <= d->dir.entries)
        return -1;
    new_blocks = (new_entries + FS_PER_BLOCK - 1) / FS_PER_BLOCK;
    nb = alloc_blocks(new_blocks);
    if (!nb)
        return -1;

    for (u32 i = 0; i < old_blocks; i++) {
        if (blk_read(cur->dev, d->dir.start + i, 1, buf) != 0)
            goto fail;
        if (blk_write(cur->dev, nb + i, 1, buf) != 0)
            goto fail;
    }
    memset(buf, 0, 512);
    for (u32 i = old_blocks; i < new_blocks; i++)
        if (blk_write(cur->dev, nb + i, 1, buf) != 0)
            goto fail;

    free_blocks_raw(d->dir.start, old_blocks);
    dcache_drop();                         /* 旧目录块已释放，缓存的扇区不能再用 */
    d->dir.start = nb;
    d->dir.entries = new_entries;

    if (!d->is_root) {
        fs_dirent_t e;
        if (dir_read(&d->parent, d->parent_idx, &e) != 0)
            return -1;
        e.first_block = nb;
        e.size = new_entries;
        if (dir_write(&d->parent, d->parent_idx, &e) != 0)
            return -1;
    }
    return 0;

fail:
    free_blocks_raw(nb, new_blocks);
    return -1;
}

static int dir_add(dirloc_t *d, const fs_dirent_t *e)
{
    u32 idx;

    if (dir_free_slot(&d->dir, &idx) != 0) {
        if (d->is_root)
            return -1;
        if (dir_grow(d) != 0)
            return -1;
        if (dir_free_slot(&d->dir, &idx) != 0)
            return -1;
    }
    if (dir_write(&d->dir, idx, e) != 0)
        return -1;
    super_flush();
    return 0;
}

static void loc_root(dirloc_t *l)
{
    l->dir.start = cur->super.dir_start;
    l->dir.entries = cur->super.max_entries;
    l->is_root = 1;
    l->parent_idx = 0;
    memset(&l->parent, 0, sizeof(l->parent));
}

static void loc_child(const dirloc_t *from, u32 idx, const fs_dirent_t *e, dirloc_t *out)
{
    out->dir.start = e->first_block;
    out->dir.entries = e->size;
    out->is_root = 0;
    out->parent = from->dir;
    out->parent_idx = idx;
}

static int walk_dir(const char *path, dirloc_t *out)
{
    dirloc_t cur;
    const char *p = path;

    loc_root(&cur);
    while (*p == '/')
        p++;

    while (*p) {
        char name[FS_MAX_NAME + 1];
        int n = 0;
        fs_dirent_t e;
        u32 idx;
        dirloc_t next;

        while (*p && *p != '/' && n < FS_MAX_NAME)
            name[n++] = *p++;
        name[n] = 0;
        while (*p && *p != '/')
            p++;
        while (*p == '/')
            p++;
        if (n == 0)
            continue;
        if (dir_find(&cur.dir, name, &e, &idx) != 0)
            return -1;
        if (!(e.flags & FS_FLAG_DIR))
            return -1;
        loc_child(&cur, idx, &e, &next);
        cur = next;
    }
    *out = cur;
    return 0;
}

static int split_leaf(const char *path, dirloc_t *parent, char *leaf)
{
    char tmp[256];
    char *slash;
    int len;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    len = (int)strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = 0;

    slash = strrchr(tmp, '/');
    if (!slash)
        return -1;
    strncpy(leaf, slash + 1, FS_MAX_NAME);
    leaf[FS_MAX_NAME] = 0;
    if (!leaf[0])
        return -1;

    if (slash == tmp) {
        loc_root(parent);
    } else {
        *slash = 0;
        if (walk_dir(tmp, parent) != 0)
            return -1;
    }
    return 0;
}

static void upcase(const char *src, char *dst)
{
    int i = 0;
    for (; src[i] && i < FS_MAX_NAME; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        dst[i] = c;
    }
    dst[i] = 0;
}

static void count_dir(const fs_dir_t *d, u32 *files, u32 *dirs, int depth)
{
    if (depth > 8)
        return;
    for (u32 i = 0; i < d->entries; i++) {
        fs_dirent_t e;
        if (dir_read(d, i, &e) != 0)
            return;
        if (e.name[0] == 0)
            continue;
        if (e.flags & FS_FLAG_DIR) {
            fs_dir_t sub;
            (*dirs)++;
            sub.start = e.first_block;
            sub.entries = e.size;
            count_dir(&sub, files, dirs, depth + 1);
        } else {
            (*files)++;
        }
    }
}

void mfs_use(mfs_state_t *st)
{
    if (cur != st)
        dcache_drop();
    cur = st;
}

int mfs_bind(mfs_state_t *st, int dev, u32 base)
{
    mfs_state_t *save = cur;

    cur = st;
    st->dev = dev;
    st->base = base;
    st->mounted = 0;
    st->bitmap = NULL;

    if (blk_read(dev, base, 1, &st->super) != 0) {
        kprintf("[minifs] bind: cannot read the superblock at LBA %d\n", base);
        goto fail;
    }
    if (memcmp(st->super.magic, FS_MAGIC, 8) != 0) {
        kprintf("[minifs] bind: bad magic '%c%c%c%c' at LBA %d\n",
                st->super.magic[0], st->super.magic[1], st->super.magic[2],
                st->super.magic[3], base);
        goto fail;
    }
    if (st->super.block_size != 512) {
        kprintf("[minifs] bind: block size %d is not 512\n", st->super.block_size);
        goto fail;
    }
    if (st->super.total_blocks == 0 || st->super.bitmap_blocks == 0) {
        kprintf("[minifs] bind: total %d, bitmap %d blocks\n", st->super.total_blocks,
                st->super.bitmap_blocks);
        goto fail;
    }

    st->bitmap = (u8 *)kmalloc((size_t)st->super.bitmap_blocks * 512);
    if (!st->bitmap) {
        kprintf("[minifs] bind: out of memory for a %d KiB bitmap\n",
                st->super.bitmap_blocks / 2);
        goto fail;
    }
    if (blk_read(dev, st->super.bitmap_start, st->super.bitmap_blocks,
                 st->bitmap) != 0) {
        kprintf("[minifs] bind: cannot read %d bitmap blocks at LBA %d\n",
                st->super.bitmap_blocks, st->super.bitmap_start);
        goto fail;
    }

    st->mounted = 1;
    cur = save;
    return 0;

fail:
    st->mounted = 0;
    cur = save;
    return -1;
}

int mfs_format(int dev, u32 base, const char *label, u32 total_blocks)
{
    mfs_super_t sb;
    u8 zero[512];
    u32 bitmap_blocks, dir_start, data_start, data_blocks;
    u32 i;

    if (total_blocks < 512)
        return -1;

    bitmap_blocks = ((total_blocks + 7) / 8 + 511) / 512;
    if (bitmap_blocks == 0)
        bitmap_blocks = 1;
    dir_start = 1 + bitmap_blocks;
    data_start = dir_start + DIR_BLOCKS;
    if (data_start + 8 >= total_blocks)
        return -1;
    data_blocks = total_blocks - data_start;

    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, FS_MAGIC, 8);
    sb.block_size = 512;
    sb.total_blocks = total_blocks;
    sb.bitmap_start = base + 1;
    sb.bitmap_blocks = bitmap_blocks;
    sb.dir_start = base + dir_start;
    sb.dir_blocks = DIR_BLOCKS;
    sb.data_start = base + data_start;
    sb.data_blocks = data_blocks;
    sb.max_entries = DIR_BLOCKS * FS_PER_BLOCK;
    sb.free_blocks = data_blocks;
    strncpy(sb.label, label ? label : "aptos", sizeof(sb.label) - 1);

    if (blk_write(dev, base, 1, &sb) != 0)
        return -1;

    memset(zero, 0, 512);
    for (i = 0; i < bitmap_blocks; i++)
        if (blk_write(dev, base + 1 + i, 1, zero) != 0)
            return -1;
    for (i = 0; i < DIR_BLOCKS; i++)
        if (blk_write(dev, base + dir_start + i, 1, zero) != 0)
            return -1;
    return 0;
}

int mfs_mounted(void)
{
    return cur->mounted;
}

int mfs_resolve(const char *path, fs_dirent_t *out)
{
    dirloc_t parent;
    char leaf[FS_MAX_NAME + 1];

    if (!cur->mounted)
        return -1;
    if (!path || !path[0])
        return -1;
    if (strcmp(path, "/") == 0) {
        if (out) {
            memset(out, 0, sizeof(*out));
            out->name[0] = '/';
            out->name[1] = 0;
            out->flags = FS_FLAG_DIR;
            out->first_block = cur->super.dir_start;
            out->size = cur->super.max_entries;
        }
        return 0;
    }
    if (split_leaf(path, &parent, leaf) != 0)
        return -1;
    return dir_find(&parent.dir, leaf, out, NULL);
}

int mfs_exists(const char *path)
{
    return mfs_resolve(path, NULL) == 0;
}

int mfs_is_dir(const char *path)
{
    fs_dirent_t e;
    if (mfs_resolve(path, &e) != 0)
        return 0;
    return (e.flags & FS_FLAG_DIR) != 0;
}

int mfs_list(const char *path, int index, fs_dirent_t *out)
{
    dirloc_t d;
    int seen = 0;

    if (!cur->mounted)
        return -1;
    if (walk_dir(path, &d) != 0)
        return -1;

    for (u32 i = 0; i < d.dir.entries; i++) {
        fs_dirent_t e;
        if (dir_read(&d.dir, i, &e) != 0)
            return -1;
        if (e.name[0] == 0)
            continue;
        if (seen == index) {
            if (out)
                *out = e;
            return 0;
        }
        seen++;
    }
    return -1;
}

/* 一次性遍历目录：按扇区批量读取，避免 fs_list 那种"每取一项都从目录头重扫"的 O(n^2)。
   回调返回非 0 表示提前结束（例如桌面图标已经摆满）。 */
int mfs_dir_scan(const char *path, int (*cb)(const fs_dirent_t *, void *), void *ctx)
{
    dirloc_t d;
    u8 chunk[4 * 512];
    u32 per, nsec, s;

    if (!cur || !cur->mounted)
        return -1;
    if (walk_dir(path, &d) != 0)
        return -1;

    per = 512 / FS_ENTRY_SZ;
    nsec = (d.dir.entries + per - 1) / per;

    for (s = 0; s < nsec; ) {
        u32 cnt = nsec - s;
        u32 k;

        if (cnt > 4)
            cnt = 4;
        if (blk_read(cur->dev, d.dir.start + s, cnt, chunk) != 0)
            return -1;

        for (k = 0; k < cnt * per; k++) {
            u32 idx = s * per + k;
            fs_dirent_t e;

            if (idx >= d.dir.entries)
                break;
            memcpy(&e, chunk + k * FS_ENTRY_SZ, FS_ENTRY_SZ);
            if (e.name[0] == 0)
                continue;
            if (cb(&e, ctx) != 0)
                return 0;
        }
        s += cnt;
    }
    return 0;
}

int mfs_count(const char *path)
{
    int n = 0;
    while (mfs_list(path, n, NULL) == 0)
        n++;
    return n;
}

int mfs_read(const char *path, void *buf, u32 max)
{
    fs_dirent_t e;
    u32 n, blocks, done = 0;
    u8 *p = (u8 *)buf;

    if (mfs_resolve(path, &e) != 0)
        return -1;
    if (e.flags & FS_FLAG_DIR)
        return -1;

    n = e.size;
    if (n > max)
        n = max;
    if (n == 0)
        return 0;

    blocks = (n + 511u) / 512u;
    {
        /* 完整块直接读入用户缓冲；最后一块可能不满 512B，
         * 先读入临时缓冲再拷贝，避免 blk_read 越界写用户缓冲区 */
        u32 full = blocks - 1;
        while (done < full) {
            u32 chunk = full - done;
            if (chunk > 255)
                chunk = 255;
            if (blk_read(cur->dev, e.first_block + done, (u8)chunk, p) != 0)
                return -1;
            p += chunk * 512;
            done += chunk;
        }
        if (done < blocks) {
            u8 tmp[512];
            if (blk_read(cur->dev, e.first_block + done, 1, tmp) != 0)
                return -1;
            memcpy(p, tmp, n - done * 512u);
        }
    }
    return (int)n;
}

int mfs_write(const char *path, const void *buf, u32 size)
{
    dirloc_t parent;
    char leaf[FS_MAX_NAME + 1];
    char name[FS_MAX_NAME + 1];
    fs_dirent_t e;
    fs_dirent_t old;
    u32 blocks, new_first = 0, done = 0, old_blocks = 0;
    const u8 *p = (const u8 *)buf;
    int existed;

    if (!cur->mounted)
        return -1;
    if (split_leaf(path, &parent, leaf) != 0)
        return -1;
    upcase(leaf, name);
    if (!name[0])
        return -1;

    existed = (dir_find(&parent.dir, name, &old, NULL) == 0);
    if (existed && (old.flags & FS_FLAG_DIR))
        return -1;

    blocks = (size + 511u) / 512u;
    if (blocks) {
        new_first = alloc_blocks(blocks);
        if (!new_first) {
            kprintf("[fs] no space for %d bytes\n", size);
            return -1;
        }
        while (done < blocks) {
            u32 chunk = blocks - done;
            if (chunk > 255)
                chunk = 255;
            if (blk_write(cur->dev, new_first + done, (u8)chunk, p) != 0) {
                free_blocks_raw(new_first, blocks);
                super_flush();
                return -1;
            }
            p += chunk * 512;
            done += chunk;
        }
    }

    if (existed) {
        old_blocks = (old.size + 511u) / 512u;
        if (old_blocks)
            free_blocks_raw(old.first_block, old_blocks);
    }

    memset(&e, 0, sizeof(e));
    strncpy(e.name, name, FS_MAX_NAME);
    e.size = size;
    e.first_block = new_first;
    e.flags = 0;
    e.mdate = rtc_date_stamp();
    e.mtime = rtc_time_stamp();

    if (existed) {
        u32 idx;
        if (dir_find(&parent.dir, name, NULL, &idx) != 0 ||
            dir_write(&parent.dir, idx, &e) != 0) {
            free_blocks_raw(new_first, blocks);
            super_flush();
            return -1;
        }
        super_flush();
    } else {
        if (dir_add(&parent, &e) != 0) {
            free_blocks_raw(new_first, blocks);
            super_flush();
            return -1;
        }
    }
    return (int)size;
}

int mfs_mkdir(const char *path)
{
    dirloc_t parent;
    char leaf[FS_MAX_NAME + 1];
    char name[FS_MAX_NAME + 1];
    fs_dirent_t e;
    u32 blk;
    u8 zero[512];

    if (!cur->mounted)
        return -1;
    if (split_leaf(path, &parent, leaf) != 0)
        return -1;
    upcase(leaf, name);
    if (dir_find(&parent.dir, name, NULL, NULL) == 0)
        return -1;

    blk = alloc_blocks(1);
    if (!blk)
        return -1;
    memset(zero, 0, 512);
    if (blk_write(cur->dev, blk, 1, zero) != 0) {
        free_blocks_raw(blk, 1);
        super_flush();
        return -1;
    }

    memset(&e, 0, sizeof(e));
    strncpy(e.name, name, FS_MAX_NAME);
    e.size = FS_PER_BLOCK;
    e.first_block = blk;
    e.flags = FS_FLAG_DIR;
    e.mdate = rtc_date_stamp();
    e.mtime = rtc_time_stamp();

    if (dir_add(&parent, &e) != 0) {
        free_blocks_raw(blk, 1);
        super_flush();
        return -1;
    }
    return 0;
}

int mfs_delete(const char *path)
{
    dirloc_t parent;
    char leaf[FS_MAX_NAME + 1];
    char name[FS_MAX_NAME + 1];
    fs_dirent_t e;
    u32 idx;

    if (!cur->mounted)
        return -1;
    if (strcmp(path, "/") == 0)
        return -1;
    if (split_leaf(path, &parent, leaf) != 0)
        return -1;
    upcase(leaf, name);
    if (dir_find(&parent.dir, name, &e, &idx) != 0)
        return -1;

    if (e.flags & FS_FLAG_DIR) {
        u32 files = 0, dirs = 0;
        fs_dir_t sub;
        sub.start = e.first_block;
        sub.entries = e.size;
        count_dir(&sub, &files, &dirs, 0);
        if (files || dirs)
            return -2;
        free_blocks_raw(e.first_block, dir_blocks(&sub));
    } else {
        u32 blocks = (e.size + 511u) / 512u;
        if (blocks)
            free_blocks_raw(e.first_block, blocks);
    }

    memset(&e, 0, sizeof(e));
    if (dir_write(&parent.dir, idx, &e) != 0)
        return -1;
    super_flush();
    return 0;
}

int mfs_rename(const char *from, const char *to)
{
    dirloc_t src_dir, dst_dir;
    char src_leaf[FS_MAX_NAME + 1], dst_leaf[FS_MAX_NAME + 1];
    char src_name[FS_MAX_NAME + 1], dst_name[FS_MAX_NAME + 1];
    fs_dirent_t e;
    u32 idx;

    if (!cur->mounted)
        return -1;
    if (split_leaf(from, &src_dir, src_leaf) != 0)
        return -1;
    if (split_leaf(to, &dst_dir, dst_leaf) != 0)
        return -1;
    upcase(src_leaf, src_name);
    upcase(dst_leaf, dst_name);
    if (dir_find(&src_dir.dir, src_name, &e, &idx) != 0)
        return -1;
    if (dir_find(&dst_dir.dir, dst_name, NULL, NULL) == 0)
        return -2;

    memset(e.name, 0, sizeof(e.name));
    strncpy(e.name, dst_name, FS_MAX_NAME);
    if (dir_add(&dst_dir, &e) != 0)
        return -1;

    {
        fs_dirent_t blank;
        memset(&blank, 0, sizeof(blank));
        if (dir_write(&src_dir.dir, idx, &blank) != 0)
            return -1;
    }
    super_flush();
    return 0;
}

void mfs_stat(fs_stat_t *out)
{
    u32 files = 0, dirs = 0;
    fs_dir_t root;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!cur->mounted)
        return;

    root.start = cur->super.dir_start;
    root.entries = cur->super.max_entries;
    count_dir(&root, &files, &dirs, 0);

    out->total_kb = cur->super.total_blocks / 2;
    out->free_kb = cur->super.free_blocks / 2;
    out->files = files;
    out->dirs = dirs;
}

int mfs_count_kind(const char *path, u32 *files, u32 *dirs)
{
    dirloc_t d;
    if (walk_dir(path, &d) != 0)
        return -1;
    if (files)
        *files = 0;
    if (dirs)
        *dirs = 0;
    count_dir(&d.dir, files, dirs, 0);
    return 0;
}

u32 mfs_total_kb(void)
{
    return cur->mounted ? cur->super.total_blocks / 2u : 0;
}

u32 mfs_free_kb(void)
{
    return cur->mounted ? cur->super.free_blocks / 2u : 0;
}

const char *mfs_volume_label(void)
{
    return cur->mounted ? cur->super.label : "";
}

