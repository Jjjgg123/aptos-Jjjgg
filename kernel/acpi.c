#include "acpi.h"
#include "io.h"
#include "klog.h"
#include "util.h"

typedef struct PACKED {
    char signature[8];
    u8   checksum;
    char oem_id[6];
    u8   revision;
    u32  rsdt_addr;
    u32  length;
    u64  xsdt_addr;
    u8   ext_checksum;
    u8   reserved[3];
} rsdp_t;

typedef struct PACKED {
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} acpi_header_t;

typedef struct PACKED {
    acpi_header_t h;
    u32 firmware_ctrl;
    u32 dsdt;
    u8  reserved0;
    u8  preferred_profile;
    u16 sci_int;
    u32 smi_cmd;
    u8  acpi_enable;
    u8  acpi_disable;
    u8  s4bios_req;
    u8  pstate_cnt;
    u32 pm1a_evt_blk;
    u32 pm1b_evt_blk;
    u32 pm1a_cnt_blk;
    u32 pm1b_cnt_blk;
} fadt_t;

typedef struct PACKED {
    acpi_header_t h;
    u32 lapic_addr;
    u32 flags;
} madt_t;

static acpi_info_t info;
static u64 rsdt_or_xsdt;
static int use_xsdt;

static int checksum_ok(const void *p, u32 len)
{
    const u8 *b = (const u8 *)p;
    u8 sum = 0;
    for (u32 i = 0; i < len; i++)
        sum = (u8)(sum + b[i]);
    return sum == 0;
}

static const char *rsdp_search_range(u32 start, u32 len)
{
    for (u32 addr = start; addr < start + len; addr += 16) {
        const char *p = (const char *)addr;
        if (memcmp(p, "RSD PTR ", 8) == 0)
            if (checksum_ok(p, 20))
                return p;
    }
    return NULL;
}

int acpi_init(void)
{
    const rsdp_t *rsdp = NULL;
    u16 ebda_seg = *(u16 *)0x40E;
    u32 ebda = (u32)ebda_seg << 4;

    memset(&info, 0, sizeof(info));
    info.pm1a_cnt = 0;

    if (ebda >= 0x400 && ebda < 0xA0000) {
        const char *p = rsdp_search_range(ebda, 1024);
        if (p && ((const rsdp_t *)p)->revision >= 2)
            rsdp = (const rsdp_t *)p;
    }
    if (!rsdp) {
        const char *p = rsdp_search_range(0xE0000, 0x20000);
        if (p)
            rsdp = (const rsdp_t *)p;
    }
    if (!rsdp && ebda >= 0x400 && ebda < 0xA0000)
        rsdp = (const rsdp_t *)rsdp_search_range(ebda, 1024);
    if (!rsdp) {
        kprintf("[acpi] no RSDP found\n");
        return -1;
    }

    info.present = 1;
    info.revision = rsdp->revision;
    memcpy(info.oem_id, rsdp->oem_id, 6);
    info.oem_id[6] = 0;
    info.rsdt_addr = rsdp->rsdt_addr;
    info.xsdt_addr = (rsdp->revision >= 2) ? rsdp->xsdt_addr : 0;

    if (info.xsdt_addr && checksum_ok((void *)(u32)info.xsdt_addr, 36)) {
        use_xsdt = 1;
        rsdt_or_xsdt = info.xsdt_addr;
    } else {
        use_xsdt = 0;
        rsdt_or_xsdt = info.rsdt_addr;
    }
    if (!rsdt_or_xsdt) {
        kprintf("[acpi] no RSDT/XSDT\n");
        info.present = 0;
        return -1;
    }

    {
        const acpi_header_t *h = (const acpi_header_t *)(u32)rsdt_or_xsdt;
        u32 n = (h->length - sizeof(acpi_header_t)) / (use_xsdt ? 8 : 4);
        memcpy(info.oem_table_id, h->oem_table_id, 8);
        info.oem_table_id[8] = 0;
        info.table_count = (int)n;
    }

    for (int i = 0; i < info.table_count; i++) {
        const acpi_header_t *t;
        u32 addr;

        if (use_xsdt)
            addr = (u32)((const u64 *)(u32)(rsdt_or_xsdt + sizeof(acpi_header_t)))[i];
        else
            addr = ((const u32 *)(u32)(rsdt_or_xsdt + sizeof(acpi_header_t)))[i];
        if (!addr)
            continue;
        t = (const acpi_header_t *)(u32)addr;
        if (!checksum_ok(t, t->length)) {
            kprintf("[acpi] bad checksum on %.4s\n", t->signature);
            continue;
        }

        if (memcmp(t->signature, "FACP", 4) == 0) {
            const fadt_t *f = (const fadt_t *)t;
            info.pm1a_cnt = (u16)f->pm1a_cnt_blk;
            info.pm1b_cnt = (u16)f->pm1b_cnt_blk;
            info.dsdt_addr = f->dsdt;
            memcpy(info.facs_sig, "FACS", 4);
            info.facs_sig[4] = 0;
        } else if (memcmp(t->signature, "APIC", 4) == 0) {
            const madt_t *m = (const madt_t *)t;
            u32 off = sizeof(madt_t);
            info.lapic_addr = m->lapic_addr;
            info.cpus = 0;
            info.ioapics = 0;
            while (off + 2 <= m->h.length) {
                const u8 *e = (const u8 *)t + off;
                u8 type = e[0];
                u8 len = e[1];
                if (len < 2 || off + len > m->h.length)
                    break;
                if (type == 0) {
                    info.cpus++;
                } else if (type == 1) {
                    info.ioapics++;
                }
                off += len;
            }
        } else if (memcmp(t->signature, "DSDT", 4) == 0) {
            memcpy(info.dsdt_sig, "DSDT", 4);
            info.dsdt_sig[4] = 0;
        }
    }

    kprintf("[acpi] rev %d, oem '%s', %d tables, %d cpu(s), lapic 0x%x, pm1a 0x%x\n",
            info.revision, info.oem_id, info.table_count, info.cpus,
            info.lapic_addr, info.pm1a_cnt);
    return 0;
}

int acpi_cpu_count(void)
{
    return info.cpus;
}

int acpi_lapic_addr(void)
{
    return (int)info.lapic_addr;
}

const acpi_info_t *acpi_info(void)
{
    return &info;
}

int acpi_find_table(const char *sig, void **addr, u32 *length)
{
    if (!info.present)
        return -1;
    for (int i = 0; i < info.table_count; i++) {
        const acpi_header_t *t;
        u32 a;
        if (use_xsdt)
            a = (u32)((const u64 *)(u32)(rsdt_or_xsdt + sizeof(acpi_header_t)))[i];
        else
            a = ((const u32 *)(u32)(rsdt_or_xsdt + sizeof(acpi_header_t)))[i];
        if (!a)
            continue;
        t = (const acpi_header_t *)(u32)a;
        if (memcmp(t->signature, sig, 4) == 0) {
            if (addr)
                *addr = (void *)a;
            if (length)
                *length = t->length;
            return 0;
        }
    }
    return -1;
}

void acpi_shutdown(void)
{
    if (info.present && info.pm1a_cnt) {
        kprintf("[acpi] entering S5 (power off)\n");
        outw((u16)info.pm1a_cnt, (u16)(0x3400));
        if (info.pm1b_cnt)
            outw((u16)info.pm1b_cnt, (u16)(0x3400));
    }
    for (int i = 0; i < 100; i++)
        io_wait();
}

void acpi_reboot(void)
{
    outb(0x64, 0xFE);
}
