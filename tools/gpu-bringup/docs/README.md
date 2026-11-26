# Galaxy S8 Mali-G71 bring-up (local, opt-in)

Everything here lives on `gpu-bringup`. `main`, `ios16-root.img`, boot images
and the phone are deliberately untouched. No step in this directory flashes,
mounts, rebuilds a boot image or edits a root image.

## What is in the patch

`xnu/patches/0126-dreamlte-hakios-mali.patch` adds three files and two build
list lines:

- `bsd/dev/hakios.mali.c` — S8 constants, nine staged steps, legacy Mali LPAE
  page tables, a one-job-at-a-time queue and the C API used by the service.
- `bsd/dev/hakios.mali_va.h` — 32 MiB–4 GiB GPU VA allocator, 64 KiB granules,
  with executable buffers kept inside one 16 MiB line.
- `iokit/Kernel/hakios.mali.cpp` — `HakiosMali` / `HakiosMaliUserClient` with
  the Panfrost DRM selectors Mesa 25.0.7 needs.
- `bsd/conf/files`, `iokit/conf/files`, and a gated `hakios_mali_init()` call
  from `bsd/dev/sweet_ufs.c` after the existing S8 init sequence.

`DREAMLTE` and `ARM64_BOARD_CONFIG_A5XELTE` are required. On any other config
the C file compiles to inert stubs. The service also refuses clients until the
nine steps have completed.

### Boot arg gate

| argument | effect |
| --- | --- |
| absent or `mali=0` | no MMIO mapping, no register read, no service |
| `mali=1` | map PMU/CMU/GPU and read boot state only |
| `mali=2` | enables the G3D domain if it is off (no PMIC write) |
| `mali=3` | adds bus divider reads |
| `mali=4` | adds GPU_ID/capability checks |
| `mali=5` | adds soft/hard reset |
| `mali=6` | adds L2/shader/tiler power-on |
| `mali=7` | adds legacy page tables and a WRITE_VALUE job |
| `mali=8` | adds a second GPU VA job |
| `mali=9` | starts `HakiosMali` for Mesa |

Use `mali_trace=1` while bringing up. `mali_fastmap=1` skips AS0 table
flushes after mapping and is unverified on G71; leave it off first.

**Never combine `mali=9` with `s8gpu=4`, `s8metal=1`, `sweetfb_gpu=1` or
`/dev/s8mali`.** `tools/dreamlte/make-boot.sh` refuses such a combination and
adds the optional ADT node only for a non-zero `mali=` argument.

There is no port of the S9 ACPM driver: the S8 mailbox/SRAM layout differs.
The rail and PLL are used as S-Boot left them; there is no PMIC or clock-rate
write in this patch.

## ADT

`tools/mkadt_dreamlte.py --gpu-node` adds `/arm-io/mali` with the verified
GPU/CMU/PMU ranges and `boot-arg = "mali=N"`. It is descriptive only: the
driver reads those addresses directly and ignores the node. Without
`--gpu-node` no GPU node is emitted.

## WSL build

The cross build needs an iPhoneOS SDK, Apple-target clang, cctools (`ld64`,
`install_name_tool`) and LLVM tools. On Ubuntu/Debian WSL:

```sh
sudo apt install build-essential curl python3 python3-venv patch binutils \
    clang lld llvm
```

Meson, ninja, Mako, PyYAML and packaging are installed into a private venv by
the script. `THEOS` (default `$HOME/theos`) provides the SDK and cctools;
override with `SDK=...`, `LD64=...` as needed.

```sh
bash tools/gpu-bringup/preflight-wsl.sh

# First: CPU-only proof that eagl.c + OSMesa + the generated slot table link.
DRIVERS=softpipe bash tools/gpu-bringup/build-mesa.sh
MESA_OUT=$PWD/tools/gpu-bringup/out/mesa \
    bash tools/gpu-bringup/build-userland.sh

# Only after mali=1..8 logs are understood:
DRIVERS=softpipe,panfrost bash tools/gpu-bringup/build-mesa.sh
MESA_OUT=$PWD/tools/gpu-bringup/out/mesa \
    bash tools/gpu-bringup/build-userland.sh
```

Expected outputs:

- `tools/gpu-bringup/out/mesa/libOSMesa.8.dylib` (install name
  `/redwood/lib/libOSMesa.8.dylib`), `exported-gl.txt`, `gl.xml`, `built-from`
- `tools/gpu-bringup/out/userland/libhakios.gl.dylib`,
  `libOSMesa.8.dylib`, `gl_test`

Mesa is pinned to 25.0.7 with
`SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6`.
Its three local patches add the Darwin/Panfrost DRM bridge, an OSMesa ES
profile and a clang pack-macro fix.

To place the files in a root tree, mount it yourself and then run:

```sh
bash tools/gpu-bringup/install-to-root.sh /mnt/hfsroot
```

That script only copies into `$ROOT/redwood/lib` and `$ROOT/redwood/bin`; it
does not mount, unmount, write `ios16-root.img` or touch the phone. The target
iOS 16.7.16 root has no `/redwood` yet, so this is a prepared step, not one to
run as part of this change.

## Deliberately not ported

- S9 `MiniIOSurface`/display hooks and selectors `0x7c`, `0x7e`, `0x7f`. The
  reduced service implements `0x7d` (wait by sequence) and the Panfrost
  selectors `0x09`, `0x40`–`0x45`, `0x48`. `gl_osmesa.c` falls back to copying
  an IOSurface when `0x7e` is unsupported, so CPU-composited frames still
  work; GPU-textured CoreAnimation surfaces remain a separate follow-up.
- S9 `HakiosGPU`, Freedreno, `MASTER.hakios`, `HAKIOS_DRIVERS` and ACPM.
- The S9 injection addresses: they are for iOS 16.5 `20F66__iPhone10,3_6`,
  not the S8's iOS 16.7.16 `20H392`. No injection is attempted here.

## Verification done here

- `0126` applies with `git apply --check` to a locally replayed tree carrying
  patches `0001`–`0125`, and applies cleanly in a copy of that tree.
- `hakios.mali.c` and `hakios.mali.cpp` pass host-side `gcc`/`g++`
  `-fsyntax-only` checks with local XNU/IOKit stubs.
- `test-va.c` passes: no overlapping allocations, correct free/misalignment
  rejection and no executable buffer crossing a 16 MiB line.
- `mkadt_dreamlte.py` compiles; with `--gpu-node` the parsed ADT contains
  `/arm-io/mali`, and without it there is no Mali node.

Not done: a real cross build (this WSL has no clang, meson, ninja, SDK or
cctools yet), an XNU compile in Actions, any boot, and any phone or image
change.
