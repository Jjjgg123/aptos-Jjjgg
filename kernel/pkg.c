#include "pkg.h"
#include "fs.h"
#include "gui.h"
#include "heap.h"
#include "klog.h"
#include "lst.h"
#include "user.h"
#include "util.h"

#define PKG_MAX_SIZE (512 * 1024)

static u8 *pkg_scratch;

static u8 *scratch(void)
{
    if (!pkg_scratch)
        pkg_scratch = (u8 *)kmalloc(PKG_MAX_SIZE);
    return pkg_scratch;
}

int pkg_info(const char *path)
{
    u8 *data = scratch();
    int len;
    const lst_header_t *h;

    if (!data)
        return -1;
    len = fs_read(path, data, PKG_MAX_SIZE);
    if (len < 0)
        return -2;
    if (!lst_validate(data, (u32)len))
        return -3;
    h = lst_header(data);
    kprintf("[pkg] %s: '%s' by %s, %d entries, installer is '%s'\n", path, h->name,
            h->vendor, h->count,
            lst_entry(data, (u32)len, h->installer) ? "present" : "missing");
    return (int)h->count;
}

int pkg_run(const char *path)
{
    u8 *data = scratch();
    int len;
    const lst_header_t *h;
    const lst_entry_t *inst;
    const u8 *payload;
    u32 psize;
    int r;

    if (!data)
        return -1;
    len = fs_read(path, data, PKG_MAX_SIZE);
    if (len < 0) {
        kprintf("[pkg] cannot read %s\n", path);
        return -2;
    }
    if (!lst_validate(data, (u32)len)) {
        kprintf("[pkg] %s is not a valid .lst package\n", path);
        return -3;
    }

    h = lst_header(data);
    kprintf("[pkg] '%s' by %s - %d entries, installer #%d\n", h->name, h->vendor,
            h->count, h->installer);

    inst = lst_entry(data, (u32)len, h->installer);
    payload = lst_payload(data, (u32)len, h->installer, &psize);
    if (!inst || !payload)
        return -4;

    kprintf("[pkg] launching installer '%s' (%d bytes) in ring 3\n", inst->name, psize);

    r = user_exec_image(payload, psize, path, inst->name);
    kprintf("[pkg] installer '%s' returned %d\n", inst->name, r);
    return r;
}
