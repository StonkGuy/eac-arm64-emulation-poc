#!/bin/sh
# Builds the freestanding static test binary.
#   ./build.sh [clang] [output]     x86-64 binary (any host with clang and its x86-64 target): run it under FEX, and
#                                   unchanged on a real x86-64 Linux for the reference
set -e
CC=${1:-clang}; OUT=${2:-proc_maps_test}
HERE=$(cd "$(dirname "$0")" && pwd)
CFLAGS="--target=x86_64-linux-gnu -O1 -g0 -ffreestanding -fno-stack-protector -fno-builtin -fno-pic -fno-pie -fno-asynchronous-unwind-tables -Wall"
$CC $CFLAGS -c -o "$OUT.o" "$HERE/proc_maps_test.c"
if $CC --target=x86_64-linux-gnu -nostdlib -static -fuse-ld=lld -Wl,-e,_start -Wl,-z,norelro -Wl,--build-id=none -o "$OUT" "$OUT.o" 2>/dev/null; then
  :
else
  python3 "$HERE/../../tools/static-link.py" "$OUT.o" "$OUT"
fi
rm -f "$OUT.o"
echo "built $OUT"
