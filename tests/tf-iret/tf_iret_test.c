// SPDX-License-Identifier: MIT
//
// Regression test: where the single-step trap lands when TF is switched on by POPFQ or IRETQ.
//
// Wine returns from a system call through IRETQ when the caller had TF set (the frame's restore_flags carry
// CONTEXT_CONTROL), so "syscall with TF set" resumes with TF on at the instruction after the syscall. Anti-tamper code
// relies on that: it raises an exception, sets TF from its handler, single-steps over a raw syscall and checks where the
// EXCEPTION_SINGLE_STEP lands. Under FEX a TF loaded by IRETQ was not noticed until translated code next went through
// the dispatcher, so the trap landed several instructions (and a function return) later.
//
// Checks (the SIGTRAP handler records RIP and clears TF):
//   1. POPFQ sets TF: exactly one trap, at the expected instruction;
//   2. IRETQ loads TF (target in the same code, 2000 repetitions so the translation is cached and linked): every
//      repetition traps once, at the same instruction as the first;
//   3. the same through a far IRETQ target reached by a call/ret pair, 2000 repetitions;
//   4. Wine's CONTEXT_CONTROL syscall return, POPFQ (TF) immediately followed by IRETQ (TF): the trap lands on the
//      IRETQ target, before its first instruction runs;
//   5. single-stepping across a call and a return into already translated code: one trap per instruction.
// The expected RIP for each case is whatever the first repetition on a real kernel reports, printed as an offset from
// the case's label; the test asserts the offsets the native x86-64 run prints (see EXPECT_* below).
//
// Build: tests/tf-iret/build.sh [clang]   (x86-64 binary; run it under FEX and on a real x86-64 Linux for reference)
// Run:   ./tf_iret_test   (exit status = number of failed checks)

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
#define sc3(n, a, b, c) sc6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
enum { SYS_write = 1, SYS_rt_sigaction = 13, SYS_alarm = 37, SYS_exit_group = 231 };
enum { SIGTRAP = 5, SA_SIGINFO = 4, SA_RESTORER = 0x04000000, REG_EFL = 17, REG_RIP = 16 };

struct ksigaction { void* handler; u64 flags; void* restorer; u64 mask; };
__attribute__((naked, used)) static void restore_rt(void) { __asm__ volatile("mov $15, %eax\n syscall\n"); }

static volatile u64 trap_rip, trap_count;
static volatile u64 step_until;   // case 5: keep TF until RIP reaches this address
static void handler(int sig, void* info, void* uc) {
  u64* gregs = (u64*)((char*)uc + 40);
  (void)sig; (void)info;
  trap_rip = gregs[REG_RIP];
  ++trap_count;
  if (!step_until || gregs[REG_RIP] == step_until) gregs[REG_EFL] &= ~0x100UL;
}

// Case 1: POPFQ with TF. Returns after the nops.
extern char c1_label[];
void case_popf(void);
__asm__(".text\n.globl case_popf\ncase_popf:\n"
        " pushfq\n orq $0x100, (%rsp)\n popfq\n"
        ".globl c1_label\nc1_label:\n nop\n nop\n nop\n nop\n ret\n");

// Case 2: IRETQ to a label in the same function with TF set in the pushed RFLAGS.
extern char c2_label[];
void case_iret(void);
__asm__(".text\n.globl case_iret\ncase_iret:\n"
        " mov %ss, %eax\n push %rax\n"          // SS
        " lea 8(%rsp), %rax\n push %rax\n"       // RSP after the frame is popped
        " pushfq\n orq $0x100, (%rsp)\n"         // RFLAGS | TF
        " mov %cs, %eax\n push %rax\n"           // CS
        " lea c2_label(%rip), %rax\n push %rax\n" // RIP
        " iretq\n"
        ".globl c2_label\nc2_label:\n nop\n nop\n nop\n nop\n ret\n");

// Case 3: IRETQ to the instruction after a call, the target then returns through two frames (the shape of Wine's
// dispatcher returning into the caller of a raw syscall).
extern char c3_label[];
void case_iret_ret(void);
__asm__(".text\n.globl c3_inner\nc3_inner:\n"
        " mov %ss, %eax\n push %rax\n"
        " lea 8(%rsp), %rax\n push %rax\n"
        " pushfq\n orq $0x100, (%rsp)\n"
        " mov %cs, %eax\n push %rax\n"
        " lea c3_label(%rip), %rax\n push %rax\n"
        " iretq\n"
        ".globl c3_label\nc3_label:\n add $0, %rax\n ret\n"
        ".globl case_iret_ret\ncase_iret_ret:\n call c3_inner\n nop\n nop\n ret\n");

// Case 4: Wine's CONTEXT_CONTROL return (__wine_syscall_dispatcher_return): build the IRETQ frame, then
// "pushq flags; popfq; iretq" with TF in both. POPFQ arms TF; the trap is due after the next instruction, the IRETQ, so
// it lands on the IRETQ target before that instruction runs ("make sure that if trap flag is set the trap happens on the
// first instruction after iret").
extern char c4_label[];
void case_wine_ret(void);
__asm__(".text\n.globl c4_inner\nc4_inner:\n"
        " mov %ss, %eax\n push %rax\n"
        " lea 8(%rsp), %rax\n push %rax\n"
        " pushfq\n orq $0x100, (%rsp)\n"
        " mov %cs, %eax\n push %rax\n"
        " lea c4_label(%rip), %rax\n push %rax\n"
        " pushfq\n orq $0x100, (%rsp)\n popfq\n"
        " iretq\n"
        ".globl c4_label\nc4_label:\n add $0, %rax\n ret\n"
        ".globl case_wine_ret\ncase_wine_ret:\n call c4_inner\n nop\n nop\n ret\n");

// Case 5: single-step across a call and a return into code that has already run (so it is translated and linked):
// 10 instructions after the POPFQ (nop, call, nop, nop, ret, nop, call, nop, nop, ret), one trap each.
extern char c5_end[];
void case_step_ret(void);
void c5_leaf(void);
__asm__(".text\n.globl c5_leaf\nc5_leaf:\n nop\n nop\n ret\n"
        ".globl case_step_ret\ncase_step_ret:\n"
        " pushfq\n orq $0x100, (%rsp)\n popfq\n"
        " nop\n call c5_leaf\n nop\n call c5_leaf\n"
        ".globl c5_end\nc5_end:\n ret\n");

static int failures;
static void out(const char* s) { u64 n = 0; while (s[n]) ++n; sc3(SYS_write, 1, s, n); }
static void outdec(i64 v) { char b[22]; int i = 21; int neg = v < 0; u64 u = neg ? -v : v; b[i] = 0; do { b[--i] = '0' + u % 10; u /= 10; } while (u); if (neg) b[--i] = '-'; out(b + i); }
static void check(int ok, const char* what) { out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) ++failures; }

// Expected trap offsets from each label, from the native x86-64 reference run.
#define EXPECT_POPF 1   // after the first nop following POPFQ
#define EXPECT_IRET 1   // after the first instruction at the IRETQ target
#define EXPECT_IRET_RET 4 // after the first instruction (add $0,%rax is 4 bytes) at the IRETQ target
#define EXPECT_WINE_RET 0 // POPFQ;IRETQ: on the IRETQ target itself

static int run(void (*fn)(void), char* label, int reps, i64 expect, const char* name) {
  int ok = 1;
  i64 first = 0;
  for (int i = 0; i < reps; ++i) {
    trap_count = 0; trap_rip = 0;
    fn();
    i64 off = (i64)(trap_rip - (u64)label);
    if (i == 0) first = off;
    if (trap_count != 1 || off != expect) {
      if (ok) { out("  "); out(name); out(": repetition "); outdec(i); out(" traps "); outdec(trap_count); out(" times, offset "); outdec(off); out("\n"); }
      ok = 0;
    }
  }
  out("  "); out(name); out(": first trap at label+"); outdec(first); out("\n");
  return ok;
}

__attribute__((used)) void entry_c(void) {
  struct ksigaction sa = {(void*)handler, SA_SIGINFO | SA_RESTORER, (void*)restore_rt, 0};
  sc6(SYS_rt_sigaction, SIGTRAP, (i64)&sa, 0, 8, 0, 0);
  sc1(SYS_alarm, 60);
  check(run(case_popf, c1_label, 2000, EXPECT_POPF, "popfq"), "1. POPFQ sets TF: one trap after the next instruction, every time");
  check(run(case_iret, c2_label, 2000, EXPECT_IRET, "iretq"), "2. IRETQ loads TF: one trap after the first target instruction, every time");
  check(run(case_iret_ret, c3_label, 2000, EXPECT_IRET_RET, "iretq+ret"), "3. IRETQ loads TF before a return: the trap does not slip past the return");
  check(run(case_wine_ret, c4_label, 2000, EXPECT_WINE_RET, "popfq+iretq"), "4. POPFQ arms TF right before IRETQ (Wine's syscall return): the trap lands on the IRETQ target");
  {
    int ok = 1;
    for (int i = 0; i < 50; ++i) c5_leaf();
    for (int i = 0; i < 200; ++i) {
      trap_count = 0; step_until = (u64)c5_end;
      case_step_ret();
      step_until = 0;
      if (trap_count != 10) { if (ok) { out("  step+ret: repetition "); outdec(i); out(" traps "); outdec(trap_count); out(" times\n"); } ok = 0; }
    }
    check(ok, "5. single-stepping across call/ret into translated code traps once per instruction");
  }
  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc1(SYS_exit_group, failures);
}

__attribute__((naked, used)) void _start(void) { __asm__ volatile("and $-16, %rsp\n call entry_c\n"); }
