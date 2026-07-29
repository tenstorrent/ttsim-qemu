/*
 * Tenstorrent ttsim PCI device.
 *
 * Forwards BAR MMIO to the libttsim simulator (loaded at realize time via
 * gmodule) and routes simulator-initiated DMA into the guest address space.
 *
 * libttsim is a process-wide singleton that can present several host-visible
 * chips at once (e.g. a wh_x32 Galaxy exposes 32).  Each chip is a distinct PCI
 * device selected by the config-space device number, and each occupies its own
 * host-physical BAR window.  We therefore model one QEMU PCI device per chip,
 * all sharing a single loaded library through the ttsim_lib context below: add
 * one "-device ttsim" per chip (index auto-assigns 0, 1, 2, ...).
 *
 * SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qom/object.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"

#include <gmodule.h>

#define TYPE_PCI_TTSIM_DEVICE "ttsim"
typedef struct TTSimState TTSimState;
DECLARE_INSTANCE_CHECKER(TTSimState, TTSIM, TYPE_PCI_TTSIM_DEVICE)

/*
 * BAR window sizes, matching libttsim's fixed per-chip layout.  BAR4's size is
 * chip-dependent (WH: 32 MiB, BH: 32 GiB) and comes from the bar4-size property.
 * The per-chip BAR *bases* are not hard-coded here: each chip's BARs live in a
 * distinct host-physical window, and we read the bases from config space.
 */
#define TTSIM_BAR0_SIZE (512 * MiB)
#define TTSIM_BAR2_SIZE (1 * MiB)
#define TTSIM_BAR4_SIZE_DEFAULT (32 * GiB)

/* PCI device numbers run 0..31, so a build exposes at most this many chips. */
#define TTSIM_MAX_CHIPS 32

/* index property sentinel: assign the lowest free chip number at realize. */
#define TTSIM_INDEX_AUTO 0xFFFFFFFFu

typedef struct TTSimBar {
    uint64_t base;
    MemoryRegion mr;
} TTSimBar;

struct TTSimState {
    PCIDevice pdev;
    TTSimBar bars[3];

    /* Properties. */
    char *lib_path;
    uint64_t bar4_size;
    uint32_t clock_quantum;
    uint32_t clock_period_us;
    uint32_t index;

    /* Host-visible chip (config-space device number) resolved from index. */
    uint32_t chip;

    QLIST_ENTRY(TTSimState) next;
};

/*
 * Process-wide library context shared by every ttsim PCI device.  libttsim's
 * state lives in file-scope globals and its DMA callback API takes no opaque
 * parameter, so there is exactly one loaded instance per QEMU process.  The
 * first device to realize loads the library, calls libttsim_init, discovers how
 * many chips it exposes, and starts the single global clock; the last to
 * unrealize tears it all down.  clock/lib properties are taken from that first
 * device.
 */
static struct TTSimLib {
    GModule *lib;
    unsigned refcount;
    uint32_t num_mmio_chips;
    uint64_t used_chip_mask;
    QEMUTimer clock_timer;
    uint32_t clock_quantum;
    uint32_t clock_period_us;
    char *lib_path;

    /*
     * Simulator-initiated DMA has no chip identity, so it is routed through one
     * representative device's address space.  This is correct for a flat guest
     * with no vIOMMU (every ttsim device shares system memory); it always points
     * at a live device (reassigned on unrealize).
     */
    PCIDevice *dma_dev;
    QLIST_HEAD(, TTSimState) devices;

    /* libttsim entry points. */
    void     (*sim_init)(void);
    void     (*sim_exit)(void);
    void     (*sim_set_dma_cbs)(void (*)(uint64_t, void *, uint32_t),
                                void (*)(uint64_t, const void *, uint32_t));
    uint32_t (*sim_config_rd32)(uint32_t bdf, uint32_t offset);
    void     (*sim_mem_rd)(uint64_t paddr, void *p, uint32_t size);
    void     (*sim_mem_wr)(uint64_t paddr, const void *p, uint32_t size);
    void     (*sim_clock)(uint32_t n);
} ttsim_lib;

static void ttsim_dma_read(uint64_t paddr, void *buf, uint32_t size)
{
    pci_dma_read(ttsim_lib.dma_dev, paddr, buf, size);
}

static void ttsim_dma_write(uint64_t paddr, const void *buf, uint32_t size)
{
    pci_dma_write(ttsim_lib.dma_dev, paddr, buf, size);
}

static uint64_t ttsim_bar_read(void *opaque, hwaddr addr, unsigned size)
{
    TTSimBar *bar = opaque;
    uint64_t val = 0;

    ttsim_lib.sim_mem_rd(bar->base + addr, &val, size);
    return val;
}

static void ttsim_bar_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    TTSimBar *bar = opaque;

    ttsim_lib.sim_mem_wr(bar->base + addr, &val, size);
}

static const MemoryRegionOps ttsim_bar_ops = {
    .read = ttsim_bar_read,
    .write = ttsim_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl  = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void ttsim_clock_tick(void *opaque)
{
    ttsim_lib.sim_clock(ttsim_lib.clock_quantum);
    timer_mod(&ttsim_lib.clock_timer,
              qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + ttsim_lib.clock_period_us);
}

static bool ttsim_load_lib(const char *lib_path, Error **errp)
{
    const char *path = lib_path ? lib_path : "libttsim.so";

    ttsim_lib.lib = g_module_open(path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    if (!ttsim_lib.lib) {
        error_setg(errp, "ttsim: g_module_open(%s) failed: %s",
                   path, g_module_error());
        return false;
    }

#define RESOLVE(field, name) do {                                              \
        if (!g_module_symbol(ttsim_lib.lib, (name), (gpointer *)&ttsim_lib.field)) { \
            error_setg(errp, "ttsim: g_module_symbol(%s) failed: %s",          \
                       (name), g_module_error());                             \
            g_module_close(ttsim_lib.lib);                                    \
            ttsim_lib.lib = NULL;                                             \
            return false;                                                     \
        }                                                                     \
    } while (0)

    RESOLVE(sim_init,        "libttsim_init");
    RESOLVE(sim_exit,        "libttsim_exit");
    RESOLVE(sim_set_dma_cbs, "libttsim_set_pci_dma_mem_callbacks");
    RESOLVE(sim_config_rd32, "libttsim_pci_config_rd32");
    RESOLVE(sim_mem_rd,      "libttsim_pci_mem_rd_bytes");
    RESOLVE(sim_mem_wr,      "libttsim_pci_mem_wr_bytes");
    RESOLVE(sim_clock,       "libttsim_clock");
#undef RESOLVE

    return true;
}

/*
 * Count host-visible chips by walking config-space device numbers: libttsim
 * returns all-ones once the device number passes the last present chip.
 */
static uint32_t ttsim_count_chips(void)
{
    uint32_t n = 0;

    while (n < TTSIM_MAX_CHIPS &&
           ttsim_lib.sim_config_rd32(n << 3, 0) != 0xFFFFFFFFu) {
        n++;
    }
    return n;
}

/* Read a 64-bit BAR base from config space, stripping the low flag bits. */
static uint64_t ttsim_bar_base(uint32_t bdf, uint32_t lo_offset)
{
    uint32_t lo = ttsim_lib.sim_config_rd32(bdf, lo_offset);
    uint32_t hi = ttsim_lib.sim_config_rd32(bdf, lo_offset + 4);

    return ((uint64_t)hi << 32) | (lo & ~0xFu);
}

static void ttsim_lib_teardown(void)
{
    timer_del(&ttsim_lib.clock_timer);
    ttsim_lib.sim_exit();
    g_module_close(ttsim_lib.lib);
    g_free(ttsim_lib.lib_path);
    memset(&ttsim_lib, 0, sizeof(ttsim_lib));
}

static void ttsim_init_bar(TTSimState *s, unsigned idx, uint64_t base,
                           uint64_t size, const char *name)
{
    TTSimBar *bar = &s->bars[idx];

    bar->base = base;
    memory_region_init_io(&bar->mr, OBJECT(s), &ttsim_bar_ops, bar, name, size);
}

static void pci_ttsim_realize(PCIDevice *pdev, Error **errp)
{
    TTSimState *s = TTSIM(pdev);
    uint32_t id, class_revision, bdf;
    bool loaded_here = false;

    if (!s->clock_period_us) {
        error_setg(errp, "ttsim: clock-period-us must be non-zero");
        return;
    }

    if (ttsim_lib.refcount == 0) {
        if (!ttsim_load_lib(s->lib_path, errp)) {
            return;
        }
        loaded_here = true;
        ttsim_lib.clock_quantum = s->clock_quantum;
        ttsim_lib.clock_period_us = s->clock_period_us;
        ttsim_lib.lib_path = g_strdup(s->lib_path ? s->lib_path : "libttsim.so");
        /*
         * The DMA callbacks reach back through ttsim_lib.dma_dev, so it must be
         * set before sim_set_dma_cbs() - and both must run before sim_init(), in
         * case the library initiates DMA during init.
         */
        ttsim_lib.dma_dev = pdev;
        ttsim_lib.sim_set_dma_cbs(ttsim_dma_read, ttsim_dma_write);
        ttsim_lib.sim_init();
        ttsim_lib.num_mmio_chips = ttsim_count_chips();
        timer_init_us(&ttsim_lib.clock_timer, QEMU_CLOCK_VIRTUAL,
                      ttsim_clock_tick, NULL);
        timer_mod(&ttsim_lib.clock_timer,
                  qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) +
                  ttsim_lib.clock_period_us);
    } else {
        /*
         * The library, its clock, and the lib path belong to the first device
         * to realize; the same properties on later devices are ignored.  Warn
         * so a mismatch is not silently dropped.
         */
        const char *path = s->lib_path ? s->lib_path : "libttsim.so";

        if (s->clock_quantum != ttsim_lib.clock_quantum ||
            s->clock_period_us != ttsim_lib.clock_period_us ||
            strcmp(path, ttsim_lib.lib_path) != 0) {
            warn_report("ttsim: lib/clock-quantum/clock-period-us are "
                        "process-wide and set by the first ttsim device; "
                        "ignoring this device's values (using lib=%s, "
                        "clock-quantum=%u, clock-period-us=%u)",
                        ttsim_lib.lib_path, ttsim_lib.clock_quantum,
                        ttsim_lib.clock_period_us);
        }
    }

    if (s->index == TTSIM_INDEX_AUTO) {
        uint32_t c;
        for (c = 0; c < ttsim_lib.num_mmio_chips; c++) {
            if (!(ttsim_lib.used_chip_mask & (1ull << c))) {
                break;
            }
        }
        if (c == ttsim_lib.num_mmio_chips) {
            error_setg(errp, "ttsim: no free chip slot (library exposes %u "
                       "MMIO chip(s))", ttsim_lib.num_mmio_chips);
            goto err;
        }
        s->chip = c;
    } else {
        if (s->index >= ttsim_lib.num_mmio_chips) {
            error_setg(errp, "ttsim: index=%u out of range (library exposes %u "
                       "MMIO chip(s))", s->index, ttsim_lib.num_mmio_chips);
            goto err;
        }
        if (ttsim_lib.used_chip_mask & (1ull << s->index)) {
            error_setg(errp, "ttsim: index=%u already in use", s->index);
            goto err;
        }
        s->chip = s->index;
    }

    ttsim_lib.used_chip_mask |= 1ull << s->chip;
    QLIST_INSERT_HEAD(&ttsim_lib.devices, s, next);
    ttsim_lib.refcount++;

    /*
     * Read the PCI identity from the simulator so the guest-visible config
     * header tracks whichever TT_VERSION libttsim was built against.
     */
    bdf = s->chip << 3;
    id = ttsim_lib.sim_config_rd32(bdf, 0);
    pci_config_set_vendor_id(pdev->config, extract32(id,  0, 16));
    pci_config_set_device_id(pdev->config, extract32(id, 16, 16));
    class_revision = ttsim_lib.sim_config_rd32(bdf, PCI_CLASS_REVISION);
    pci_config_set_revision(pdev->config, extract32(class_revision, 0, 8));
    pci_config_set_prog_interface(pdev->config, extract32(class_revision, 8, 8));
    pci_config_set_class(pdev->config, extract32(class_revision, 16, 16));

    ttsim_init_bar(s, 0, ttsim_bar_base(bdf, 0x10), TTSIM_BAR0_SIZE,
                   "ttsim-bar0");
    ttsim_init_bar(s, 1, ttsim_bar_base(bdf, 0x18), TTSIM_BAR2_SIZE,
                   "ttsim-bar2");
    ttsim_init_bar(s, 2, ttsim_bar_base(bdf, 0x20), s->bar4_size,
                   "ttsim-bar4");

    pci_register_bar(pdev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bars[0].mr);
    pci_register_bar(pdev, 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bars[1].mr);
    pci_register_bar(pdev, 4,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bars[2].mr);
    return;

err:
    if (loaded_here) {
        ttsim_lib_teardown();
    }
}

static void pci_ttsim_exit(PCIDevice *pdev)
{
    TTSimState *s = TTSIM(pdev);

    QLIST_REMOVE(s, next);
    ttsim_lib.used_chip_mask &= ~(1ull << s->chip);
    if (ttsim_lib.dma_dev == pdev) {
        TTSimState *live = QLIST_FIRST(&ttsim_lib.devices);
        ttsim_lib.dma_dev = live ? &live->pdev : NULL;
    }
    if (--ttsim_lib.refcount == 0) {
        ttsim_lib_teardown();
    }
}

static const Property ttsim_properties[] = {
    DEFINE_PROP_STRING("lib", TTSimState, lib_path),
    DEFINE_PROP_SIZE("bar4-size", TTSimState, bar4_size,
                     TTSIM_BAR4_SIZE_DEFAULT),
    DEFINE_PROP_UINT32("clock-quantum", TTSimState, clock_quantum, 1000),
    DEFINE_PROP_UINT32("clock-period-us", TTSimState, clock_period_us, 100),
    DEFINE_PROP_UINT32("index", TTSimState, index, TTSIM_INDEX_AUTO),
};

static void ttsim_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize = pci_ttsim_realize;
    k->exit = pci_ttsim_exit;
    /*
     * These are overwritten at realize from libttsim's config space; the
     * values here exist only to satisfy PCI core requirements during class
     * registration.
     */
    k->vendor_id = PCI_VENDOR_ID_TENSTORRENT;
    k->device_id = 0;
    k->class_id = 0x1200;

    dc->desc = "Tenstorrent ttsim simulator";
    device_class_set_props(dc, ttsim_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ttsim_types[] = {
    {
        .name          = TYPE_PCI_TTSIM_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(TTSimState),
        .class_init    = ttsim_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(ttsim_types)
