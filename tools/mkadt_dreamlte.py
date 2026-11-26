#!/usr/bin/env python3
"""Build the Galaxy S8 (SM-G950F, dreamlte, Exynos 8895) ADT.

Starts from the donor schema like tools/mkadt_a5xelte.py and replaces the
board facts with the S8's (live DT and /proc/iomem read from TWRP):
  - XNU RAM: the 2 GiB bank 0x880000000-0x8ffffffff, free of firmware
    carve-outs (the low banks hold ramoops, sec_debug, modem, abox, TIMA, fb)
  - ram_banks=1: /chosen/hakios-ram-banks also offers reservation-filtered
    ranges from both low banks and the rest of the high bank; dram-base and
    dram-size remain the primary 512 MiB bootstrap window
  - GIC-400: GICD 0x10201000, GICC 0x10202000; virtual timer PPI 11 = INTID 27
  - timer 26 MHz (S-Boot leaves CNTFRQ_EL0 = 0; XNU reads this property)
  - CPUs: logical 0-3 = Cortex-A53 MPIDR 0x100-0x103 (S-Boot boots 0x100),
    logical 4-7 = Mongoose M2 MPIDR 0-3; cpus=N still selects a prefix
  - S-Boot framebuffer 1440x2960 XRGB8888 at 0xcc000000
  - no eMMC node: the A5 eMMC driver must not touch S8 MMIO
  - --gpu-node: an optional Mali-G71 node; the kernel driver is still off
    unless boot-arg mali=N is given, and make-boot.sh adds this node only
    for a non-zero mali= argument
The identity stays the donor's iPhone X (D22AP): the S8 has no Home key.
"""
import argparse
import os
from pathlib import Path

import adt
import mkadt

DRAM_BASE = 0x880000000
# 512 MiB, the A5XELTE-proven size: with the whole 2 GiB bank the first S8
# boot panicked in kmem_range_init (kernel VA claims in the 39-bit 4K layout).
DRAM_SIZE = 0x20000000

# Extra banks for XNU's opt-in sparse-RAM path (boot arg ram_banks=1). The
# first pair must equal dram-base/dram-size: it is still boot_args' single
# contiguous bootstrap window. All other pairs are the union of stock DTB
# memory nodes minus reserved-memory, the 0xe0000000 memreserve, initrd, the
# framebuffer and the loader's ramoops/marker ranges. ~3.37 GiB managed in
# total; no physical hole is claimed as pages.
RAM_BANKS = (
    (0x880000000, 0x20000000),  # primary bootstrap window, 512 MiB
    (0x882000000, 0x60000000),  # high bank remainder, 1536 MiB

    (0x80000000, 0x00002000),   # low bank head, 8 KiB
    (0x80004000, 0x01ffc000),   # low bank, ~32 MiB
    (0x82100000, 0x0ff80000),   # low bank, 255 MiB
    (0x92100000, 0x00080000),   # ramoops gap, 512 KiB
    (0x92200000, 0x1ee00000),   # low bank, 494 MiB
    (0xb1200000, 0x0b600000),   # low bank tail, 182 MiB

    (0xc0010000, 0x003f0000),   # second bank head, ~4 MiB
    (0xce800000, 0x01800000),   # second bank, 24 MiB
    (0xd1400000, 0x0ec00000),   # second bank, 236 MiB
    (0xe1900000, 0x02300000),   # second bank, 35 MiB
    (0xeac00000, 0x08800000),   # second bank tail, 136 MiB
)

FB_PHYS, FB_W, FB_H = 0xcc000000, 1440, 2960
TIMEBASE = 26000000

# Verified on the zealous-planck S8 branch (boots 36-49). Present for future
# drivers only: bsd/dev/hakios.mali.c reads these addresses directly and stays
# inert unless boot-arg mali=N is given.
GPU_PHYS, GPU_SIZE = 0x13900000, 0x10000
CMU_G3D_PHYS, CMU_G3D_SIZE = 0x13800000, 0x10000
PMU_G3D_PHYS, PMU_G3D_SIZE = 0x16480000, 0x10000


def set_prop(node, name, value):
    for i, (key, flags, _) in enumerate(node.props):
        if key == name:
            node.props[i] = (key, flags, value)
            return
    node.props.append((name, 0, value))


def child(node, name):
    for candidate in node.children:
        if candidate.prop('name') == mkadt.cstr(name):
            return candidate
    raise ValueError(f'missing donor ADT node {name}')


def cpu_node(logical):
    a53 = logical < 4
    mpidr = 0x100 + logical if a53 else logical - 4
    cluster = 0 if a53 else 1
    # SweetCPU's kernel personality matches only 'arm,cortex-a53' on the
    # A5XELTE/DREAMLTE board.  Keep the descriptive Mongoose compatible for
    # userspace, but advertise the A53 string on every core so IOKit creates
    # a SweetCPU nub for logical CPUs 4-7 too.
    if a53:
        compatible = mkadt.cstr('arm,cortex-a53')
    else:
        compatible = (mkadt.cstr('samsung,mongoose-m2') +
                      mkadt.cstr('arm,cortex-a53'))
    return [
        ('device_type', mkadt.cstr('cpu')),
        ('compatible', compatible +
         mkadt.cstr('ARM,v8')),
        ('reg', mkadt.u32(mpidr)),
        ('cpu-id', mkadt.u32(logical)),
        ('cluster-id', mkadt.u32(cluster)),
        ('cluster-type', mkadt.u32(0)),
        ('die-cluster-id', mkadt.u32(cluster)),
        ('cluster-core-id', mkadt.u32(logical % 4)),
        ('l2-cache-id', mkadt.u32(cluster)),
        ('l3-cache-id', mkadt.u32(0)),
        ('state', mkadt.cstr('running' if logical == 0 else 'stopped')),
        ('timebase-frequency', mkadt.u32(TIMEBASE)),
        ('clock-frequency', mkadt.u32(1690000000 if a53 else 2314000000)),
        ('bus-frequency', mkadt.u32(100000000)),
        ('memory-frequency', mkadt.u32(1794000000)),
        ('peripheral-frequency', mkadt.u32(TIMEBASE)),
        ('fixed-frequency', mkadt.u32(TIMEBASE)),
    ]


def gpu_node():
    return mkadt.node('mali', [
        ('device_type', mkadt.cstr('gpu')),
        ('compatible', mkadt.cstr('samsung,exynos8895-mali') + mkadt.cstr('arm,mali-g71')),
        # No interrupt is claimed: the S8 evidence is register polling only.
        ('reg', mkadt.u64(GPU_PHYS, GPU_SIZE,
                          CMU_G3D_PHYS, CMU_G3D_SIZE,
                          PMU_G3D_PHYS, PMU_G3D_SIZE)),
        ('reg-names', mkadt.cstr('gpu') + mkadt.cstr('cmu-g3d') + mkadt.cstr('pmu-g3d')),
        ('status', mkadt.cstr('okay')),
        ('boot-arg', mkadt.cstr('mali=N')),
    ])


def build(canvas=(FB_W, FB_H), gpu=False):
    root = mkadt.build(seed=os.urandom(64), qemu=True)
    # The kernel's SweetPlatformExpert personality matches 'samsung,a5xelte'
    # (KernelConfigTables.cpp); without it IOKit panics "Unable to find driver
    # for this platform" (S8 boot 3). IONameMatch accepts any listed name.
    set_prop(root, 'compatible', mkadt.cstr('samsung,dreamlte') +
             mkadt.cstr('samsung,a5xelte') + mkadt.cstr('samsung,exynos8895') +
             mkadt.cstr('AppleARM'))
    chosen = child(root, 'chosen')
    set_prop(chosen, 'dram-base', mkadt.u64(DRAM_BASE))
    set_prop(chosen, 'dram-size', mkadt.u64(DRAM_SIZE))
    set_prop(chosen, 'hakios-ram-banks',
             b''.join(mkadt.u64(base, size) for base, size in RAM_BANKS))
    set_prop(chosen, 'firmware-version', mkadt.cstr('dreamlte-entry'))
    set_prop(chosen, 'system-firmware-version', mkadt.cstr('dreamlte-entry'))
    cpus = child(root, 'cpus')
    cpu0 = child(cpus, 'cpu0')
    for key, value in cpu_node(0):
        if key != 'cpu-id':
            set_prop(cpu0, key, value)
    for logical in range(1, 8):
        cpus.children.append(mkadt.node(f'cpu{logical}', cpu_node(logical)))
    arm_io = child(root, 'arm-io')
    set_prop(arm_io, 'device_type', mkadt.cstr('dreamlte-io'))
    set_prop(arm_io, 'compatible', mkadt.cstr('samsung,exynos8895-io') + mkadt.cstr('arm-io'))
    set_prop(arm_io, 'ranges', mkadt.u64(0, 0x10000000, 0xf0000000))
    arm_io.children = [n for n in arm_io.children if n.prop('name') == mkadt.cstr('wdt')]
    arm_io.children.append(mkadt.node('gic', [
        ('compatible', mkadt.cstr('arm,cortex-a15-gic')),
        ('interrupt-controller', mkadt.cstr('master')),
        # Non-secure physical timer (PPI 14): Samsung's DT says
        # "use-physical-timer"; the kernel (A5_USE_PHYS_TIMER) uses CNTP.
        ('virtual-timer-intid', mkadt.u32(30)),
        ('reg', mkadt.u64(0x0201000, 0x1000, 0x0202000, 0x1000)),
    ]))
    if gpu:
        arm_io.children.append(gpu_node())
    arm_io.children.append(mkadt.node('disp0', [
        ('device_type', mkadt.cstr('display-subsystem')),
        # AppleCLCD (SweetFramebuffer.cpp) matches 'samsung,a5xelte-display';
        # without it no main display exists and SpringBoard aborts (boot 16).
        ('compatible', mkadt.cstr('samsung,dreamlte-display') +
         mkadt.cstr('samsung,a5xelte-display')),
        # Must equal the size AppleCLCD reports (GetDisplaySize): 1125x2436
        # unless sweetfb_native=1. Boot 18 presented 1125x2436 with a
        # 1440x2960 canvas here and stalled after the first SwapBegin.
        ('canvas-width', mkadt.u32(canvas[0])),
        ('canvas-height', mkadt.u32(canvas[1])),
    ]))
    root.children = [n for n in root.children if n.prop('name') not in
                     (mkadt.cstr('sweet-fbcon'), mkadt.cstr('sweet-ufs'))]
    # pe_serial.c looks the console up by this name on the A5XELTE config.
    root.children.append(mkadt.node('a5xelte-fbcon', [
        ('reg', mkadt.u64(FB_PHYS, FB_W * FB_H * 4)),
        ('width', mkadt.u32(FB_W)),
        ('height', mkadt.u32(FB_H)),
        ('stride', mkadt.u32(FB_W * 4)),
    ]))
    # bsd/dev/sweet_ufs.c (A5XELTE config, EXYNOS_UFS): UFSHCI standard
    # registers, then the Exynos vendor HCI block (HCI_UTRL_NEXUS_TYPE etc.).
    root.children.append(mkadt.node('dreamlte-ufs', [
        ('reg', mkadt.u64(0x11120000, 0x200, 0x11121100, 0x200)),
    ]))
    return root


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--canvas', default=f'{FB_W}x{FB_H}',
                        help='display size presented to iOS, WxH')
    parser.add_argument('--gpu-node', action='store_true',
                        help='include the Mali-G71 node (the kernel still needs mali=N)')
    args = parser.parse_args()
    blob = adt.serialize(build(tuple(int(v) for v in args.canvas.split('x')), gpu=args.gpu_node))
    args.output.write_bytes(blob)
    print(f'{args.output}: {len(blob)} bytes')
