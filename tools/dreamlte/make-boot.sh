#!/bin/bash
# Kernel Mach-O -> Galaxy S8 BOOT image.
#   make-boot.sh KERNEL STOCK_BOOT.img OUT.img [boot-args...]
# Default boot-args: one CPU, verbose console, no watchdog; no root device
# yet, so the first trials end in a root-mount panic by design.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
kernel=$(realpath "$1"); stock=$(realpath "$2"); out=$(realpath -m "$3")
shift 3
# a5touchprobe=0: the A5 touch probe maps Exynos7580 I2C/GPIO; on the S8 its
# first read is a synchronous external abort (boot 8, a5xelte_touch_probe).
args=${*:-debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 wdt_test=no_user_checkin a5touchprobe=0}

# The S8's two GPU paths are incompatible. Never package a boot image that
# would start S8GPU//dev/s8mali and HakiosMali in the same boot.
gpu_node=()
other_gpu=
for arg in $args; do
	case "$arg" in
	mali=*)
		n=${arg#mali=}
		if [[ "$n" =~ ^[1-9][0-9]*$ ]]; then gpu_node=(--gpu-node); fi
		;;
	s8gpu=*|s8metal=*|sweetfb_gpu=*)
		other_gpu="$other_gpu $arg"
		;;
	esac
done
if [ -n "$other_gpu" ] && [ "${#gpu_node[@]}" -ne 0 ]; then
	echo "make-boot.sh: do not combine mali=N with:$other_gpu" >&2
	exit 1
fi

work=$repo/shim/dreamlte/out
mkdir -p "$work"
# The ADT canvas must match the size the kernel presents (SweetFramebuffer).
canvas=1125x2436
[[ " $args " == *" sweetfb_native=1 "* ]] && canvas=1440x2960
python3 "$repo/tools/mkadt_dreamlte.py" "$work/dreamlte.adt" --canvas "$canvas" "${gpu_node[@]}"
python3 "$repo/tools/mkpayload.py" "$kernel" "$work/dreamlte.adt" "$work/payload.bin" --cmdline "$args"
bash "$repo/shim/dreamlte/build.sh" "$stock" "$work/payload.bin" "$out"
python3 "$repo/tools/dreamlte_bootimg_info.py" "$out" | head -4
