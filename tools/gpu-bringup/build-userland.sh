#!/bin/bash
# Build the S8 GLES userland dylibs against a Mesa output from build-mesa.sh.
#
#   MESA_OUT=/path/to/out/mesa ./build-userland.sh
#
# Outputs in out/userland:
#   libOSMesa.8.dylib     copied from Mesa (install name /redwood/lib/...)
#   libhakios.gl.dylib    EAGLContext shim behind CoreAnimation
#   gl_test               one triangle, read back and checked in-process
#
# This only writes files under this repository. It never mounts or edits a
# root image; use install-to-root.sh for that, on an already-mounted root.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
SRC=$HERE/userland/src
OUT=${OUT:-$HERE/out/userland}
OBJ=$HERE/.build/userland
MESA_OUT=${MESA_OUT:-$HERE/out/mesa}
SDK=${SDK:-${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk}
CC_WRAP=${CC_WRAP:-$HERE/toolchain/bin/xnu-clang}
LD64=${LD64:-$HERE/toolchain/bin/ld64}
export XNU_SDK_VERSION=${XNU_SDK_VERSION:-16.7}

for f in "$MESA_OUT/libOSMesa.8.dylib" "$MESA_OUT/exported-gl.txt" "$MESA_OUT/gl.xml"; do
	[ -f "$f" ] || { echo "build-userland.sh: missing $f; run build-mesa.sh first" >&2; exit 1; }
done
[ -d "$SDK" ] || { echo "build-userland.sh: missing SDK $SDK" >&2; exit 1; }
[ -x "$CC_WRAP" ] || { echo "build-userland.sh: missing $CC_WRAP" >&2; exit 1; }
[ -x "$LD64" ] || { echo "build-userland.sh: missing ld64 $LD64" >&2; exit 1; }

mkdir -p "$OUT" "$OBJ"
python3 "$HERE/userland/mkeagl.py" "$SRC/eagl_slots.txt" \
	"$MESA_OUT/gl.xml" "$MESA_OUT/exported-gl.txt" "$SRC/eagl.c" > "$OBJ/eagl_gen.h"
"$CC_WRAP" -arch arm64 -isysroot "$SDK" -O2 -c -o "$OBJ/surface_checksum.o" \
	"$SRC/surface_checksum.c"

FLAGS=(-arch arm64 -isysroot "$SDK" -O1 -I"$OBJ" -I"$SRC" -fno-stack-protector)
"$CC_WRAP" "${FLAGS[@]}" -dynamiclib \
	-install_name /redwood/lib/libhakios.gl.dylib \
	-framework CoreFoundation -framework IOKit -framework IOSurface -lobjc \
	"$MESA_OUT/libOSMesa.8.dylib" -o "$OUT/libhakios.gl.dylib" \
	"$SRC/eagl.c" "$SRC/gl_osmesa.c" "$OBJ/surface_checksum.o"
"$CC_WRAP" "${FLAGS[@]}" -framework CoreFoundation -framework IOKit \
	-framework IOSurface "$MESA_OUT/libOSMesa.8.dylib" -o "$OUT/gl_test" \
	"$SRC/gl_test.c"
cp -f "$MESA_OUT/libOSMesa.8.dylib" "$OUT/libOSMesa.8.dylib"
cat "$MESA_OUT/built-from" > "$OUT/mesa-built-from"
echo "build-userland.sh: $OUT/libhakios.gl.dylib, $OUT/gl_test, $OUT/libOSMesa.8.dylib"
