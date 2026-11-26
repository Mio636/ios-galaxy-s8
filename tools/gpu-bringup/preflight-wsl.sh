#!/bin/bash
# Check the WSL side of the S8 GLES bring-up. Does not install anything and
# does not touch the phone, the boot image or ios16-root.img.
set -uo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
fail=0

need() {
	command -v "$1" >/dev/null 2>&1 || { echo "missing command: $1"; fail=1; }
}

for tool in bash python3 curl tar sha256sum patch clang clang++ llvm-ar llvm-ranlib llvm-strip llvm-nm install_name_tool; do
	need "$tool"
done

SDK=${SDK:-${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk}
if [ ! -d "$SDK" ]; then
	echo "missing iPhoneOS SDK: $SDK (set SDK=/path/to/iPhoneOS16.5.sdk)"
	fail=1
fi
for framework in CoreFoundation IOKit IOSurface; do
	if [ ! -d "$SDK/System/Library/Frameworks/$framework.framework" ]; then
		echo "missing framework in $SDK: $framework.framework"
		fail=1
	fi
done

LD64=${LD64:-$HERE/toolchain/bin/ld64}
if [ ! -x "$LD64" ]; then
	echo "missing ld64: $LD64 (set LD64=/path/to/ld64.lld or install cctools)"
	fail=1
fi
for wrapper in xnu-clang xnu-clang++; do
	if [ ! -x "$HERE/toolchain/bin/$wrapper" ]; then
		echo "missing wrapper: $HERE/toolchain/bin/$wrapper"
		fail=1
	fi
done

if [ "$fail" -ne 0 ]; then
	cat >&2 <<EOF

On Ubuntu/Debian WSL, the usual prerequisites are:
  sudo apt install build-essential curl python3 python3-venv patch binutils \
      clang lld llvm

Meson, ninja, Mako, PyYAML and packaging are installed into a private
venv by build-mesa.sh. A cctools ld64 and install_name_tool are still
required for Mach-O linking; THEOS ships both.
EOF
	exit 1
fi
echo "preflight-wsl: OK (SDK $SDK, ld64 $LD64)"
