// SPDX-License-Identifier: MIT
//
// Regression test: /proc/<pid>/maps of an x86-64 process lists only that process's own mappings.
//
// On a real x86-64 kernel every user mapping lies below TASK_SIZE_MAX = (1 << 47) - PAGE_SIZE
// (arch/x86/include/asm/page_64_types.h, 4-level paging; a process only gets addresses above it if it asks for them
// with an mmap hint). The only line above it is the [vsyscall] gate page at 0xffffffffff600000
// (fs/proc/task_mmu.c prints every VMA plus the gate VMA). Every file-backed ELF mapping of an x86-64 process is
// an x86-64 ELF.
//
// Under FEX the host file also lists FEX itself, the AArch64 host libraries (ld-linux-aarch64.so.1, libc.so.6,
// libstdc++...) and the host vdso, all above 2^47. A reader that resolves a library by name finds the host's AArch64
// libc.so.6 next to the guest's. The Easy Anti-Cheat launcher module served to some deployments reads /proc/<child>/maps
// of the child it traces, picks a library mapping, peeks its ELF and injects a call into it: under FEX it picked a
// mapping in the host range, the injected call faulted and the launcher reported "Unexpected error. (#1)" (patch 0033).
//
// Checks, for /proc/self/maps and for /proc/<child>/maps of a forked child (the tracer's view):
//   1. no line starts at or above TASK_SIZE_MAX, except [vsyscall];
//   2. every file-backed mapping whose file is an ELF is an x86-64 ELF (e_machine 62);
//   3. the line covering this program's own code is present (the filter passes the process's mappings through).
//
// Expected: passes on x86-64 Linux (derived from the kernel source above; the x86 reference box was offline when this
// test was written) and under the patched FEX; check 1 and 2 fail on stock FEX and on 0001-0026.
// Build: tests/proc-maps/build.sh [clang]   (x86-64 binary, run it under FEX)
// Run:   ./proc_maps_test   (exit status = number of failed checks)

typedef unsigned long u64;
typedef long i64;

static inline i64 sc6(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  i64 ret;
  register i64 r10 __asm__("r10") = d;
  register i64 r8 __asm__("r8") = e;
  register i64 r9 __asm__("r9") = f;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return ret;
}
#define sc1(n, a) sc6(n, (i64)(a), 0, 0, 0, 0, 0)
#define sc2(n, a, b) sc6(n, (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sc3(n, a, b, c) sc6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sc4(n, a, b, c, d) sc6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), 0, 0)
enum { SYS_read = 0, SYS_write = 1, SYS_open = 2, SYS_close = 3, SYS_pipe = 22, SYS_fork = 57, SYS_exit = 60, SYS_wait4 = 61,
       SYS_kill = 62, SYS_exit_group = 231 };
enum { SIGKILL = 9 };

static const u64 TaskSizeMax = (1UL << 47) - 4096;
static const u64 VsyscallBase = 0xffffffffff600000UL;

static u64 slen(const char* s) { u64 n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (i64)s, slen(s)); }
static void out_hex(u64 v) {
  char b[19]; b[0] = '0'; b[1] = 'x';
  for (int i = 0; i < 16; i++) { int d = (v >> (60 - 4 * i)) & 15; b[2 + i] = d < 10 ? '0' + d : 'a' + d - 10; }
  b[18] = 0; out(b);
}
static void out_dec(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); out(b + i); }

static int failures;
static void check(int ok, const char* what) { out(ok ? "  ok   " : "  FAIL "); out(what); out("\n"); if (!ok) failures++; }

static char maps[1 << 20];

static i64 read_file(const char* path, char* buf, u64 cap) {
  i64 fd = sc3(SYS_open, (i64)path, 0, 0);
  if (fd < 0) return fd;
  u64 len = 0;
  for (;;) {
    i64 n = sc3(SYS_read, fd, (i64)(buf + len), cap - 1 - len);
    if (n <= 0) break;
    len += n;
  }
  sc1(SYS_close, fd);
  buf[len] = 0;
  return len;
}

static u64 parse_hex(const char** p) {
  u64 v = 0;
  for (;;) {
    char c = **p;
    if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
    else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
    else break;
    (*p)++;
  }
  return v;
}

// Checks one maps text. Returns through the counters; 'self' enables check 3.
static void check_maps(const char* label, char* text, int self) {
  int high = 0, foreign_elf = 0, own_code = 0, lines = 0;
  u64 code = (u64)&check_maps;
  char* line = text;
  while (*line) {
    char* eol = line;
    while (*eol && *eol != '\n') eol++;
    char saved = *eol; *eol = 0;
    lines++;
    const char* p = line;
    u64 start = parse_hex(&p);
    p++;
    u64 end = parse_hex(&p);
    const char* path = 0;
    for (const char* q = p; *q; q++) if (*q == '/' || *q == '[') { path = q; break; }
    int vsyscall = start == VsyscallBase;
    if (start >= TaskSizeMax && !vsyscall) {
      if (high < 3) { out("    above TASK_SIZE_MAX: "); out(line); out("\n"); }
      high++;
    }
    if (path && *path == '/') {
      unsigned char h[20] = {0};
      i64 fd = sc3(SYS_open, (i64)path, 0, 0);
      if (fd >= 0) {
        i64 n = sc3(SYS_read, fd, (i64)h, sizeof h);
        sc1(SYS_close, fd);
        if (n == sizeof h && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
          unsigned machine = h[18] | (h[19] << 8);
          if (machine != 62) {
            if (foreign_elf < 3) { out("    non-x86-64 ELF (e_machine "); out_dec(machine); out("): "); out(path); out("\n"); }
            foreign_elf++;
          }
        }
      }
    }
    if (code >= start && code < end) own_code = 1;
    *eol = saved;
    line = saved ? eol + 1 : eol;
  }
  out(label); out(": "); out_dec(lines); out(" lines\n");
  check(lines > 0, "maps is readable and not empty");
  check(high == 0, "no mapping at or above TASK_SIZE_MAX (except [vsyscall])");
  check(foreign_elf == 0, "every file-backed ELF mapping is x86-64");
  if (self) check(own_code, "the mapping of this program's own code is listed");
}

static void path_of(char* buf, i64 pid) {
  const char* a = "/proc/";
  int i = 0;
  while (*a) buf[i++] = *a++;
  char d[24]; int j = 23; d[j] = 0; do { d[--j] = '0' + pid % 10; pid /= 10; } while (pid);
  for (const char* q = d + j; *q; q++) buf[i++] = *q;
  a = "/maps";
  while (*a) buf[i++] = *a++;
  buf[i] = 0;
}

__attribute__((used)) void entry_c(void) {
  out("proc_maps_test: /proc/<pid>/maps lists only the x86-64 process's own mappings\n");
  out("  TASK_SIZE_MAX = "); out_hex(TaskSizeMax); out("\n");

  // (a) the process's own view.
  check(read_file("/proc/self/maps", maps, sizeof maps) > 0, "open/read /proc/self/maps");
  check_maps("/proc/self/maps", maps, 1);

  // (b) another process's view, as a tracer reads it: a forked child that blocks on a pipe until it is killed.
  int fds[2];
  if (sc1(SYS_pipe, fds) != 0) { check(0, "pipe"); goto done; }
  i64 child = sc1(SYS_fork, 0);
  if (child == 0) {
    char c;
    sc1(SYS_close, fds[1]);
    sc3(SYS_read, fds[0], (i64)&c, 1);
    sc1(SYS_exit, 0);
  }
  sc1(SYS_close, fds[0]);
  check(child > 0, "fork");
  if (child > 0) {
    char path[64];
    path_of(path, child);
    check(read_file(path, maps, sizeof maps) > 0, "open/read /proc/<child>/maps");
    check_maps("/proc/<child>/maps", maps, 0);
    sc2(SYS_kill, child, SIGKILL);
    sc4(SYS_wait4, child, 0, 0, 0);
  }
done:
  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc1(SYS_exit_group, failures);
}

__attribute__((naked, used)) void _start(void) { __asm__ volatile("mov %rsp, %rdi\n and $-16, %rsp\n call entry_c\n"); }
