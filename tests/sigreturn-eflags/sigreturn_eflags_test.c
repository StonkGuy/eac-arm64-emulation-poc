// SPDX-License-Identifier: MIT
//
// Regression test for EFLAGS written by a signal handler when the handler leaves RIP alone.
//
// On Linux rt_sigreturn restores the whole register set from the frame (arch/x86/kernel/signal.c restore_sigcontext),
// so a handler that edits only uc_mcontext.gregs[REG_EFL] gets exactly those flags back when the interrupted code
// resumes. That is how a debugger, or Wine's SetThreadContext on a thread it has suspended with a signal, starts
// single-stepping: set TF in the context, leave RIP where it is, return. FEX restored the flags only when the handler
// had also changed RIP, so such an edit was dropped.
//
// The signal is sent to self with tgkill, so the frame's RIP is exactly the interrupted RIP (the instruction after the
// syscall). The handler then:
//   1. sets DF (bit 10): the code after the syscall must read DF = 1;
//   2. sets TF (bit 8): the instruction after the syscall must raise exactly one single-step SIGTRAP (whose handler
//      clears TF again).
//
// The binary is x86-64 only (it checks x86 flags). Run it under FEX, and run the same binary on a real x86-64 Linux
// kernel for the reference result.
// Build: tests/sigreturn-eflags/build.sh [clang]
// Run:   ./sigreturn_eflags_test   (exit status = number of failed checks)

#if !defined(__x86_64__)
#error "x86-64 only: the test checks x86 EFLAGS"
#endif

typedef unsigned long u64;
typedef long i64;

static inline i64 sc4(i64 nr, i64 a, i64 b, i64 c, i64 d) {
  i64 ret;
  register i64 r10 __asm__("r10") = d;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
  return ret;
}
enum { SYS_write = 1, SYS_rt_sigaction = 13, SYS_getpid = 39, SYS_gettid = 186, SYS_tgkill = 234, SYS_exit_group = 231 };
enum { SIGTRAP = 5, SIGUSR1 = 10 };
#define SA_SIGINFO 0x4
#define SA_RESTORER 0x04000000
#define EFL_TF 0x100UL
#define EFL_DF 0x400UL
struct ksigaction { void* handler; u64 flags; void* restorer; u64 mask; };

__attribute__((naked, used)) static void restore_rt(void) { __asm__ volatile("mov $15, %eax\n syscall\n"); }

// Handlers in assembly: rdx = ucontext_t*, uc_mcontext.gregs[REG_EFL] is at offset 176.
//   husr1: ORs g_set_flags into the frame's EFLAGS and leaves RIP untouched.
//   htrap: counts the trap and clears TF in the frame so stepping stops after one instruction.
extern volatile u64 g_set_flags, g_traps, g_flags_after;
extern void husr1(void);
extern void htrap(void);
__asm__(".data\n"
        ".globl g_set_flags\ng_set_flags: .quad 0\n"
        ".globl g_traps\ng_traps: .quad 0\n"
        ".globl g_flags_after\ng_flags_after: .quad 0\n"
        ".text\n"
        ".globl husr1\nhusr1:\n"
        "  movq g_set_flags(%rip), %rax\n"
        "  orq %rax, 176(%rdx)\n"
        "  ret\n"
        ".globl htrap\nhtrap:\n"
        "  incq g_traps(%rip)\n"
        "  andq $~0x100, 176(%rdx)\n"
        "  ret\n");

static u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sc4(SYS_write, 1, (i64)s, slen(s), 0); }
static void outdec(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); out(b + i); }

static int failures;
static void check(int ok, const char* what) { out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) failures++; }

// tgkill(SIGUSR1) to self, then record EFLAGS right after the syscall (clearing DF again before any C code runs).
static void raise_and_read_flags(i64 pid, i64 tid) {
  __asm__ volatile("mov %[nr], %%eax\n"
                   "syscall\n"
                   "pushfq\n"
                   "popq %%rcx\n"
                   "cld\n"
                   "movq %%rcx, g_flags_after(%%rip)\n"
                   :
                   : [nr] "i"(SYS_tgkill), "D"(pid), "S"(tid), "d"((i64)SIGUSR1)
                   : "rax", "rcx", "r11", "memory", "cc");
}

// tgkill(SIGUSR1) to self, then two plain instructions for a single-step to land on.
static void raise_then_step(i64 pid, i64 tid) {
  __asm__ volatile("mov %[nr], %%eax\n"
                   "syscall\n"
                   "nop\n"
                   "nop\n"
                   :
                   : [nr] "i"(SYS_tgkill), "D"(pid), "S"(tid), "d"((i64)SIGUSR1)
                   : "rax", "rcx", "r11", "memory", "cc");
}

__attribute__((used)) static void entry_c(void) {
  struct ksigaction usr1 = { (void*)husr1, SA_SIGINFO | SA_RESTORER, (void*)restore_rt, 0 };
  struct ksigaction trap = { (void*)htrap, SA_SIGINFO | SA_RESTORER, (void*)restore_rt, 0 };
  check(sc4(SYS_rt_sigaction, SIGUSR1, (i64)&usr1, 0, 8) == 0, "install the SIGUSR1 handler");
  check(sc4(SYS_rt_sigaction, SIGTRAP, (i64)&trap, 0, 8) == 0, "install the SIGTRAP handler");
  i64 pid = sc4(SYS_getpid, 0, 0, 0, 0), tid = sc4(SYS_gettid, 0, 0, 0, 0);

  // (1) Control: the handler changes nothing, so DF stays clear.
  g_set_flags = 0;
  raise_and_read_flags(pid, tid);
  check((g_flags_after & EFL_DF) == 0, "control: DF is clear after a handler that changes nothing");

  // (2) The handler sets DF in the frame and leaves RIP alone: rt_sigreturn must install it.
  g_set_flags = EFL_DF;
  raise_and_read_flags(pid, tid);
  check((g_flags_after & EFL_DF) != 0, "a handler that sets DF (RIP unchanged) resumes with DF set");

  // (3) The handler sets TF and leaves RIP alone: the next instruction must single-step exactly once.
  g_traps = 0;
  g_set_flags = EFL_TF;
  raise_then_step(pid, tid);
  g_set_flags = 0;
  if (g_traps != 1) { out("  (single-step traps: "); outdec(g_traps); out(")\n"); }
  check(g_traps == 1, "a handler that sets TF (RIP unchanged) starts single-stepping: exactly one SIGTRAP");

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc4(SYS_exit_group, failures, 0, 0, 0);
}
__attribute__((naked, used)) void _start(void) { __asm__ volatile("and $-16, %rsp\n call entry_c\n"); }
