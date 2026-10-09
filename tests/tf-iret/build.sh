#!/bin/sh
# Builds the freestanding static test binary.
#   ./build.sh [clang] [output]
#
# The binary is x86-64: the test executes POPFQ/IRETQ with TF and reads x86 ucontext fields, so it only makes sense as an
# x86-64 binary. Run it under FEX, and run the same binary on a real x86-64 Linux kernel for the reference result.
set -e
CC=${1:-clang}; OUT=${2:-tf_iret_test}
HERE=$(cd "$(dirname "$0")" && pwd)
CFLAGS="--target=x86_64-linux-gnu -O1 -g0 -ffreestanding -fno-stack-protector -fno-builtin -fno-pic -fno-pie -fno-asynchronous-unwind-tables -Wall"
$CC $CFLAGS -c -o "$OUT.o" "$HERE/tf_iret_test.c"
if $CC --target=x86_64-linux-gnu -nostdlib -static -fuse-ld=lld -Wl,-e,_start -Wl,-z,norelro -Wl,--build-id=none -o "$OUT" "$OUT.o" 2>/dev/null; then
  :
else
  python3 "$HERE/../../tools/static-link.py" "$OUT.o" "$OUT"
fi
rm -f "$OUT.o"
echo "built $OUT"
