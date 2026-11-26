#!/bin/bash
# Copy the built GLES libraries into an ALREADY-MOUNTED root filesystem.
#
#   install-to-root.sh /mnt/hfsroot
#
# This script does not mount, unmount, loopback, flash, rebuild a boot image,
# or write ios16-root.img. It refuses an unmounted directory unless
# ALLOW_UNMOUNTED=1 is set deliberately.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=${1:?usage: install-to-root.sh MOUNTED_ROOT [OUT_DIR]}
SRC=${2:-$HERE/out/userland}
LIBDIR=${LIBDIR:-redwood/lib}

[ -d "$ROOT" ] || { echo "install-to-root.sh: $ROOT is not a directory" >&2; exit 1; }
[ "$ROOT" != "/" ] || { echo "install-to-root.sh: refusing /" >&2; exit 1; }
if ! mountpoint -q "$ROOT" && [ "${ALLOW_UNMOUNTED:-0}" != 1 ]; then
	echo "install-to-root.sh: $ROOT is not a mountpoint; set ALLOW_UNMOUNTED=1 only if you know it is a root tree" >&2
	exit 1
fi
for f in libOSMesa.8.dylib libhakios.gl.dylib gl_test; do
	[ -f "$SRC/$f" ] || { echo "install-to-root.sh: missing $SRC/$f" >&2; exit 1; }
done

install -d -o 0 -g 0 -m 0755 "$ROOT/$LIBDIR"
install -d -o 0 -g 0 -m 0755 "$ROOT/$LIBDIR/../bin"
for f in libOSMesa.8.dylib libhakios.gl.dylib; do
	install -o 0 -g 0 -m 0755 "$SRC/$f" "$ROOT/$LIBDIR/$f"
	echo "installed $ROOT/$LIBDIR/$f"
done
install -o 0 -g 0 -m 0755 "$SRC/gl_test" "$ROOT/$LIBDIR/../bin/gl_test"
echo "installed $ROOT/$LIBDIR/../bin/gl_test"
echo "install-to-root.sh: no image, boot or device was touched"
