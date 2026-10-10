// SPDX-License-Identifier: MIT
//
// Regression test for CPUID faulting (arch_prctl ARCH_SET_CPUID / ARCH_GET_CPUID) under FEX.
//
// Linux on a CPU with X86_FEATURE_CPUID_FAULT lets a thread turn CPUID off: arch_prctl(ARCH_SET_CPUID, 0) returns 0,
// a later CPUID from that thread raises #GP, and a user-mode #GP is delivered as SIGSEGV with si_code SI_KERNEL
// (arch/x86/kernel/traps.c gp_user_force_sig_segv -> force_sig). ARCH_GET_CPUID returns the live state. On a CPU
// without the feature ARCH_SET_CPUID returns -ENODEV and CPUID always executes.
//
// FEX used to hardcode ARCH_GET_CPUID = 1 and ARCH_SET_CPUID = -ENODEV, so a checker that encapsulates CPUID to
// catch a fault could never see one. This test installs a SIGSEGV handler and checks the whole contract:
//   ARCH_GET_CPUID is 0 or 1; ARCH_SET_CPUID(0) -> 0; CPUID then faults as SIGSEGV/SI_KERNEL at the CPUID instruction;
//   ARCH_SET_CPUID(1) -> 0; CPUID executes again and returns a leaf.
//
// The FEX side is behind the EnableCPUIDFaulting option (off by default, so an ordinary guest is unaffected). The
// binary re-execs itself with FEX_ENABLECPUIDFAULTING=1 so it exercises the feature on its own; on a real kernel (or
// an unpatched FEX) ARCH_SET_CPUID still reports -ENODEV and the fault checks are reported as SKIP.
//
// The handler is written in assembly so no compiler register allocation is involved, like tests/signal-regs.
//
// Build: tests/cpuid-fault/build.sh [clang]   (x86-64 binary, run it under FEX)   or   build.sh native (host arch)
// Run:   ./cpuid_fault_test   (exit status = number of failed checks)

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
enum { SYS_write = 1, SYS_rt_sigaction = 13, SYS_clone = 56, SYS_fork = 57, SYS_exit = 60, SYS_wait4 = 61, SYS_execve = 59, SYS_exit_group = 231, SYS_arch_prctl = 158 };
struct ksigaction { void* handler; u64 flags; void* restorer; u64 mask; };
__attribute__((naked, used)) static void restore_rt(void) { __asm__ volatile("mov $15, %eax\n syscall\n"); }
#define MAKE_ACTION(h) { (void*)(h), SA_SIGINFO | SA_RESTORER, (void*)restore_rt, 0 }
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
enum { SYS_write = 64, SYS_rt_sigaction = 134, SYS_execve = 221, SYS_exit_group = 94, SYS_arch_prctl = 244 };
struct ksigaction { void* handler; u64 flags; u64 mask; };
#define MAKE_ACTION(h) { (void*)(h), SA_SIGINFO, 0 }
#else
#error "x86-64 and aarch64 only"
#endif

#define sc1(n, a) sc6(n, (i64)(a), 0, 0, 0, 0, 0)
#define sc2(n, a, b) sc6(n, (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sc3(n, a, b, c) sc6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sc4(n, a, b, c, d) sc6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), 0, 0)
#define sc5(n, a, b, c, d, e) sc6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), (i64)(e), 0)

enum { SIGSEGV_ = 11 };
#define SA_SIGINFO 0x4
#define SA_RESTORER 0x04000000
enum { ARCH_SET_CPUID = 0x1012, ARCH_GET_CPUID = 0x1011, SI_KERNEL_ = 0x80 };
// clone(2): a real thread (CLONE_THREAD) with the mandatory resource-sharing flags, and a plain fork.
enum {
  CLONE_VM_ = 0x100, CLONE_FS_ = 0x200, CLONE_FILES_ = 0x400, CLONE_SIGHAND_ = 0x800,
  CLONE_THREAD_ = 0x10000, CLONE_SYSVSEM_ = 0x40000,
  CLONE_THREAD_FLAGS = CLONE_VM_ | CLONE_FS_ | CLONE_FILES_ | CLONE_SIGHAND_ | CLONE_THREAD_ | CLONE_SYSVSEM_ /* exit_signal = 0 */
};

static u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (i64)s, slen(s)); }
static void outdec(i64 v) {
  char b[24]; int n = 0, neg = v < 0; u64 u = neg ? (u64)(-v) : (u64)v;
  if (!u) b[n++] = '0'; while (u) { b[n++] = '0' + (u % 10); u /= 10; }
  if (neg) b[n++] = '-'; char r[24]; int k = 0; while (n) r[k++] = b[--n]; r[k] = 0; out(r);
}
static int seq(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static void ucopy(char* d, const char* s) { while ((*d++ = *s++)) {} }

static int failures;
static void check(int ok, const char* what) {
  out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) ++failures;
}

// ------------------------------------------------------------------------------------------------------
// the CPUID instruction at a known address, and the SIGSEGV handler (assembly, sharing an RWX section)
// ------------------------------------------------------------------------------------------------------
extern u64 cpuid_run(u64 leaf, u64 sub);
extern char cpuid_at[], cpuid_after[];
extern void hsegv(void);
extern volatile u64 si_code_seen, fault_rip, fault_count;

#if defined(__x86_64__)
__asm__(
  ".text\n.balign 16\n"
  ".globl cpuid_run\n"
  "cpuid_run:\n"
  "  mov %rdi, %rax\n"
  "  mov %rsi, %rcx\n"
  ".globl cpuid_at\n"
  "cpuid_at:\n"
  "  cpuid\n"                        // 0f a2 = 2 bytes
  ".globl cpuid_after\n"
  "cpuid_after:\n"
  "  mov %eax, %eax\n"
  "  ret\n"
  // sigaction handler: rsi = siginfo, rdx = ucontext. siginfo_t layout is si_signo @0, si_errno @4, si_code @8;
  // ucontext_t.uc_mcontext.gregs[REG_RIP] is at ucontext+168.
  ".section .rwx,\"awx\",@progbits\n.balign 16\n"
  ".globl hsegv\nhsegv:\n"
  "  movl 8(%rsi), %eax\n"           // si_code
  "  mov %rax, si_code_seen(%rip)\n"
  "  movq 168(%rdx), %rax\n"         // uc_mcontext.gregs[REG_RIP]
  "  mov %rax, fault_rip(%rip)\n"
  "  addq $2, 168(%rdx)\n"           // step over the 2-byte CPUID
  "  incl fault_count(%rip)\n"
  "  ret\n"
  ".balign 16\n"
  ".globl si_code_seen\nsi_code_seen: .quad 0\n"
  ".globl fault_rip\nfault_rip: .quad 0\n"
  ".globl fault_count\nfault_count: .quad 0\n"
  ".text\n");
#else
// aarch64: there is no CPUID and no arch_prctl; the reference build only reports the test as skipped.
__asm__(".text\n.globl cpuid_run\ncpuid_run:\n mov x0, xzr\n ret\n.globl cpuid_at\ncpuid_at:\n nop\n.globl cpuid_after\ncpuid_after:\n ret\n"
        ".section .rwx,\"awx\",@progbits\n.balign 16\n.globl hsegv\nhsegv:\n ret\n"
        ".balign 16\n.globl si_code_seen\nsi_code_seen: .quad 0\n.globl fault_rip\nfault_rip: .quad 0\n.globl fault_count\nfault_count: .quad 0\n.text\n");
#endif

// A CLONE_THREAD thread shares the address space, so it reports what it saw through these variables. Its first
// CPUID must fault: the parent did ARCH_SET_CPUID(0), and Linux inherits that across clone.
static volatile int ct_done, ct_faulted;
static char ct_stack[65536] __attribute__((aligned(16)));
static void ct_child(void) {
  __asm__ volatile("" ::: "memory");
  fault_count = 0;
  (void)cpuid_run(0, 0);
  ct_faulted = (fault_count != 0) ? 1 : 0;
  __asm__ volatile("" ::: "memory");
  ct_done = 1;
  sc1(SYS_exit, 0);
  for (;;) {}
}

static void run(void) {
#if defined(__aarch64__)
  out("SKIP: CPUID and arch_prctl do not exist on aarch64\n");
#else
  struct ksigaction a = MAKE_ACTION(hsegv);
  check(sc6(SYS_rt_sigaction, SIGSEGV_, (i64)&a, 0, 8, 0, 0) == 0, "install the SIGSEGV handler (SA_SIGINFO)");

  i64 live = sc2(SYS_arch_prctl, ARCH_GET_CPUID, 0);
  check(live == 0 || live == 1, "ARCH_GET_CPUID returns the live CPUID state (0 or 1)");
  if (live != 0 && live != 1) { out("  (ARCH_GET_CPUID returned "); outdec(live); out(")\n"); }

  // Turn CPUID faulting on for this thread: a CPU with the feature answers 0; without it, -ENODEV.
  i64 set0 = sc2(SYS_arch_prctl, ARCH_SET_CPUID, 0);
  if (set0 == -19 /* ENODEV */) {
    out("SKIP: ARCH_SET_CPUID reports -ENODEV here; CPUID faulting cannot be exercised\n");
  } else {
    check(set0 == 0, "ARCH_SET_CPUID(0) returns 0");
    check(sc2(SYS_arch_prctl, ARCH_GET_CPUID, 0) == 0, "ARCH_GET_CPUID reports CPUID disabled after ARCH_SET_CPUID(0)");

    __asm__ volatile("" ::: "memory");
    fault_count = 0;
    (void)cpuid_run(0, 0);
    check(fault_count == 1, "CPUID raises a fault while CPUID faulting is on");
    check(si_code_seen == SI_KERNEL_, "the CPUID fault is SIGSEGV with si_code SI_KERNEL");
    check(fault_rip == (u64)&cpuid_at[0], "the fault RIP is the CPUID instruction itself");

    // Linux inherits CPUID faulting (TIF_NOCPUID) across fork and clone, so a child that never called
    // ARCH_SET_CPUID must fault too. The fork child reports through its exit status; the CLONE_THREAD thread
    // shares the address space and reports through a shared variable.
    {
      i64 fpid = sc1(SYS_fork, 0);
      if (fpid == 0) {
        __asm__ volatile("" ::: "memory");
        fault_count = 0;
        (void)cpuid_run(0, 0);
        sc1(SYS_exit, fault_count);
        for (;;) {}
      }
      i64 status = 0;
      if (fpid > 0) sc4(SYS_wait4, fpid, (i64)&status, 0, 0);
      check(fpid > 0 && ((status >> 8) & 0xff) == 1, "a forked child inherits CPUID faulting from its parent");
    }
    {
      i64 pid = sc5(SYS_clone, CLONE_THREAD_FLAGS, (i64)(ct_stack + sizeof ct_stack), 0, 0, 0);
      if (pid == 0) { ct_child(); }
      check(pid > 0, "spawn a CLONE_THREAD thread");
      for (volatile long spin = 0; !ct_done && spin < 400000000L; ++spin) {}
      check(ct_done, "the CLONE_THREAD thread ran to completion");
      check(ct_faulted == 1, "a CLONE_THREAD thread inherits CPUID faulting from its parent");
    }

    // Re-enabling CPUID must make it execute transparently again.
    check(sc2(SYS_arch_prctl, ARCH_SET_CPUID, 1) == 0, "ARCH_SET_CPUID(1) returns 0");
    check(sc2(SYS_arch_prctl, ARCH_GET_CPUID, 0) == 1, "ARCH_GET_CPUID reports CPUID enabled after ARCH_SET_CPUID(1)");
    __asm__ volatile("" ::: "memory");
    fault_count = 0;
    u64 leaf0 = cpuid_run(0, 0);
    check(fault_count == 0, "CPUID executes without a fault once CPUID faulting is off again");
    check(leaf0 > 0, "the CPUID result is returned (max standard leaf > 0)");
  }
#endif

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc1(SYS_exit_group, failures);
}

// ------------------------------------------------------------------------------------------------------
// entry: re-exec with the FEX option that enables CPUID faulting, so the test is self-contained
// ------------------------------------------------------------------------------------------------------
static char envbuf[80][128];
static char* newenv[84];
__attribute__((used)) static void entry_c(u64* sp) {
  u64 argc = sp[0]; char** argv = (char**)(sp + 1); char** envp = argv + argc + 1;
  int have = 0, n = 0;
  for (char** e = envp; *e && n < 78; ++e) {
    if (seq(*e, "FEX_ENABLECPUIDFAULTING=1")) have = 1;
    ucopy(envbuf[n], *e); newenv[n] = envbuf[n]; ++n;
  }
  if (!have) {
    ucopy(envbuf[n], "FEX_ENABLECPUIDFAULTING=1"); newenv[n] = envbuf[n]; ++n;
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
