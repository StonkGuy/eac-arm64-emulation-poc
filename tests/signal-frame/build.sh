#!/bin/sh
# Builds the freestanding static test binary.
#   ./build.sh [clang] [output]     x86-64 binary (any host with clang and its x86-64 target): the one to run under FEX
#   ./build.sh native [output]      x86-64 hosts only, built with the system cc: the reference run on a real Linux
#                                   kernel (every check is an x86-64 frame property; there is no aarch64 reference)
set -e
CC=${1:-clang}; OUT=${2:-signal_frame_test}
HERE=$(cd "$(dirname "$0")" && pwd)
if [ "$CC" = native ]; then
  [ "$(uname -m)" = x86_64 ] || { echo "build.sh native: the signal-frame reference run needs an x86-64 Linux host" >&2; exit 1; }
  ${NATIVE_CC:-cc} -O1 -g0 -ffreestanding -fno-stack-protector -fno-builtin -fno-pic -fno-pie -fno-asynchronous-unwind-tables -Wall \
    -nostdlib -static -Wl,-e,_start -Wl,-z,norelro -o "$OUT" "$HERE/signal_frame_test.c"
  echo "built $OUT (native)"; exit 0
fi
CFLAGS="--target=x86_64-linux-gnu -O1 -g0 -ffreestanding -fno-stack-protector -fno-builtin -fno-pic -fno-pie -fno-asynchronous-unwind-tables -Wall"
$CC $CFLAGS -c -o "$OUT.o" "$HERE/signal_frame_test.c"
if $CC --target=x86_64-linux-gnu -nostdlib -static -fuse-ld=lld -Wl,-e,_start -Wl,-z,norelro -Wl,--build-id=none -o "$OUT" "$OUT.o" 2>/dev/null; then
  :
else
  python3 "$HERE/../../tools/static-link.py" "$OUT.o" "$OUT"
fi
rm -f "$OUT.o"
echo "built $OUT"
