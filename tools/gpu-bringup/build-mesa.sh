#!/bin/bash
# Build the pinned Mesa 25.0.7 tree for Darwin/iOS on WSL, with the local
# Hakios DRM stand-in. Produces libOSMesa.8.dylib, exported-gl.txt and gl.xml.
#
#   DRIVERS=softpipe            ./build-mesa.sh     # first, CPU-only proof
#   DRIVERS=softpipe,panfrost   ./build-mesa.sh     # after mali=9 is known good
#
# Needs the tools checked by preflight-wsl.sh. Nothing is flashed, mounted or
# copied into a root image here.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
MESA=$HERE/mesa
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
URL=https://archive.mesa3d.org/mesa-$VERSION.tar.xz

BUILD=${BUILD:-$HERE/.build/mesa}
OUT=${OUT:-$HERE/out/mesa}
SDK=${SDK:-${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk}
CC_WRAP=${CC_WRAP:-$HERE/toolchain/bin/xnu-clang}
CXX_WRAP=${CXX_WRAP:-$HERE/toolchain/bin/xnu-clang++}
DRIVERS=${DRIVERS:-softpipe}

for tool in curl tar sha256sum patch python3 llvm-strip llvm-nm install_name_tool; do
	command -v "$tool" >/dev/null || { echo "build-mesa.sh: missing $tool" >&2; exit 1; }
done
for path in "$SDK" "$CC_WRAP" "$CXX_WRAP"; do
	[ -e "$path" ] || { echo "build-mesa.sh: missing $path" >&2; exit 1; }
done

mkdir -p "$BUILD" "$OUT"
cd "$BUILD"
PATCH_HASH=$(cat "$MESA"/*.patch "$MESA"/hakios-drm/* | sha256sum | cut -d' ' -f1)
SRC=$BUILD/mesa-$VERSION
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.hakios-patches" 2>/dev/null)" != "$PATCH_HASH" ]; then
	[ -f "mesa-$VERSION.tar.xz" ] || curl -sSL --fail -o "mesa-$VERSION.tar.xz" "$URL"
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf "mesa-$VERSION" build-*
	tar xJf "mesa-$VERSION.tar.xz"
	for p in "$MESA"/*.patch; do
		echo "build-mesa.sh: applying $(basename "$p")"
		patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$p"
	done
	echo "$PATCH_HASH" > "$SRC/.hakios-patches"
fi
rm -rf "$SRC/src/hakios-drm"
cp -a "$MESA/hakios-drm" "$SRC/src/hakios-drm"

if [ ! -x "$BUILD/venv/bin/meson" ] || [ ! -x "$BUILD/venv/bin/ninja" ]; then
	python3 -m venv "$BUILD/venv"
	"$BUILD/venv/bin/pip" -q install meson ninja mako pyyaml packaging
fi
MESON=${MESON:-$BUILD/venv/bin/meson}
NINJA=${NINJA:-$BUILD/venv/bin/ninja}
export PATH="$BUILD/venv/bin:$PATH"

SAFE=${DRIVERS//,/-}
CROSS=$BUILD/cross-$SAFE.ini
cat > "$CROSS" <<INI
[binaries]
c = ['$CC_WRAP', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$CXX_WRAP', '-arch', 'arm64', '-isysroot', '$SDK']
objc = ['$CC_WRAP', '-arch', 'arm64', '-isysroot', '$SDK']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI

EXTRA=
case ",$DRIVERS," in
*,freedreno,*) EXTRA="-Dfreedreno-kmds=msm" ;;
esac
# shellcheck disable=SC2086
"$MESON" setup "$BUILD/build-$SAFE" "$SRC" \
	--cross-file "$CROSS" --wrap-mode=nofallback \
	--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" $EXTRA \
	-Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled \
	-Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled \
	-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
	-Dglvnd=disabled -Dvideo-codecs= -Dtools= \
	-Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled \
	-Dlmsensors=disabled -Dbuild-tests=false
"$NINJA" -C "$BUILD/build-$SAFE"

llvm-strip -x -o "$OUT/libOSMesa.8.dylib" \
	"$BUILD/build-$SAFE/src/gallium/targets/osmesa/libOSMesa.8.dylib"
install_name_tool -id /redwood/lib/libOSMesa.8.dylib "$OUT/libOSMesa.8.dylib"
llvm-nm -gU "$OUT/libOSMesa.8.dylib" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > "$OUT/exported-gl.txt"
cp "$SRC/src/mapi/glapi/registry/gl.xml" "$OUT/gl.xml"
cat "$MESA"/*.patch "$MESA"/hakios-drm/* | sha256sum > "$OUT/built-from"
echo "build-mesa.sh: Mesa $VERSION ($DRIVERS): $OUT/libOSMesa.8.dylib" \
	"($(stat -c %s "$OUT/libOSMesa.8.dylib") bytes," \
	"$(wc -l < "$OUT/exported-gl.txt") gl* functions)"
