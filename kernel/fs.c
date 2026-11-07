#include "fs.h"
#include "blkdev.h"
#include "bootinfo.h"
#include "fat.h"
#include "klog.h"
#include "iso9660.h"
#include "minifs.h"
#include "util.h"

#define FS_MAX_MOUNTS 6

typedef struct {
    int         used;
    char        point[32];
    int         dev;
    int         type;
    char        label[32];
    int         writable;
    mfs_state_t mfs;
    iso_state_t iso;
    fat_state_t fat;
} fs_mount_t;

static fs_mount_t mounts[FS_MAX_MOUNTS];
static int boot_dev = -1;

static fs_mount_t *root_mount(void)
{
    for (int i = 0; i < FS_MAX_MOUNTS; i++)
        if (mounts[i].used && strcmp(mounts[i].point, "/") == 0)
            return &mounts[i];
    return NULL;
}

static fs_mount_t *route(const char *path, const char **rel)
{
    fs_mount_t *best = NULL;
    int bestlen = -1;

    for (int i = 0; i < FS_MAX_MOUNTS; i++) {
        fs_mount_t *m = &mounts[i];
        int l;
        char c;

        if (!m->used)
            continue;
        l = (int)strlen(m->point);
        if (strncmp(path, m->point, (size_t)l) != 0)
            continue;
        c = path[l];
        if (!(l == 1 || c == 0 || c == '/'))
            continue;
        if (l > bestlen) {
            best = m;
            bestlen = l;
        }
    }
    if (!best)
        return NULL;
    if (bestlen == 1)
        *rel = path;
    else
        *rel = path + bestlen;
    if (**rel == 0)
        *rel = "/";
    return best;
}

static fs_mount_t *free_slot(void)
{
    for (int i = 0; i < FS_MAX_MOUNTS; i++)
        if (!mounts[i].used)
            return &mounts[i];
    return NULL;
}

static fs_mount_t *find_mount(const char *point)
{
    for (int i = 0; i < FS_MAX_MOUNTS; i++)
        if (mounts[i].used && strcmp(mounts[i].point, point) == 0)
            return &mounts[i];
    return NULL;
}

const char *fs_type_name(int type)
{
    switch (type) {
    case FS_TYPE_MINIFS:  return "minifs";
    case FS_TYPE_ISO9660: return "iso9660";
    case FS_TYPE_FAT12:   return "fat12";
    case FS_TYPE_FAT16:   return "fat16";
    case FS_TYPE_FAT32:   return "fat32";
    default:              return "none";
    }
}

static int detect_and_bind(fs_mount_t *m, int dev, u32 base)
{
    const blk_info_t *bi = blk_info(dev);
    if (!bi)
        return -1;

    m->dev = dev;

    if (bi->sector_size == 512 && mfs_bind(&m->mfs, dev, base) == 0) {
        m->type = FS_TYPE_MINIFS;
        m->writable = 1;
        strncpy(m->label, m->mfs.super.label, sizeof(m->label) - 1);
        return 0;
    }

    if (bi->sector_size == 512) {
        int ft = fat_probe(dev, base);
        if (ft == FAT_TYPE_12 || ft == FAT_TYPE_16 || ft == FAT_TYPE_32) {
            if (fat_bind(&m->fat, dev, base) == 0) {
                if (ft == FAT_TYPE_12)
                    m->type = FS_TYPE_FAT12;
                else if (ft == FAT_TYPE_16)
                    m->type = FS_TYPE_FAT16;
                else
                    m->type = FS_TYPE_FAT32;
                m->writable = 1;
                strncpy(m->label, m->fat.label, sizeof(m->label) - 1);
                return 0;
            }
        }
    }

    if (iso_bind(&m->iso, dev) == 0) {
        m->type = FS_TYPE_ISO9660;
        m->writable = 0;
        strncpy(m->label, m->iso.label[0] ? m->iso.label : "iso9660",
                sizeof(m->label) - 1);
        return 0;
    }

    return -1;
}

static int ensure_dir(const char *path)
{
    char parent[256];
    char tmp[256];
    char full[256];
    const char *name;

    if (strcmp(path, "/") == 0)
        return 0;
    if (fs_exists(path))
        return 0;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    fs_parent_path(tmp, parent, sizeof(parent));
    name = fs_basename(tmp);
    if (!name[0])
        return -1;

    snprintf(full, sizeof(full), "%s%s%s", parent,
             strcmp(parent, "/") == 0 ? "" : "/", name);
    return fs_mkdir(full);
}

static int device_is_mounted(int dev)
{
    for (int i = 0; i < FS_MAX_MOUNTS; i++)
        if (mounts[i].used && mounts[i].dev == dev)
            return 1;
    return 0;
}

int fs_mount(const char *devname, const char *point)
{
    int dev = blk_find(devname);
    fs_mount_t *m;
    u32 base = 0;

    if (dev < 0)
        return -2;
    if (!point || point[0] != '/')
        return -3;
    if (find_mount(point))
        return -4;
    if (device_is_mounted(dev))
        return -8;
    if (strcmp(point, "/") != 0 && !fs_mounted())
        return -1;

    m = free_slot();
    if (!m)
        return -5;
    memset(m, 0, sizeof(*m));

    if (dev == boot_dev && blk_info(dev)->type == BLK_HDD)
        base = BOOTINFO->fs_lba;

    if (detect_and_bind(m, dev, base) != 0)
        return -6;

    strncpy(m->point, point, sizeof(m->point) - 1);
    m->used = 1;

    if (strcmp(point, "/") != 0)
        (void)ensure_dir(point);

    kprintf("[vfs] mounted %s (%s) on %s%s\n", blk_info(dev)->name,
            fs_type_name(m->type), point, m->writable ? "" : " [read-only]");
    return 0;
}

int fs_umount(const char *point)
{
    fs_mount_t *m = find_mount(point);
    if (!m)
        return -1;
    if (strcmp(point, "/") == 0)
        return -2;
    kprintf("[vfs] unmounted %s\n", point);
    m->used = 0;
    return 0;
}

int fs_format(const char *devname, const char *label)
{
    int dev = blk_find(devname);
    const blk_info_t *bi;
    u32 base = 0;

    if (dev < 0)
        return -2;
    bi = blk_info(dev);
    if (!bi->writable || bi->sector_size != 512)
        return -3;
    if (device_is_mounted(dev))
        return -4;
    if (dev == boot_dev)
        base = BOOTINFO->fs_lba;

    kprintf("[vfs] formatting %s at sector %d (%d blocks)\n", bi->name, base,
            bi->sectors);
    if (mfs_format(dev, base, label ? label : "aptos", bi->sectors) != 0)
        return -5;
    return 0;
}

int fs_format_fat(const char *devname, const char *label, int want_type)
{
    int dev = blk_find(devname);
    const blk_info_t *bi;
    u32 base = 0;
    u32 sectors;

    if (dev < 0)
        return -2;
    bi = blk_info(dev);
    if (!bi->writable || bi->sector_size != 512)
        return -3;
    if (device_is_mounted(dev))
        return -4;
    if (dev == boot_dev)
        base = BOOTINFO->fs_lba;
    sectors = bi->sectors - base;
    if (sectors < 64)
        return -5;

    kprintf("[vfs] formatting %s as FAT at sector %d (%d sectors)\n", bi->name,
            base, sectors);
    if (fat_format(dev, base, label ? label : "aptos", sectors, want_type) != 0)
        return -5;
    return 0;
}

int fs_mounts(fs_mountinfo_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < FS_MAX_MOUNTS && n < max; i++) {
        fs_mount_t *m = &mounts[i];
        const blk_info_t *bi;
        if (!m->used)
            continue;
        bi = blk_info(m->dev);
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].point, m->point, sizeof(out[n].point) - 1);
        strncpy(out[n].dev, bi->name, sizeof(out[n].dev) - 1);
        out[n].type = m->type;
        strncpy(out[n].label, m->label, sizeof(out[n].label) - 1);
        out[n].writable = m->writable;
        if (m->type == FS_TYPE_MINIFS) {
            out[n].total_kb = m->mfs.super.total_blocks / 2;
            out[n].free_kb = m->mfs.super.free_blocks / 2;
        } else if (m->type == FS_TYPE_ISO9660) {
            out[n].total_kb = m->iso.root_size ? bi->sectors * 2 : 0;
            out[n].free_kb = 0;
        } else if (m->type == FS_TYPE_FAT12 || m->type == FS_TYPE_FAT16 ||
                   m->type == FS_TYPE_FAT32) {
            fs_stat_t st;
            fat_stat(&m->fat, &st);
            out[n].total_kb = st.total_kb;
            out[n].free_kb = st.free_kb;
        } else {
            out[n].total_kb = bi->sectors * bi->sector_size / 1024;
            out[n].free_kb = 0;
        }
        n++;
    }
    return n;
}

int fs_init(void)
{
    memset(mounts, 0, sizeof(mounts));
    boot_dev = -1;

    if (blk_count() == 0) {
        kprintf("[vfs] no block devices\n");
        return -1;
    }

    /* Pass A: 根盘 —— 只有第一个 HDD（hd0）在可识别时挂载到 /，
     * 先试 boot 布局（fs_lba 偏移），失败再从 0 试。
     * 这样 CD 引导时 / 保留给 CD-ROM，硬盘引导时系统盘挂 /。 */
    for (int i = 0; i < blk_count(); i++) {
        const blk_info_t *bi = blk_info(i);
        fs_mount_t *m;
        u32 base;

        if (i != 0 || bi->type != BLK_HDD)
            continue;
        m = free_slot();
        if (!m)
            break;
        memset(m, 0, sizeof(*m));
        if (detect_and_bind(m, i, BOOTINFO->fs_lba) != 0) {
            memset(m, 0, sizeof(*m));
            if (detect_and_bind(m, i, 0) != 0) {
                memset(m, 0, sizeof(*m));
                break;
            }
        }
        strcpy(m->point, "/");
        m->used = 1;
        boot_dev = i;
        kprintf("[vfs] %s mounted on / (%s, label '%s')\n", bi->name,
                fs_type_name(m->type), m->label);
        break;
    }

    /* Pass B: 即插即用 —— 其余 HDD 上可识别的文件系统自动挂到 /HD<n> 盘符 */
    for (int i = 0; i < blk_count(); i++) {
        const blk_info_t *bi = blk_info(i);
        fs_mount_t *m;
        u32 base;

        if (bi->type != BLK_HDD)
            continue;
        if (i == 0 && boot_dev == 0)
            continue; /* 已是根盘 */
        m = free_slot();
        if (!m)
            break;
        memset(m, 0, sizeof(*m));
        base = (i == 0) ? BOOTINFO->fs_lba : 0;
        if (detect_and_bind(m, i, base) != 0) {
            memset(m, 0, sizeof(*m));
            continue;
        }
        {
            char pt[16];
            snprintf(pt, sizeof(pt), "/HD%d", i);            strcpy(m->point, pt);
            m->used = 1;
            kprintf("[vfs] %s mounted on %s (%s, label '%s')\n", bi->name,
                    pt, fs_type_name(m->type), m->label);
        }
    }
    return 0;
}

#define FS_INSTALL_MAX_BLOCKS (256u * 1024u * 2u)

int fs_install(int dev, u32 base, const char *label)
{
    const blk_info_t *bi = blk_info(dev);
    fs_mount_t *m;
    u32 blocks;
    int capped = 0;

    if (!bi || bi->sector_size != 512)
        return -1;
    if (base >= bi->sectors)
        return -2;
    blocks = bi->sectors - base;
    if (blocks > FS_INSTALL_MAX_BLOCKS) {
        blocks = FS_INSTALL_MAX_BLOCKS;
        capped = 1;
    }
    if (blocks < 512)
        return -3;

    if (mfs_format(dev, base, label, blocks) != 0)
        return -3;

    m = free_slot();
    if (!m)
        return -4;
    memset(m, 0, sizeof(*m));
    if (mfs_bind(&m->mfs, dev, base) != 0) {
        memset(m, 0, sizeof(*m));
        return -5;
    }
    m->dev = dev;
    m->type = FS_TYPE_MINIFS;
    m->writable = 1;
    strncpy(m->label, m->mfs.super.label, sizeof(m->label) - 1);
    strcpy(m->point, "/TARGET");
    m->used = 1;
    boot_dev = dev;
    kprintf("[vfs] %s formatted at LBA %d, mounted on /TARGET (%d blocks = %d MiB%s)\n",
            bi->name, base, blocks, blocks / 2048,
            capped ? ", capped" : "");
    return 0;
}

int fs_install_root(const char *devname)
{
    int dev = blk_find(devname);
    if (dev < 0)
        return -1;
    return fs_install(dev, 0, "aptos");
}

int fs_install_fat(int dev, u32 base, const char *label, int want_type)
{
    const blk_info_t *bi = blk_info(dev);
    fs_mount_t *m;
    u32 blocks;

    if (!bi || bi->sector_size != 512)
        return -1;
    if (base >= bi->sectors)
        return -2;
    blocks = bi->sectors - base;
    if (blocks < 512)
        return -3;

    if (fat_format(dev, base, label ? label : "aptos", blocks, want_type) != 0)
        return -3;

    m = free_slot();
    if (!m)
        return -4;
    memset(m, 0, sizeof(*m));
    if (fat_bind(&m->fat, dev, base) != 0) {
        memset(m, 0, sizeof(*m));
        return -5;
    }
    m->dev = dev;
    if (m->fat.type == FAT_TYPE_12)
        m->type = FS_TYPE_FAT12;
    else if (m->fat.type == FAT_TYPE_16)
        m->type = FS_TYPE_FAT16;
    else
        m->type = FS_TYPE_FAT32;
    m->writable = 1;
    strncpy(m->label, m->fat.label, sizeof(m->label) - 1);
    strcpy(m->point, "/TARGET");
    m->used = 1;
    boot_dev = dev;
    kprintf("[vfs] %s formatted as FAT at LBA %d, mounted on /TARGET (%d sectors)\n",
            bi->name, base, blocks);
    return 0;
}

int fs_mounted(void)
{
    return root_mount() != NULL;
}

int fs_resolve(const char *path, fs_dirent_t *out)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    if (m->type == FS_TYPE_ISO9660)
        return iso_resolve(&m->iso, rel, out);
    if (m->type == FS_TYPE_FAT12 || m->type == FS_TYPE_FAT16 ||
        m->type == FS_TYPE_FAT32)
        return fat_resolve(&m->fat, rel, out);
    if (m->type != FS_TYPE_MINIFS)
        return -1;
    mfs_use(&m->mfs);
    return mfs_resolve(rel, out);
}

int fs_exists(const char *path)
{
    fs_dirent_t e;
    return fs_resolve(path, &e) == 0;
}

int fs_is_dir(const char *path)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);
    fs_dirent_t e;

    if (!m)
        return 0;
    if (strcmp(rel, "/") == 0)
        return 1;
    if (fs_resolve(path, &e) != 0)
        return 0;
    return (e.flags & FS_FLAG_DIR) != 0;
}

int fs_list(const char *path, int index, fs_dirent_t *out)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    if (m->type == FS_TYPE_ISO9660)
        return iso_list(&m->iso, rel, index, out);
    if (m->type == FS_TYPE_FAT12 || m->type == FS_TYPE_FAT16 ||
        m->type == FS_TYPE_FAT32)
        return fat_list(&m->fat, rel, index, out);
    if (m->type != FS_TYPE_MINIFS)
        return -1;
    mfs_use(&m->mfs);
    return mfs_list(rel, index, out);
}

int fs_count(const char *path)
{
    int n = 0;
    while (fs_list(path, n, NULL) == 0)
        n++;
    return n;
}

/* 一次性遍历目录；回调返回非 0 时提前结束 */
int fs_dir_scan(const char *path, int (*cb)(const fs_dirent_t *, void *), void *ctx)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);
    int i;

    if (!m)
        return -1;
    if (m->type != FS_TYPE_MINIFS) {
        /* 其他文件系统条目少，退回逐项列举 */
        for (i = 0; ; i++) {
            fs_dirent_t e;
            if (fs_list(path, i, &e) != 0)
                return 0;
            if (cb(&e, ctx) != 0)
                return 0;
        }
    }
    mfs_use(&m->mfs);
    return mfs_dir_scan(rel, cb, ctx);
}

int fs_read(const char *path, void *buf, u32 max)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    if (m->type == FS_TYPE_ISO9660)
        return iso_read(&m->iso, rel, buf, max);
    if (m->type == FS_TYPE_FAT12 || m->type == FS_TYPE_FAT16 ||
        m->type == FS_TYPE_FAT32)
        return fat_read(&m->fat, rel, buf, max);
    if (m->type != FS_TYPE_MINIFS)
        return -1;
    mfs_use(&m->mfs);
    return mfs_read(rel, buf, max);
}

int fs_write(const char *path, const void *buf, u32 size)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    if (!m->writable)
        return -2;
    if (m->type == FS_TYPE_FAT12 || m->type == FS_TYPE_FAT16 ||
        m->type == FS_TYPE_FAT32)
        return fat_write(&m->fat, rel, buf, size);
    if (m->type != FS_TYPE_MINIFS)
        return -2;
    mfs_use(&m->mfs);
    return mfs_write(rel, buf, size);
}

int fs_mkdir(const char *path)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    if (!m->writable)
        return -2;
    if (m->type == FS_TYPE_FAT12 || m->type == FS_TYPE_FAT16 ||
        m->type == FS_TYPE_FAT32)
        return fat_mkdir(&m->fat, rel);
    if (m->type != FS_TYPE_MINIFS)
        return -2;
    mfs_use(&m->mfs);
    return mfs_mkdir(rel);
}

int fs_delete(const char *path)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    if (!m->writable)
        return -2;
    if (m->type == FS_TYPE_FAT12 || m->type == FS_TYPE_FAT16 ||
        m->type == FS_TYPE_FAT32)
        return fat_delete(&m->fat, rel);
    if (m->type != FS_TYPE_MINIFS)
        return -2;
    mfs_use(&m->mfs);
    return mfs_delete(rel);
}

int fs_rename(const char *from, const char *to)
{
    const char *rel_a, *rel_b;
    fs_mount_t *ma = route(from, &rel_a);
    fs_mount_t *mb = route(to, &rel_b);

    if (!ma || !mb || ma != mb)
        return -1;
    if (!ma->writable)
        return -2;
    if (ma->type == FS_TYPE_FAT12 || ma->type == FS_TYPE_FAT16 ||
        ma->type == FS_TYPE_FAT32)
        return fat_rename(&ma->fat, rel_a, rel_b);
    if (ma->type != FS_TYPE_MINIFS)
        return -2;
    mfs_use(&ma->mfs);
    return mfs_rename(rel_a, rel_b);
}

void fs_stat(fs_stat_t *out)
{
    fs_mount_t *m = root_mount();

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!m)
        return;
    if (m->type == FS_TYPE_FAT12 || m->type == FS_TYPE_FAT16 ||
        m->type == FS_TYPE_FAT32) {
        fat_stat(&m->fat, out);
        return;
    }
    if (m->type != FS_TYPE_MINIFS)
        return;
    mfs_use(&m->mfs);
    mfs_stat(out);
}

int fs_count_kind(const char *path, u32 *files, u32 *dirs)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);
    fs_stat_t st;

    if (!m)
        return -1;
    if (m->type == FS_TYPE_FAT12 || m->type == FS_TYPE_FAT16 ||
        m->type == FS_TYPE_FAT32) {
        fat_stat(&m->fat, &st);
        if (files)
            *files = st.files;
        if (dirs)
            *dirs = st.dirs;
        return 0;
    }
    if (m->type != FS_TYPE_MINIFS)
        return -1;
    mfs_use(&m->mfs);
    return mfs_count_kind(rel, files, dirs);
}

u32 fs_total_kb(void)
{
    fs_mount_t *m = root_mount();
    fs_stat_t st;
    if (!m)
        return 0;
    if (m->type == FS_TYPE_FAT12 || m->type == FS_TYPE_FAT16 ||
        m->type == FS_TYPE_FAT32) {
        fat_stat(&m->fat, &st);
        return st.total_kb;
    }
    return (m->type == FS_TYPE_MINIFS) ? m->mfs.super.total_blocks / 2 : 0;
}

u32 fs_free_kb(void)
{
    fs_mount_t *m = root_mount();
    fs_stat_t st;
    if (!m)
        return 0;
    if (m->type == FS_TYPE_FAT12 || m->type == FS_TYPE_FAT16 ||
        m->type == FS_TYPE_FAT32) {
        fat_stat(&m->fat, &st);
        return st.free_kb;
    }
    return (m->type == FS_TYPE_MINIFS) ? m->mfs.super.free_blocks / 2 : 0;
}

const char *fs_volume_label(void)
{
    fs_mount_t *m = root_mount();
    return m ? m->label : "";
}

void fs_path(char *out, u32 outsz, const char *cwd, const char *arg)
{
    char work[256];
    int wl = 0;
    const char *p = arg;

    if (!arg || !arg[0])
        arg = ".";
    if (arg[0] == '/') {
        work[0] = 0;
        wl = 0;
    } else {
        strncpy(work, (cwd && cwd[0]) ? cwd : "/", sizeof(work) - 1);
        work[sizeof(work) - 1] = 0;
        wl = (int)strlen(work);
        if (wl == 1 && work[0] == '/')
            wl = 0;
    }

    while (*p) {
        char comp[FS_MAX_NAME + 1];
        int n = 0;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        while (*p && *p != '/' && n < FS_MAX_NAME)
            comp[n++] = *p++;
        comp[n] = 0;
        while (*p && *p != '/')
            p++;

        if (strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0) {
            while (wl > 0 && work[wl - 1] != '/')
                wl--;
            if (wl > 0)
                wl--;
            work[wl] = 0;
            continue;
        }
        if (wl + 1 + n >= (int)sizeof(work) - 1)
            break;
        work[wl++] = '/';
        for (int i = 0; i < n; i++)
            work[wl++] = comp[i];
        work[wl] = 0;
    }

    if (wl == 0) {
        work[0] = '/';
        work[1] = 0;
    }
    strncpy(out, work, outsz - 1);
    out[outsz - 1] = 0;
}

const char *fs_basename(const char *path)
{
    const char *slash;
    if (!path)
        return "";
    slash = strrchr(path, '/');
    if (!slash)
        return path;
    if (!slash[1])
        return slash;
    return slash + 1;
}

void fs_parent_path(const char *path, char *out, u32 outsz)
{
    char tmp[256];
    char *slash;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) {
        strncpy(out, "/", outsz - 1);
        out[outsz - 1] = 0;
        return;
    }
    *slash = 0;
    strncpy(out, tmp, outsz - 1);
    out[outsz - 1] = 0;
}
