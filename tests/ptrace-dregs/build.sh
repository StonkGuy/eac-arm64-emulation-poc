#!/bin/sh
# Builds the freestanding static x86-64 test binary on any host that has clang with the x86-64 target.
#   ./build.sh [clang] [output]
# Uses the host's ld.lld when it works, otherwise falls back to tools/static-link.py (single object, no libc).
set -e
CC=${1:-clang}; OUT=${2:-ptrace_dregs_test}
HERE=$(cd "$(dirname "$0")" && pwd)
CFLAGS="--target=x86_64-linux-gnu -O1 -g0 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-builtin -Wall -Wno-unused-function"
$CC $CFLAGS -c -o "$OUT.o" "$HERE/ptrace_dregs_test.c"
if $CC --target=x86_64-linux-gnu -nostdlib -static -fuse-ld=lld -Wl,-e,_start -Wl,-z,norelro -Wl,--build-id=none -o "$OUT" "$OUT.o" 2>/dev/null; then
  :
else
  python3 "$HERE/../../tools/static-link.py" "$OUT.o" "$OUT"
fi
rm -f "$OUT.o"
echo "built $OUT"
