#!/bin/bash
# Regenerate xnu/patches/0126-dreamlte-hakios-mali.patch from the files in
# tools/gpu-bringup/kernel/.  The patch is made against the tree that results
# from xnu/patches/0001..0125, not against a pristine XNU checkout, so it can
# be dropped in beside the existing patches and applied by dxb's build.sh.
set -euo pipefail

repo=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
xnu=${XNU_SRC:-/home/mio/.cache/opencode/xnu8796}
out="$repo/xnu/patches/0126-dreamlte-hakios-mali.patch"
tmp=${TMPDIR:-/tmp}/hackios-mali-patch.$$
trap 'rm -rf "$tmp"' EXIT

[[ -d "$xnu/.git" ]] || { echo "no clean XNU clone at $xnu" >&2; exit 1; }
cp -a "$xnu" "$tmp"
cd "$tmp"
git reset --hard -q HEAD
git clean -fdq

# Only the three files this patch touches need the earlier patches' context.
for p in "$repo"/xnu/patches/*.patch; do
    [[ "$(basename "$p")" == 0126-* ]] && continue
    git apply --include='bsd/conf/files' --include='iokit/conf/files' \
        --include='bsd/dev/sweet_ufs.c' --whitespace=nowarn "$p" 2>/dev/null || true
done
git add -A
git -c user.email=local@example -c user.name=local commit -qm base

cp "$repo/tools/gpu-bringup/kernel/hakios.mali.c" bsd/dev/hakios.mali.c
cp "$repo/tools/gpu-bringup/kernel/hakios.mali_va.h" bsd/dev/hakios.mali_va.h
cp "$repo/tools/gpu-bringup/kernel/hakios.mali.cpp" iokit/Kernel/hakios.mali.cpp

python3 - "$tmp" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])

def replace(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise SystemExit(f'{path}: expected one occurrence of {old!r}, got {text.count(old)}')
    path.write_text(text.replace(old, new))

replace(root / 'bsd/conf/files',
        'bsd/dev/sweet_sample.c\t\t\tstandard\n',
        'bsd/dev/sweet_sample.c\t\t\tstandard\nbsd/dev/hakios.mali.c\t\t\tstandard\n')
replace(root / 'iokit/conf/files',
        'iokit/Kernel/IOUserClient.cpp\t\t\t\toptional iokitcpp\n',
        'iokit/Kernel/IOUserClient.cpp\t\t\t\toptional iokitcpp\n'
        'iokit/Kernel/hakios.mali.cpp\t\t\toptional iokitcpp\n')
replace(root / 'bsd/dev/sweet_ufs.c',
        'extern void sweet_usb_probe(void);\n',
        'extern void sweet_usb_probe(void);\n'
        '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
        'extern void hakios_mali_init(void);\n'
        '#endif\n')
replace(root / 'bsd/dev/sweet_ufs.c',
        '#endif\n\treturn 0;\n}\n\n/*\n * Persistent console log',
        '#endif\n#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
        '\t/* Only the opt-in boot-arg mali=N touches the GPU; see bsd/dev/hakios.mali.c. */\n'
        '\thakios_mali_init();\n#endif\n\treturn 0;\n}\n\n/*\n * Persistent console log')
PY

git add -A
git diff --cached --no-ext-diff --binary > "$out"
echo "wrote $out ($(wc -l < "$out") lines)"
