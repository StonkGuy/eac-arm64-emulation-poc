#!/bin/sh
# ./build.sh [clang] [output]  — freestanding x86-64 micro-benchmarks (single object; uses tools/static-link.py if no usable lld)
set -e
CC=${1:-clang}; OUT=${2:-fexbench}; HERE=$(cd "$(dirname "$0")" && pwd)
python3 "$HERE/gen_bigcode.py" "$HERE/bigcode.h"
EXTRA=${EXTRA:-}
CF="$EXTRA --target=x86_64-linux-gnu -O2 -g0 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-builtin -msse4.1"
$CC $CF -I"$HERE" -c -o "$OUT.o" "$HERE/fexbench.c"
if $CC --target=x86_64-linux-gnu -nostdlib -static -fuse-ld=lld -Wl,-e,_start -Wl,-z,norelro -Wl,--build-id=none -o "$OUT" "$OUT.o" 2>/dev/null; then :; else python3 "$HERE/../../tools/static-link.py" "$OUT.o" "$OUT"; fi
rm -f "$OUT.o"; echo "built $OUT"
