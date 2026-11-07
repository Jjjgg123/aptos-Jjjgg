#include "iso9660.h"
#include "blkdev.h"
#include "klog.h"
#include "util.h"

typedef struct {
    int  dev;
    u32  lba;
    u32  size;
    u32  pos;
    u8   buf[ISO_SECTOR];
    u32  have;
} iso_iter_t;

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void iso_name(const u8 *rec, char *out, u32 outsz)
{
    u32 n = rec[32];
    const u8 *p = rec + 33;
    u32 i = 0;

    if (n == 1 && (p[0] == 0 || p[0] == 1)) {
        strncpy(out, p[0] == 1 ? ".." : ".", outsz - 1);
        out[outsz - 1] = 0;
        return;
    }

    for (; i < n && i < outsz - 1; i++) {
        char c = (char)p[i];
        if (c == ';')
            break;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        out[i] = c;
    }
    while (i > 0 && out[i - 1] == ' ')
        i--;
    out[i] = 0;
    if (i == 0)
        strncpy(out, "?", outsz - 1);
}

static void iter_start(iso_iter_t *it, int dev, u32 lba, u32 size)
{
    it->dev = dev;
    it->lba = lba;
    it->size = size;
    it->pos = 0;
    it->have = 0xFFFFFFFFu;
}

static int iter_next(iso_iter_t *it, u8 **rec)
{
    while (it->pos < it->size) {
        u32 sec = it->pos / ISO_SECTOR;
        u32 off = it->pos % ISO_SECTOR;
        u8 len;

        if (sec != it->have) {
            if (blk_read(it->dev, it->lba + sec, 1, it->buf) != 0)
                return -1;
            it->have = sec;
        }
        if (off >= ISO_SECTOR)
            return -1;

        len = it->buf[off];
        if (len == 0) {
            it->pos = (sec + 1) * ISO_SECTOR;
            continue;
        }
        if (off + len > ISO_SECTOR) {
            it->pos = (sec + 1) * ISO_SECTOR;
            continue;
        }
        *rec = it->buf + off;
        it->pos += len;
        return 0;
    }
    return -1;
}

static int lookup(iso_state_t *st, u32 dir_lba, u32 dir_size, const char *name,
                  fs_dirent_t *out)
{
    iso_iter_t it;
    u8 *rec;
    char want[FS_MAX_NAME + 1];

    strncpy(want, name, FS_MAX_NAME);
    want[FS_MAX_NAME] = 0;
    for (int i = 0; want[i]; i++)
        if (want[i] >= 'a' && want[i] <= 'z')
            want[i] = (char)(want[i] - 'a' + 'A');

    iter_start(&it, st->dev, dir_lba, dir_size);
    while (iter_next(&it, &rec) == 0) {
        char nm[FS_MAX_NAME + 1];

        if (rec[0] < 34)
            continue;
        iso_name(rec, nm, sizeof(nm));
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
            continue;
        if (strcasecmp(nm, want) != 0)
            continue;
        if (out) {
            memset(out, 0, sizeof(*out));
            strncpy(out->name, nm, FS_MAX_NAME);
            out->first_block = le32(rec + 2);
            out->size = le32(rec + 10);
            out->flags = (rec[25] & 0x02) ? FS_FLAG_DIR : 0;
            out->mdate = 20250101;
            out->mtime = 1200;
        }
        return 0;
    }
    return -1;
}

int iso_bind(iso_state_t *st, int dev)
{
    u8 sec[ISO_SECTOR];

    memset(st, 0, sizeof(*st));
    st->dev = dev;

    if (blk_read(dev, 16, 1, sec) != 0)
        return -1;
    if (sec[0] != 1 || memcmp(sec + 1, "CD001", 5) != 0)
        return -1;

    st->root_lba = le32(sec + 156 + 2);
    st->root_size = le32(sec + 156 + 10);
    memcpy(st->label, sec + 40, 32);
    st->label[32] = 0;
    for (int i = 31; i >= 0 && st->label[i] == ' '; i--)
        st->label[i] = 0;
    st->mounted = 1;
    return 0;
}

static int walk_to_dir(iso_state_t *st, const char *path, u32 *lba, u32 *size)
{
    u32 cur_lba = st->root_lba;
    u32 cur_size = st->root_size;
    const char *p = path;

    while (*p == '/')
        p++;
    while (*p) {
        char comp[FS_MAX_NAME + 1];
        int n = 0;
        fs_dirent_t e;

        while (*p && *p != '/' && n < FS_MAX_NAME)
            comp[n++] = *p++;
        comp[n] = 0;
        while (*p && *p != '/')
            p++;
        while (*p == '/')
            p++;
        if (n == 0)
            continue;
        if (lookup(st, cur_lba, cur_size, comp, &e) != 0)
            return -1;
        if (!(e.flags & FS_FLAG_DIR))
            return -1;
        cur_lba = e.first_block;
        cur_size = e.size;
    }
    *lba = cur_lba;
    *size = cur_size;
    return 0;
}

int iso_resolve(iso_state_t *st, const char *path, fs_dirent_t *out)
{
    const char *base;
    char parent[256];
    u32 lba, size;

    if (!st->mounted)
        return -1;
    if (strcmp(path, "/") == 0) {
        if (out) {
            memset(out, 0, sizeof(*out));
            out->name[0] = '/';
            out->flags = FS_FLAG_DIR;
            out->first_block = st->root_lba;
            out->size = st->root_size;
        }
        return 0;
    }

    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = 0;
    base = fs_basename(parent);
    if (!base || !base[0])
        return -1;

    {
        char dirpath[256];
        fs_parent_path(parent, dirpath, sizeof(dirpath));
        if (walk_to_dir(st, dirpath, &lba, &size) != 0)
            return -1;
    }
    return lookup(st, lba, size, base, out);
}

int iso_list(iso_state_t *st, const char *path, int index, fs_dirent_t *out)
{
    u32 lba, size;
    iso_iter_t it;
    u8 *rec;
    int seen = 0;

    if (!st->mounted)
        return -1;
    if (walk_to_dir(st, path, &lba, &size) != 0)
        return -1;

    iter_start(&it, st->dev, lba, size);
    while (iter_next(&it, &rec) == 0) {
        char nm[FS_MAX_NAME + 1];

        if (rec[0] < 34)
            continue;
        iso_name(rec, nm, sizeof(nm));
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
            continue;
        if (seen == index) {
            if (out) {
                memset(out, 0, sizeof(*out));
                strncpy(out->name, nm, FS_MAX_NAME);
                out->first_block = le32(rec + 2);
                out->size = le32(rec + 10);
                out->flags = (rec[25] & 0x02) ? FS_FLAG_DIR : 0;
                out->mdate = 20250101;
                out->mtime = 1200;
            }
            return 0;
        }
        seen++;
    }
    return -1;
}

int iso_read(iso_state_t *st, const char *path, void *buf, u32 max)
{
    fs_dirent_t e;
    u32 done = 0;
    u32 n;
    u8 *p = (u8 *)buf;
    u8 tmp[ISO_SECTOR];

    if (iso_resolve(st, path, &e) != 0)
        return -1;
    if (e.flags & FS_FLAG_DIR)
        return -1;

    n = e.size;
    if (n > max)
        n = max;
    while (done < n) {
        u32 chunk = n - done;
        if (blk_read(st->dev, e.first_block + done / ISO_SECTOR, 1, tmp) != 0)
            return -1;
        if (chunk > ISO_SECTOR)
            chunk = ISO_SECTOR;
        memcpy(p + done, tmp, chunk);
        done += chunk;
    }
    return (int)n;
}
