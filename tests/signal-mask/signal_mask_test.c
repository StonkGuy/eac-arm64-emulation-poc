// SPDX-License-Identifier: MIT
//
// Regression test for the signal mask a guest signal handler runs with (the FEX deviation behind the Wine/VRChat wedge,
// see docs/disconnects.md). A freestanding static x86-64 program that only uses raw system calls.
//
// What Linux guarantees, and what each check verifies:
//   1. while a handler runs, the signal it handles is blocked          (unless SA_NODEFER)
//   2. the mask the interrupted code had stays blocked in the handler  (interrupted mask | sa_mask | signal)
//   3. the handler's own "save old mask, block a set, restore the saved mask" does not unblock the signal it runs for
//      (this is what Wine's server_enter/leave_uninterrupted_section does inside its SIGUSR1 handler)
//   4. so a second SIGUSR1 sent from inside the handler does not nest: it stays pending and runs after the handler returns
//   5. rt_sigreturn restores exactly the mask the interrupted code had
//   6. SA_NODEFER really lets the signal nest
//   7. a handler that writes SIGKILL/SIGSTOP into the frame's uc_sigmask gets them dropped by rt_sigreturn (the kernel
//      removes them in set_current_blocked), while the other bits it wrote are honoured (x86-64 frame layout only)
//
// The same source builds for x86-64 (the binary to run under FEX) and for aarch64 (the reference run on the host's own Linux
// kernel): only the system-call glue differs. Expected: passes on a Linux kernel. On FEX with FEX_SIGNALMASKFIX=0 (the stock
// behaviour) checks 1, 3 and 4b fail. With the fix (the default since patches/0007) it passes, provided FEX also has patches/0008: without that, a handler that keeps its
// `sig` argument in a register across system calls can lose it (docs/signal-registers.md) and checks 4a, 4b and 6 fail too.
// Build: tests/signal-mask/build.sh [clang]   (x86-64 binary)   or   tests/signal-mask/build.sh native   (host architecture)
// Run:   ./signal_mask_test   (exit status = number of failed checks)

typedef unsigned long u64;
typedef long i64;

#if defined(__x86_64__)
static inline i64 sc4(i64 nr, i64 a, i64 b, i64 c, i64 d) {
  i64 ret;
  register i64 r10 __asm__("r10") = d;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
  return ret;
}
enum { SYS_write = 1, SYS_rt_sigaction = 13, SYS_rt_sigprocmask = 14, SYS_getpid = 39, SYS_exit_group = 231, SYS_gettid = 186, SYS_tgkill = 234 };
#elif defined(__aarch64__)
static inline i64 sc4(i64 nr, i64 a, i64 b, i64 c, i64 d) {
  register i64 x8 __asm__("x8") = nr;
  register i64 x0 __asm__("x0") = a;
  register i64 x1 __asm__("x1") = b;
  register i64 x2 __asm__("x2") = c;
  register i64 x3 __asm__("x3") = d;
  __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
  return x0;
}
enum { SYS_write = 64, SYS_rt_sigaction = 134, SYS_rt_sigprocmask = 135, SYS_getpid = 172, SYS_exit_group = 94, SYS_gettid = 178, SYS_tgkill = 131 };
#else
#error "x86-64 and aarch64 only"
#endif
enum { SIGKILL = 9, SIGSTOP = 19, SIGUSR1 = 10, SIGUSR2 = 12, SIGALRM = 14 };
enum { SIG_BLOCK = 0, SIG_UNBLOCK = 1, SIG_SETMASK = 2 };
#define SA_SIGINFO 0x4
#define SA_NODEFER 0x40000000
#define SA_RESTORER 0x04000000
#define BIT(s) (1UL << ((s) - 1))

#if defined(__x86_64__)
struct ksigaction { void* handler; u64 flags; void* restorer; u64 mask; };
#else   // the generic kernel sigaction has no restorer: the kernel returns through the vDSO trampoline
struct ksigaction { void* handler; u64 flags; u64 mask; };
#endif

static u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sc4(SYS_write, 1, (i64)s, slen(s), 0); }
static int failures;
static void check(int ok, const char* what) { out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) ++failures; }

#if defined(__x86_64__)
__attribute__((naked, used)) static void restore_rt(void) { __asm__ volatile("mov $15, %eax\n syscall\n"); }
#endif

static u64 getmask(void) { u64 m = 0; sc4(SYS_rt_sigprocmask, SIG_BLOCK, 0, (i64)&m, 8); return m; }
static void setmask(int how, u64 set) { sc4(SYS_rt_sigprocmask, how, (i64)&set, 0, 8); }
static void raise_self(int sig) { sc4(SYS_tgkill, sc4(SYS_getpid, 0, 0, 0, 0), sc4(SYS_gettid, 0, 0, 0, 0), sig, 0); }
static void install(int sig, void* h, u64 flags, u64 sa_mask) {
#if defined(__x86_64__)
  struct ksigaction a = {h, flags | SA_SIGINFO | SA_RESTORER, (void*)restore_rt, sa_mask};
#else
  struct ksigaction a = {h, flags | SA_SIGINFO, sa_mask};
#endif
  sc4(SYS_rt_sigaction, sig, (i64)&a, 0, 8);
}

static volatile u64 mask_in_handler, mask_after_roundtrip;
static volatile int depth, max_depth, invocations, resend;

// Handler used by checks 1, 2 and 5: just look at the mask.
static void handler_probe(int sig, void* info, void* uc) {
  (void)sig; (void)info; (void)uc;
  mask_in_handler = getmask();
}

// Handler for checks 3 and 4: Wine-style "block a set around a call, then restore what was saved", then (once) re-send the signal.
static void handler_wine(int sig, void* info, void* uc) {
  (void)info; (void)uc;
  ++invocations; ++depth; if (depth > max_depth) max_depth = depth;
  u64 saved = getmask();
  setmask(SIG_BLOCK, BIT(SIGUSR1) | BIT(SIGUSR2) | BIT(SIGALRM));   // enter the "uninterrupted section"
  setmask(SIG_SETMASK, saved);                                      // leave it: put back what we found
  mask_after_roundtrip = getmask();
  if (resend) { resend = 0; raise_self(sig); }                      // a second SIGUSR1 while we are still inside
  --depth;
}

#if defined(__x86_64__)
// Handler for check 7: rewrite the frame's uc_sigmask (offset 296 in the x86-64 ucontext) to include SIGKILL and SIGSTOP.
static void handler_killmask(int sig, void* info, void* uc) {
  (void)sig; (void)info;
  *(volatile u64*)((char*)uc + 296) |= BIT(SIGKILL) | BIT(SIGSTOP) | BIT(SIGUSR2);
}
#endif

__attribute__((used)) static void entry_c(void) {
  // 1 + 2: mask inside a handler
  install(SIGUSR1, (void*)handler_probe, 0, 0);
  setmask(SIG_BLOCK, BIT(SIGUSR2));
  u64 before = getmask();
  mask_in_handler = 0;
  raise_self(SIGUSR1);
  check(mask_in_handler != 0, "handler ran");
  check((mask_in_handler & BIT(SIGUSR1)) != 0, "1. the signal being handled is blocked inside its handler");
  check((mask_in_handler & BIT(SIGUSR2)) != 0, "2. a signal the interrupted code had blocked stays blocked inside the handler");
  check(getmask() == before, "5. rt_sigreturn restores exactly the interrupted mask");
  setmask(SIG_SETMASK, 0);

  // 3 + 4: Wine's pattern, and no nesting
  install(SIGUSR1, (void*)handler_wine, 0, 0);
  depth = max_depth = invocations = 0; resend = 1; mask_after_roundtrip = 0;
  raise_self(SIGUSR1);
  check(invocations == 2, "4a. the signal sent from inside the handler ran afterwards (2 invocations)");
  check(max_depth == 1, "4b. the second SIGUSR1 did not nest inside the first handler");
  check((mask_after_roundtrip & BIT(SIGUSR1)) != 0, "3. block-then-restore inside the handler keeps the handled signal blocked");
  check(getmask() == 0, "5b. mask is back to 0 after the handlers");

  // 6: SA_NODEFER nests
  install(SIGUSR1, (void*)handler_wine, SA_NODEFER, 0);
  depth = max_depth = invocations = 0; resend = 1;
  raise_self(SIGUSR1);
  check(invocations == 2 && max_depth == 2, "6. SA_NODEFER lets the signal nest");

#if defined(__x86_64__)
  // 7: SIGKILL and SIGSTOP can never be blocked
  install(SIGUSR1, (void*)handler_killmask, 0, 0);
  setmask(SIG_SETMASK, 0);
  raise_self(SIGUSR1);
  u64 after = getmask();
  check((after & BIT(SIGUSR2)) != 0, "7a. a bit the handler added to uc_sigmask is honoured by rt_sigreturn");
  check((after & (BIT(SIGKILL) | BIT(SIGSTOP))) == 0, "7b. rt_sigreturn never blocks SIGKILL or SIGSTOP");
  setmask(SIG_SETMASK, 0);
#endif

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc4(SYS_exit_group, failures, 0, 0, 0);
}
#if defined(__x86_64__)
__attribute__((naked, used)) void _start(void) {
  __asm__ volatile("and $-16, %rsp\n call entry_c\n");
}
#else   // file-scope asm: gcc does not support the naked attribute on aarch64
__asm__(".text\n.globl _start\n_start:\n bl entry_c\n");
#endif
