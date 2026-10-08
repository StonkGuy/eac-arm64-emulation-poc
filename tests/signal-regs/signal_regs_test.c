// SPDX-License-Identifier: MIT
//
// Regression test for registers inside a guest signal handler (the FEX bug described in docs/signal-registers.md).
// A freestanding static program that only uses raw system calls; the handler is written in assembly so that no compiler
// register allocation is involved.
//
// The handler runs in the same page as its own data (an "awx" section, so every linker puts them in one RWX segment).
// It sets r8 and r9, then stores to its data page (under FEX's default SMC mode that is a write fault into a page holding
// translated code, which makes FEX spill the registers and re-dispatch), then makes two system calls, and finally reports r8
// and r9. On Linux both registers still hold what the handler put in them.
//
// The signal is raised four ways, each twice (the first run translates the handler, the second one finds it translated):
//   tgkill and kill to self: the signal arrives while FEX is inside the C++ half of the syscall handler
//   tkill to self:           the same, but with a different argument pattern (kept as a control)
//   setitimer + spinning:    an asynchronous signal arriving in translated code (control)
//
// A second handler takes a SIGSEGV of its own (a load from address 0, skipped by a SIGSEGV handler) before its first system call:
// the nested signal is the other consumer of the stale state, with no SMC involved. Raised with tgkill/kill/setitimer too.
//
// Expected: every case passes on Linux. On unpatched FEX 2609.1 the tgkill/kill/tkill cases fail (r8 holds the interrupted code's value).
// Build: tests/signal-regs/build.sh [clang]   (x86-64 binary, run it under FEX)   or   build.sh native (host architecture)
// Run:   ./signal_regs_test   (exit status = number of failed cases)

typedef unsigned long u64;
typedef long i64;

#if defined(__x86_64__)
static inline i64 sc4(i64 nr, i64 a, i64 b, i64 c, i64 d) {
  i64 ret;
  register i64 r10 __asm__("r10") = d;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
  return ret;
}
enum { SYS_write = 1, SYS_rt_sigaction = 13, SYS_getpid = 39, SYS_setitimer = 38, SYS_kill = 62, SYS_tkill = 200, SYS_exit_group = 231,
       SYS_gettid = 186, SYS_tgkill = 234 };
struct ksigaction { void* handler; u64 flags; void* restorer; u64 mask; };
__attribute__((naked, used)) static void restore_rt(void) { __asm__ volatile("mov $15, %eax\n syscall\n"); }
#define MAKE_ACTION(h) { (void*)(h), SA_SIGINFO | SA_RESTORER, (void*)restore_rt, 0 }
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
enum { SYS_write = 64, SYS_rt_sigaction = 134, SYS_getpid = 172, SYS_setitimer = 103, SYS_kill = 129, SYS_tkill = 130, SYS_exit_group = 94,
       SYS_gettid = 178, SYS_tgkill = 131 };
struct ksigaction { void* handler; u64 flags; u64 mask; };
#define MAKE_ACTION(h) { (void*)(h), SA_SIGINFO, 0 }
#else
#error "x86-64 and aarch64 only"
#endif
#define SA_SIGINFO 0x4
#define SA_RESTORER 0x04000000
enum { SIGUSR1 = 10, SIGUSR2 = 12, SIGSEGV = 11, SIGALRM = 14 };

static u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sc4(SYS_write, 1, (i64)s, slen(s), 0); }
static void outhex(u64 v) {
  char b[19]; b[0] = '0'; b[1] = 'x';
  for (int i = 0; i < 16; i++) b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
  b[18] = 0; out(b);
}

#define MAGIC 0x5a5a1234a5a56789UL
extern volatile u64 seen_sig, seen_magic, hits;
extern void hreg(void);
extern void hreg2(void);
extern void hsegv(void);

// The handler and its data share one RWX section.
#if defined(__x86_64__)
__asm__(".section .rwx,\"awx\",@progbits\n.balign 16\n"
        ".globl hreg\nhreg:\n"
        "  mov %rdi, %r8\n"                          // r8 = signal number
        "  movabs $0x5a5a1234a5a56789, %r9\n"        // r9 = magic
        "  incl hits(%rip)\n"                        // store into the page that holds this code
        "  mov $39, %eax\n  syscall\n"               // getpid
        "  mov $186, %eax\n syscall\n"               // gettid
        "  mov %r8, seen_sig(%rip)\n"
        "  mov %r9, seen_magic(%rip)\n"
        "  ret\n"
        ".balign 16\n.globl hreg2\nhreg2:\n"
        "  mov %rdi, %r8\n"
        "  movabs $0x5a5a1234a5a56789, %r9\n"
        "  xor %eax, %eax\n"
        "  mov (%rax), %rax\n"                       // SIGSEGV; hsegv steps over these 3 bytes
        "  mov $39, %eax\n  syscall\n"
        "  mov $186, %eax\n syscall\n"
        "  mov %r8, seen_sig(%rip)\n"
        "  mov %r9, seen_magic(%rip)\n"
        "  ret\n"
        ".balign 16\n.globl hsegv\nhsegv:\n"
        "  addq $3, 168(%rdx)\n"                     // ucontext_t: uc_mcontext.gregs[REG_RIP] += 3
        "  ret\n"
        ".balign 16\n.globl seen_sig\nseen_sig: .quad 0\n.globl seen_magic\nseen_magic: .quad 0\n.globl hits\nhits: .quad 0\n"
        ".text\n");
#else
__asm__(".section .rwx,\"awx\",@progbits\n.balign 16\n"
        ".globl hreg\nhreg:\n"
        "  mov x9, x0\n"                             // x9 = signal number
        "  movz x10, #0x6789\n movk x10, #0xa5a5, lsl #16\n movk x10, #0x1234, lsl #32\n movk x10, #0x5a5a, lsl #48\n"
        "  adr x11, hits\n ldr x12, [x11]\n add x12, x12, #1\n str x12, [x11]\n"
        "  mov x8, #172\n svc 0\n"                   // getpid
        "  mov x8, #178\n svc 0\n"                   // gettid
        "  adr x11, seen_sig\n str x9, [x11]\n str x10, [x11, #8]\n"
        "  ret\n"
        ".balign 16\n.globl hreg2\nhreg2:\n"
        "  mov x9, x0\n"
        "  movz x10, #0x6789\n movk x10, #0xa5a5, lsl #16\n movk x10, #0x1234, lsl #32\n movk x10, #0x5a5a, lsl #48\n"
        "  mov x11, xzr\n"
        "  ldr x12, [x11]\n"                         // SIGSEGV; hsegv steps over this instruction
        "  mov x8, #172\n svc 0\n"
        "  mov x8, #178\n svc 0\n"
        "  adr x11, seen_sig\n str x9, [x11]\n str x10, [x11, #8]\n"
        "  ret\n"
        ".balign 16\n.globl hsegv\nhsegv:\n"
        "  ldr x9, [x2, #440]\n add x9, x9, #4\n str x9, [x2, #440]\n"   // ucontext_t: uc_mcontext.pc += 4
        "  ret\n"
        ".balign 16\n.globl seen_sig\nseen_sig: .quad 0\n.globl seen_magic\nseen_magic: .quad 0\n.globl hits\nhits: .quad 0\n"
        ".text\n");
#endif

static int failures;
static void verdict(const char* what, int sig) {
  int ok = seen_sig == (u64)sig && seen_magic == MAGIC;
  out(ok ? "PASS: " : "FAIL: "); out(what);
  if (!ok) { out("  (handler left sig="); outhex(seen_sig); out(" magic="); outhex(seen_magic); out(")"); ++failures; }
  out("\n");
  seen_sig = seen_magic = 0;
}
static void install_h(int sig, void* h) { struct ksigaction a = MAKE_ACTION(h); sc4(SYS_rt_sigaction, sig, (i64)&a, 0, 8); }
static void install(int sig) { install_h(sig, (void*)hreg); }

__attribute__((used)) static void entry_c(void) {
  install(SIGUSR1); install(SIGALRM);
  for (int pass = 1; pass <= 2; ++pass) {
    const int first = pass == 1;
    sc4(SYS_tgkill, sc4(SYS_getpid, 0, 0, 0, 0), sc4(SYS_gettid, 0, 0, 0, 0), SIGUSR1, 0);
    verdict(first ? "tgkill(self), first run of the handler" : "tgkill(self), second run", SIGUSR1);
    sc4(SYS_kill, sc4(SYS_getpid, 0, 0, 0, 0), SIGUSR1, 0, 0);
    verdict(first ? "kill(self), first run of the handler" : "kill(self), second run", SIGUSR1);
    sc4(SYS_tkill, sc4(SYS_gettid, 0, 0, 0, 0), SIGUSR1, 0, 0);
    verdict(first ? "tkill(self), first run of the handler" : "tkill(self), second run", SIGUSR1);
  }
  struct { i64 interval_s, interval_us, value_s, value_us; } timer = {0, 0, 0, 100000};
  sc4(SYS_setitimer, 0, (i64)&timer, 0, 0);
  for (volatile u64 i = 0; i < 2000000000UL && !seen_sig; ++i) { }
  verdict("setitimer, asynchronous signal", SIGALRM);

  // handler that takes a SIGSEGV of its own before its first system call
  install_h(SIGSEGV, (void*)hsegv); install_h(SIGUSR1, (void*)hreg2); install_h(SIGUSR2, (void*)hreg2); install_h(SIGALRM, (void*)hreg2);
  for (int pass = 1; pass <= 2; ++pass) {
    const int first = pass == 1;
    sc4(SYS_tgkill, sc4(SYS_getpid, 0, 0, 0, 0), sc4(SYS_gettid, 0, 0, 0, 0), SIGUSR1, 0);
    verdict(first ? "tgkill(self), handler takes a SIGSEGV, first run" : "tgkill(self), handler takes a SIGSEGV, second run", SIGUSR1);
    sc4(SYS_kill, sc4(SYS_getpid, 0, 0, 0, 0), SIGUSR2, 0, 0);
    verdict(first ? "kill(self), handler takes a SIGSEGV, first run" : "kill(self), handler takes a SIGSEGV, second run", SIGUSR2);
  }
  sc4(SYS_setitimer, 0, (i64)&timer, 0, 0);
  for (volatile u64 i = 0; i < 2000000000UL && !seen_sig; ++i) { }
  verdict("setitimer, handler takes a SIGSEGV", SIGALRM);
  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc4(SYS_exit_group, failures, 0, 0, 0);
}
#if defined(__x86_64__)
__attribute__((naked, used)) void _start(void) { __asm__ volatile("and $-16, %rsp\n call entry_c\n"); }
#else
__asm__(".text\n.globl _start\n_start:\n bl entry_c\n");
#endif
