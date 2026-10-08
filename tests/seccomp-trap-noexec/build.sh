#!/bin/sh
# Builds the freestanding static test binary.
#   ./build.sh [clang] [output]     x86-64 binary (any host with clang and its x86-64 target): the one to run under FEX
#   ./build.sh native [output]      binary for the host's own architecture (x86-64 or aarch64), built with the system cc:
#                                   the reference run on a real Linux kernel
set -e
CC=${1:-clang}; OUT=${2:-seccomp_trap_noexec_test}
HERE=$(cd "$(dirname "$0")" && pwd)
if [ "$CC" = native ]; then
  ${NATIVE_CC:-cc} -O1 -g0 -ffreestanding -fno-stack-protector -fno-builtin -fno-pic -fno-pie -fno-asynchronous-unwind-tables -Wall \
    -nostdlib -static -Wl,-e,_start -Wl,-z,norelro -o "$OUT" "$HERE/seccomp_trap_noexec_test.c"
  echo "built $OUT (native)"; exit 0
fi
CFLAGS="--target=x86_64-linux-gnu -O1 -g0 -ffreestanding -fno-stack-protector -fno-builtin -fno-pic -fno-pie -fno-asynchronous-unwind-tables -Wall"
$CC $CFLAGS -c -o "$OUT.o" "$HERE/seccomp_trap_noexec_test.c"
if $CC --target=x86_64-linux-gnu -nostdlib -static -fuse-ld=lld -Wl,-e,_start -Wl,-z,norelro -Wl,--build-id=none -o "$OUT" "$OUT.o" 2>/dev/null; then
  :
else
  python3 "$HERE/../../tools/static-link.py" "$OUT.o" "$OUT"
fi
rm -f "$OUT.o"
echo "built $OUT"
