// SPDX-License-Identifier: MIT
//
// Regression test for restart_syscall's Linux behaviour under FEX.
//
// Linux only reaches restart_syscall when the kernel restarted a syscall that was interrupted by a signal and the
// syscall's restart_block was armed. When the call arrives with no pending restart - which is the only state this
// emulated process can be in, since FEX never arms one - the kernel runs do_no_restart_syscall(), which returns
// -EINTR (kernel/signal.c). It must not abort the process.
//
// FEX used to register restart_syscall as a SYSCALL_STUB, whose body is ERROR_AND_DIE_FMT: making the syscall at all
// killed the process. A guest that probes it (or a libc that calls it after a signal) died instead of seeing -EINTR.
// This test makes the raw syscall and checks the return value and that the process survives.
//
// Expected: passes on both a real Linux kernel and the patched FEX. On an unpatched FEX the process dies and the
// RESULT line never prints.
//
// Build: tests/restart-syscall/build.sh [clang]   (x86-64 binary, run it under FEX)   or   build.sh native (host arch)
// Run:   ./restart_syscall_test   (exit status = number of failed checks)

typedef unsigned long u64;
typedef long i64;

#if defined(__x86_64__)
static inline i64 sc6(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  i64 ret;
  register i64 r10 __asm__("r10") = d;
  register i64 r8 __asm__("r8") = e;
  register i64 r9 __asm__("r9") = f;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return ret;
}
enum { SYS_write = 1, SYS_getpid = 39, SYS_exit_group = 231, SYS_restart_syscall = 219 };
#elif defined(__aarch64__)
static inline i64 sc6(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  register i64 x8 __asm__("x8") = nr;
  register i64 x0 __asm__("x0") = a;
  register i64 x1 __asm__("x1") = b;
  register i64 x2 __asm__("x2") = c;
  register i64 x3 __asm__("x3") = d;
  register i64 x4 __asm__("x4") = e;
  register i64 x5 __asm__("x5") = f;
  __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
  return x0;
}
// aarch64 has no restart_syscall; the reference build only exists to check the -EINTR expectation is Linux's.
enum { SYS_write = 64, SYS_getpid = 172, SYS_exit_group = 94, SYS_restart_syscall = 128 /* __NR_restart_syscall */ };
#else
#error "x86-64 and aarch64 only"
#endif

#define sc1(n, a) sc6(n, (i64)(a), 0, 0, 0, 0, 0)
#define sc3(n, a, b, c) sc6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)

static u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sc3(SYS_write, 1, s, slen(s)); }
static void outdec(i64 v) {
  char b[24]; int n = 0; int neg = v < 0;
  u64 u = neg ? (u64)(-v) : (u64)v;
  if (!u) b[n++] = '0';
  while (u) { b[n++] = '0' + (u % 10); u /= 10; }
  if (neg) b[n++] = '-';
  char r[24]; int k = 0; while (n) r[k++] = b[--n]; r[k] = 0; out(r);
}

static int failures;
static void check(int ok, const char* what) {
  out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) ++failures;
}

__attribute__((used)) static void entry_c(void) {
  i64 pid_before = sc1(SYS_getpid, 0);
  check(pid_before > 0, "the process is alive before the call");

  // A real Linux kernel returns EINTR here: there is no pending restart_block in a freshly entered process. Under
  // FEX the same is true - FEX never arms a restart_block, so do_no_restart_syscall()'s answer is the faithful one.
  i64 r = sc1(SYS_restart_syscall, 0);
  check(r == -4, "restart_syscall returns -EINTR (EINTR == 4)");

  // The process must survive the call, which the old ERROR_AND_DIE stub did not allow.
  i64 pid_after = sc1(SYS_getpid, 0);
  check(pid_after == pid_before, "the process survives the call (getpid is unchanged)");

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc1(SYS_exit_group, failures);
}
#if defined(__x86_64__)
__attribute__((naked, used)) void _start(void) { __asm__ volatile("and $-16, %rsp\n call entry_c\n"); }
#else
__asm__(".text\n.globl _start\n_start:\n bl entry_c\n");
#endif
