/*
 * Tenstorrent ttsim PCI device.
 *
 * Forwards BAR MMIO to the libttsim simulator (loaded at realize time via
 * gmodule) and routes simulator-initiated DMA into the guest address space.
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
#include "qom/object.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"

#include <gmodule.h>

#define TYPE_PCI_TTSIM_DEVICE "ttsim"
typedef struct TTSimState TTSimState;
DECLARE_INSTANCE_CHECKER(TTSimState, TTSIM, TYPE_PCI_TTSIM_DEVICE)

/*
 * Simulator-internal physical addresses of the BAR windows.  These are
 * hard-coded by libttsim and must be supplied verbatim to its
 * libttsim_pci_mem_*_bytes() entry points; they are unrelated to where the
 * guest BIOS programs the BARs in the VM's address space.
 */
#define TTSIM_BAR0_BASE 0x100000000ULL
#define TTSIM_BAR2_BASE 0x120000000ULL
#define TTSIM_BAR4_BASE 0x800000000ULL

#define TTSIM_BAR0_SIZE (512 * MiB)
#define TTSIM_BAR2_SIZE (1 * MiB)
#define TTSIM_BAR4_SIZE_DEFAULT (32 * GiB)

typedef struct TTSimBar {
    TTSimState *state;
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

    /* Runtime. */
    GModule *lib;
    QEMUTimer clock_timer;

    /* libttsim entry points. */
    void     (*sim_init)(void);
    void     (*sim_exit)(void);
    void     (*sim_set_dma_cbs)(void (*)(uint64_t, void *, uint32_t),
                                void (*)(uint64_t, const void *, uint32_t));
    uint32_t (*sim_config_rd32)(uint32_t bdf, uint32_t offset);
    void     (*sim_mem_rd)(uint64_t paddr, void *p, uint32_t size);
    void     (*sim_mem_wr)(uint64_t paddr, const void *p, uint32_t size);
    void     (*sim_clock)(uint32_t n);
};

/*
 * libttsim is a process-wide singleton (its state lives in file-scope
 * globals), and its DMA callback API takes no opaque parameter.  We therefore
 * cap the model at one instance per QEMU process and recover the PCIDevice
 * through this pointer from inside the DMA callbacks.
 */
static TTSimState *ttsim_singleton;

static void ttsim_dma_read(uint64_t paddr, void *buf, uint32_t size)
{
    pci_dma_read(&ttsim_singleton->pdev, paddr, buf, size);
}

static void ttsim_dma_write(uint64_t paddr, const void *buf, uint32_t size)
{
    pci_dma_write(&ttsim_singleton->pdev, paddr, buf, size);
}

static uint64_t ttsim_bar_read(void *opaque, hwaddr addr, unsigned size)
{
    TTSimBar *bar = opaque;
    uint64_t val = 0;

    bar->state->sim_mem_rd(bar->base + addr, &val, size);
    return val;
}

static void ttsim_bar_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    TTSimBar *bar = opaque;

    bar->state->sim_mem_wr(bar->base + addr, &val, size);
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
    TTSimState *s = opaque;

    s->sim_clock(s->clock_quantum);
    timer_mod(&s->clock_timer,
              qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + s->clock_period_us);
}

static bool ttsim_load_lib(TTSimState *s, Error **errp)
{
    const char *path = s->lib_path ? s->lib_path : "libttsim.so";

    s->lib = g_module_open(path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    if (!s->lib) {
        error_setg(errp, "ttsim: g_module_open(%s) failed: %s",
                   path, g_module_error());
        return false;
    }

#define RESOLVE(field, name) do {                                              \
        if (!g_module_symbol(s->lib, (name), (gpointer *)&s->field)) {         \
            error_setg(errp, "ttsim: g_module_symbol(%s) failed: %s",          \
                       (name), g_module_error());                              \
            return false;                                                      \
        }                                                                      \
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

static void ttsim_init_bar(TTSimState *s, unsigned idx, uint64_t base,
                           uint64_t size, const char *name)
{
    TTSimBar *bar = &s->bars[idx];

    bar->state = s;
    bar->base = base;
    memory_region_init_io(&bar->mr, OBJECT(s), &ttsim_bar_ops, bar, name, size);
}

static void pci_ttsim_realize(PCIDevice *pdev, Error **errp)
{
    TTSimState *s = TTSIM(pdev);
    uint32_t id;
    uint32_t class_revision;

    if (ttsim_singleton) {
        error_setg(errp,
                   "ttsim: only one instance per QEMU process is supported");
        return;
    }
    if (!s->clock_period_us) {
        error_setg(errp, "ttsim: clock-period-us must be non-zero");
        return;
    }

    if (!ttsim_load_lib(s, errp)) {
        return;
    }

    /*
     * The DMA callbacks reach back through ttsim_singleton, so it must be set
     * before sim_set_dma_cbs() - and both must run before sim_init(), in case
     * the library initiates DMA during init.
     */
    ttsim_singleton = s;
    s->sim_set_dma_cbs(ttsim_dma_read, ttsim_dma_write);
    s->sim_init();

    /*
     * Read the PCI identity from the simulator so the guest-visible config
     * header tracks whichever TT_VERSION libttsim was built against.
     */
    id = s->sim_config_rd32(0, 0);
    pci_config_set_vendor_id(pdev->config, extract32(id,  0, 16));
    pci_config_set_device_id(pdev->config, extract32(id, 16, 16));
    class_revision = s->sim_config_rd32(0, PCI_CLASS_REVISION);
    pci_config_set_revision(pdev->config, extract32(class_revision, 0, 8));
    pci_config_set_prog_interface(pdev->config, extract32(class_revision, 8, 8));
    pci_config_set_class(pdev->config, extract32(class_revision, 16, 16));

    ttsim_init_bar(s, 0, TTSIM_BAR0_BASE, TTSIM_BAR0_SIZE, "ttsim-bar0");
    ttsim_init_bar(s, 1, TTSIM_BAR2_BASE, TTSIM_BAR2_SIZE, "ttsim-bar2");
    ttsim_init_bar(s, 2, TTSIM_BAR4_BASE, s->bar4_size,    "ttsim-bar4");

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

    timer_init_us(&s->clock_timer, QEMU_CLOCK_VIRTUAL, ttsim_clock_tick, s);
    timer_mod(&s->clock_timer,
              qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + s->clock_period_us);
}

static void pci_ttsim_exit(PCIDevice *pdev)
{
    TTSimState *s = TTSIM(pdev);

    timer_del(&s->clock_timer);
    s->sim_exit();
    g_module_close(s->lib);
    ttsim_singleton = NULL;
}

static const Property ttsim_properties[] = {
    DEFINE_PROP_STRING("lib", TTSimState, lib_path),
    DEFINE_PROP_SIZE("bar4-size", TTSimState, bar4_size,
                     TTSIM_BAR4_SIZE_DEFAULT),
    DEFINE_PROP_UINT32("clock-quantum", TTSimState, clock_quantum, 1000),
    DEFINE_PROP_UINT32("clock-period-us", TTSimState, clock_period_us, 100),
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
