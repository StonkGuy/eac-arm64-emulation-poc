// SPDX-License-Identifier: MIT
//
// Regression test for the two debugger-facing lines of /proc/self/status under FEX.
//
// On a real Linux kernel /proc/<pid>/status describes *that* process: Seccomp: is the process's own seccomp mode
// (0 disabled, 1 strict, 2 filter; fs/proc/array.c task_state) and TracerPid: is the pid of the process's own
// ptracer, 0 when it is not traced. FEX used to fall through to the host file, so Seccomp: read 0 even after the
// guest installed a filter while TracerPid: exposed the host process's tracer.
//
// This test installs a seccomp filter from inside the guest and checks that the /proc/self/status it reads back
// reports the emulated truth. All the other lines must still be the ordinary status lines (nothing is fabricated).
//
// The difference only shows with FEX_NEEDSSECCOMP=1, the setting Wine's seccomp path needs (FEX then emulates the
// guest's filters instead of installing them on the host process). The binary re-execs itself with that option so it
// tests the configuration that matters; on a real kernel the variable is ignored.
//
// Expected: passes on Linux (native reference) and under the patched FEX.
// Build: tests/proc-status/build.sh [clang]   (x86-64 binary, run it under FEX)   or   build.sh native (host arch)
// Run:   ./proc_status_test   (exit status = number of failed checks)

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
enum { SYS_read = 0, SYS_write = 1, SYS_open = 2, SYS_close = 3, SYS_getpid = 39, SYS_clone = 56, SYS_wait4 = 61, SYS_prctl = 157, SYS_exit = 60, SYS_execve = 59, SYS_seccomp = 317, SYS_exit_group = 231 };
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
enum { SYS_read = 63, SYS_write = 64, SYS_open = 1024, SYS_close = 57, SYS_getpid = 172, SYS_clone = 220, SYS_wait4 = 260, SYS_prctl = 167, SYS_exit = 93, SYS_execve = 221, SYS_seccomp = 277, SYS_exit_group = 94 };
#else
#error "x86-64 and aarch64 only"
#endif

#define sc1(n, a) sc6(n, (i64)(a), 0, 0, 0, 0, 0)
#define sc2(n, a, b) sc6(n, (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sc3(n, a, b, c) sc6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sc4(n, a, b, c, d) sc6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), 0, 0)
#define sc5(n, a, b, c, d, e) sc6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), (i64)(e), 0)

enum { PR_SET_NO_NEW_PRIVS = 38, PR_SET_SECCOMP = 22, SECCOMP_MODE_FILTER = 2, SECCOMP_SET_MODE_FILTER = 1 };
// clone(2): a real thread (CLONE_THREAD) shares the address space, so it can report back through a shared
// variable. All the mandatory resource-sharing flags must be present, and CLONE_THREAD requires exit_signal 0.
enum {
  CLONE_VM_ = 0x100, CLONE_FS_ = 0x200, CLONE_FILES_ = 0x400, CLONE_SIGHAND_ = 0x800,
  CLONE_THREAD_ = 0x10000, CLONE_SYSVSEM_ = 0x40000,
  CLONE_THREAD_FLAGS = CLONE_VM_ | CLONE_FS_ | CLONE_FILES_ | CLONE_SIGHAND_ | CLONE_THREAD_ | CLONE_SYSVSEM_
};
enum { SECCOMP_RET_ALLOW = 0x7fff0000 };
#define BPF_LD 0x00
#define BPF_RET 0x06
#define BPF_K 0x00
struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter* filter; };

static u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (i64)s, slen(s)); }
static void outdec(i64 v) {
  char b[24]; int n = 0, neg = v < 0; u64 u = neg ? (u64)(-v) : (u64)v;
  if (!u) b[n++] = '0'; while (u) { b[n++] = '0' + (u % 10); u /= 10; }
  if (neg) b[n++] = '-'; char r[24]; int k = 0; while (n) r[k++] = b[--n]; r[k] = 0; out(r);
}
static int seq(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
// 1 if the line at `line` (terminated by `end`) begins with `key`
static int starts(const char* line, const char* end, const char* key) {
  while (*key) { if (line >= end || *line != *key) return 0; ++line; ++key; }
  return 1;
}

static int failures;
static void check(int ok, const char* what) {
  out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) ++failures;
}

// Reads /proc/self/status and copies the value of the line that starts with `key` (the text after the colon, with
// leading blanks removed) into outval. Returns 0 on success, -1 if the file or the key was not found.
static int status_value(const char* key, char* outval, u64 outsz) {
  static char buf[8192];
  i64 fd = sc3(SYS_open, (i64)"/proc/self/status", 0, 0);
  if (fd < 0) return -1;
  i64 n = sc3(SYS_read, fd, (i64)buf, sizeof(buf) - 1);
  sc1(SYS_close, fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  u64 klen = slen(key);
  for (char* p = buf; *p;) {
    char* eol = p; while (*eol && *eol != '\n') ++eol;
    if ((u64)(eol - p) > klen && starts(p, eol, key)) {
      char* v = p + klen;
      while (*v == ' ' || *v == '\t') ++v;
      u64 i = 0; while (v + i < eol && i + 1 < outsz) { outval[i] = v[i]; ++i; }
      outval[i] = 0;
      return 0;
    }
    p = *eol ? eol + 1 : eol;
  }
  return -1;
}
static int status_has(const char* key) { char v[64]; return status_value(key, v, sizeof(v)) == 0 && v[0]; }
static i64 parse_dec(const char* s) { i64 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

// Installs a filter that allows everything: enough for the kernel (and FEX) to enter SECCOMP_MODE_FILTER.
static i64 install_filter(void) {
  struct sock_filter insn[1] = {{(unsigned short)(BPF_RET | BPF_K), 0, 0, SECCOMP_RET_ALLOW}};
  struct sock_fprog prog = {1, insn};
  if (sc5(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
  i64 r = sc3(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, (i64)&prog);
  if (r != 0) r = sc5(SYS_prctl, PR_SET_SECCOMP, SECCOMP_MODE_FILTER, (i64)&prog, 0, 0);
  return r;
}

// A thread created with clone(2) has its own /proc/<tid>/status, and its Seccomp: is the process's seccomp mode,
// exactly like the main thread's. The child runs on its own stack and hands the value back through a shared
// variable (CLONE_VM), so the parent can check what the thread saw without parsing /proc.
static volatile i64 thread_seccomp = -2;
static volatile int thread_done;
static void thread_body(void) {
  char v[64];
  thread_seccomp = (status_value("Seccomp:", v, sizeof(v)) == 0) ? parse_dec(v) : -1;
  __asm__ volatile("" ::: "memory");
  thread_done = 1;
  sc1(SYS_exit, 0);
  for (;;) {}
}
// Returns the thread's tid in the parent and 0 in the thread (which never returns: it exits from thread_body).
static i64 spawn_thread(void) {
  static char stk[65536] __attribute__((aligned(16)));
  i64 r = sc5(SYS_clone, CLONE_THREAD_FLAGS, (i64)(stk + sizeof stk), 0, 0, 0);
  if (r == 0) thread_body();
  return r;
}

static void ucopy(char* d, const char* s) { while ((*d++ = *s++)) {} }

static void run(void) {
  char v[64];

  // (1) With no filter installed the emulated seccomp mode is 0 (SECCOMP_MODE_DISABLED), like an untraced filterless process.
  check(status_value("Seccomp:", v, sizeof(v)) == 0, "/proc/self/status is readable and has a Seccomp: line");
  check(parse_dec(v) == 0, "Seccomp: is 0 before any filter is installed");

  // (2) The untraced process reports TracerPid: 0, exactly like an untraced process on a real kernel.
  check(status_value("TracerPid:", v, sizeof(v)) == 0, "TracerPid: is present");
  check(parse_dec(v) == 0, "TracerPid: is 0 for an untraced process");

  // (3) The rest of the file is the ordinary status file, not a fabricated one.
  check(status_has("State:"), "the ordinary State: line is still present (the file is passed through)");
  check(status_has("VmSize:") || status_has("VmRSS:"), "the ordinary memory lines are still present");
  if (status_value("Pid:", v, sizeof(v)) == 0) check(parse_dec(v) == sc1(SYS_getpid, 0), "Pid: is this process");

  // (4) After the guest installs a filter, Seccomp: must read 2 (SECCOMP_MODE_FILTER), not the stale host 0.
  i64 ir = install_filter();
  if (ir != 0) {
    out("SKIP: could not install a seccomp filter here (errno "); outdec(-ir); out(")\n");
  } else {
    check(status_value("Seccomp:", v, sizeof(v)) == 0, "Seccomp: is readable after installing a filter");
    check(parse_dec(v) == 2, "Seccomp: is 2 (SECCOMP_MODE_FILTER) after the guest installs a filter");

    // (5) A clone'd thread must report the same emulated mode as the main thread - the creator reads the calling
    // thread's state, so the registration of a spawned thread has to be right too.
    if (spawn_thread() > 0) {
      for (volatile long spin = 0; !thread_done && spin < 400000000L; ++spin) {}
    }
    if (thread_seccomp != 2) { out("  (the thread read Seccomp: "); outdec(thread_seccomp); out(")\n"); }
    check(thread_seccomp == 2, "a clone'd thread also reports Seccomp: 2 after the filter is installed");
  }

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc1(SYS_exit_group, failures);
}

// ------------------------------------------------------------------------------------------------------
// entry: re-exec with FEX_NEEDSSECCOMP=1, so the test is self-contained
// ------------------------------------------------------------------------------------------------------
static char envbuf[80][128];
static char* newenv[84];
__attribute__((used)) static void entry_c(u64* sp) {
  u64 argc = sp[0]; char** argv = (char**)(sp + 1); char** envp = argv + argc + 1;
  int have = 0, n = 0;
  for (char** e = envp; *e && n < 78; ++e) {
    if (seq(*e, "FEX_NEEDSSECCOMP=1")) have = 1;
    ucopy(envbuf[n], *e); newenv[n] = envbuf[n]; ++n;
  }
  if (!have) {
    ucopy(envbuf[n], "FEX_NEEDSSECCOMP=1"); newenv[n] = envbuf[n]; ++n;
    newenv[n] = 0;
    sc3(SYS_execve, (i64)"/proc/self/exe", (i64)argv, (i64)newenv);
    // execve returned: continue and let the checks report what this environment supports
  }
  run();
}
#if defined(__x86_64__)
__attribute__((naked, used)) void _start(void) { __asm__ volatile("mov %rsp, %rdi\n and $-16, %rsp\n call entry_c\n"); }
#else
__asm__(".text\n.globl _start\n_start:\n mov x0, sp\n and sp, x0, #-16\n bl entry_c\n");
#endif
