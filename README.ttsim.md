# Tenstorrent ttsim QEMU fork

This repository is a single-patch fork of upstream QEMU. It adds one device
model, `hw/misc/ttsim.c` (the `ttsim` PCI device), on top of an unmodified
upstream `stable-11.0`. The patch exists *only* in this repository; it is not,
and cannot be, submitted upstream because it loads an external library by name
at runtime.

## Relationship to upstream

Canonical upstream QEMU lives at <https://gitlab.com/qemu-project/qemu>. The
`stable-11.0` branch here mirrors upstream exactly; all fork work lives on
`stable-11.0-ttsim` as a single commit, rebased onto upstream point releases as
needed.

## Building and using

Clone the fork branch, build QEMU as usual, and point the device at a libttsim
shared object:

```sh
git clone -b stable-11.0-ttsim https://github.com/tenstorrent/ttsim-qemu
# ... configure/make ...
qemu-system-x86_64 -device ttsim,lib=/path/to/libttsim.so ...
```

Only x86_64 (i386-softmmu) and aarch64 are wired up today; other targets can
enable `CONFIG_TTSIM` in their device config later.

### Multiple MMIO chips

A multi-chip libttsim (e.g. `wh_x32`, `bh_x32`, or the 4-MMIO `wh_x8`) presents
several host-visible chips through one process-wide library. Add one
`-device ttsim` per MMIO chip, all pointing at the same `libttsim.so`; they share
the single loaded library and each surfaces as its own PCI device (and, in the
guest, its own `/dev/tenstorrent/N`):

```sh
qemu-system-x86_64 \
    -device ttsim,lib=/path/to/libttsim.so,bar4-size=32M \
    -device ttsim,lib=/path/to/libttsim.so,bar4-size=32M \
    ...   # repeat NUM_MMIO_CHIPS times
```

Each device claims the lowest free chip number by default; pass `index=N` to pin
a specific one. Specifying more devices than the library exposes, an
out-of-range `index`, or a duplicate `index` is a startup error.

**Limitation:** simulator-initiated DMA carries no chip identity in the current
libttsim ABI, so it is routed through one representative device's address space.
This is correct for a flat guest, but with a per-device vIOMMU
(`iommu=smmuv3` / `intel-iommu`) in translated mode, DMA from non-representative
chips may translate in the wrong context; use IOMMU passthrough for multi-chip
DMA until the callback gains a chip parameter.

libttsim is a separate project: <https://github.com/tenstorrent/ttsim>

## Licensing

QEMU is licensed GPL v2; new files in this fork (including `hw/misc/ttsim.c`)
are GPL-2.0-or-later, the QEMU default. libttsim is an independent,
Apache-2.0-licensed project, loaded at runtime via `dlopen` (gmodule). QEMU and
libttsim are built and linked by the end user and are never distributed as a
combined binary.

Anyone redistributing builds derived from this fork should point back to this
repository so the complete corresponding source is available (GPL v2 sec. 3).
Disparate components packaged together rely on the GPL v2 "mere aggregation"
provision.

Tenstorrent-Ticket: OSPO-668
