// SPDX-License-Identifier: MIT
//
// Regression test for the fault-time stack pointer of `pop r/m` (opcode 0x8F) under FEX.
//
// x86 contract: when an instruction faults, the architectural state is that of the instruction *before* it, so the stack
// pointer must be unchanged. FEX used to translate `pop r/m` into an IR `Pop`, which the AArch64 JIT emits as a single
// post-indexed load (`ldr<POST>`): the RSP increment was committed in the same operation as the load, before the
// (faultable) store to the destination. A `popq (%rax)` whose destination is unmapped therefore left RSP already
// incremented in the signal frame Wine's SEH handler sees, which breaks the handler's `context->Rip += 2` resume (this
// is the Wine ntdll:exception `segfault_code` sequence).
//
// Two checks, both measured from a SIGSEGV handler that records the fault-time RSP and then exits:
//   1. absolute  -- the RSP at the fault equals the RSP just before the faulting instruction (`popq (%rax)`)
//   2. differential -- `popq (%rax)` and `mov %eax,(%rax)` fault at the *same* RSP. Real x86 does; FEX must too.
// A control run of `mov %eax,(%rax)` guards against the test itself being the thing that is broken.
//
// The code pages are hand-written so nothing depends on how a compiler arranges its own stack frame:
//   48 89 e0        mov %rsp,%rax
//   48 89 03        mov %rax,(%rbx)     ; rbx -> shared[0]; records the pre-fault RSP
//   31 c0           xor %eax,%eax       ; rax = 0, so the store/pop below targets address 0 (unmapped)
//   8f 00 / 89 00   popq (%rax)  /  mov %eax,(%rax)
//   c3              ret
//
// Build: tests/pop-fault/build.sh [clang]   (x86-64 binary, run it under FEX)   or   build.sh native (host architecture)
// Run:   ./pop_fault_test   (exit status = number of failed checks; prints RESULT: PASS/FAIL)

typedef unsigned long u64;
typedef long i64;

// ------------------------------------------------------------------------------------------------------
// raw syscalls
// ------------------------------------------------------------------------------------------------------
static inline i64 sc1(i64 nr, i64 a) {
  i64 ret;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a) : "rcx", "r11", "memory");
  return ret;
}
static inline i64 sc2(i64 nr, i64 a, i64 b) {
  i64 ret;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b) : "rcx", "r11", "memory");
  return ret;
}
static inline i64 sc4(i64 nr, i64 a, i64 b, i64 c, i64 d) {
  i64 ret;
  register i64 r10 __asm__("r10") = d;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
  return ret;
}
static inline i64 sc6(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  i64 ret;
  register i64 r10 __asm__("r10") = d;
  register i64 r8 __asm__("r8") = e;
  register i64 r9 __asm__("r9") = f;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return ret;
}

#define SYS_write 1
#define SYS_rt_sigaction 13
#define SYS_mmap 9
#define SYS_fork 57
#define SYS_wait4 61
#define SYS_exit_group 231
#define SIGSEGV 11
#define SA_SIGINFO 4
#define SA_RESTORER 0x04000000
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_SHARED 1
#define MAP_ANONYMOUS 0x20

// ------------------------------------------------------------------------------------------------------
// minimal ucontext (x86-64)
// ------------------------------------------------------------------------------------------------------
typedef struct {
  u64 r8, r9, r10, r11, r12, r13, r14, r15;
  u64 rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
  unsigned short cs, gs, fs, ss;
  u64 err, trapno, oldmask, cr2, fpstate;
  u64 reserved[8];
} sigctx_t;
typedef struct {
  u64 uc_flags;
  void *uc_link;
  u64 ss_sp;
  int ss_flags;
  unsigned int pad;
  u64 ss_size;
  sigctx_t uc_mcontext;
  u64 uc_sigmask;
} ucontext_t;
typedef struct {
  void *handler;              // SA_SIGINFO handler: void (*)(int, siginfo_t *, void *)
  u64 sa_flags;
  void *sa_restorer;
  u64 sa_mask;
} sigaction_t;

_Static_assert(__builtin_offsetof(ucontext_t, uc_mcontext.rip) == 168, "ucontext rip slot");
_Static_assert(__builtin_offsetof(ucontext_t, uc_mcontext.rsp) == 160, "ucontext rsp slot");

typedef void (*code_fn)(void);

// ------------------------------------------------------------------------------------------------------
// reporting helpers (freestanding)
// ------------------------------------------------------------------------------------------------------
static int failures;
static long out_fd = 1;

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sc4(SYS_write, out_fd, (i64)s, slen(s), 0); }
static void out_hex(u64 v) {
  char b[19];
  b[0] = '0'; b[1] = 'x';
  for (int i = 0; i < 16; i++) { int nib = (int)((v >> (60 - 4 * i)) & 15); b[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10); }
  b[18] = '\n';
  sc4(SYS_write, out_fd, (i64)b, 19, 0);
}
static void check(int ok, const char *what) {
  if (!ok) { failures++; out("FAIL: "); out(what); out("\n"); }
  else     { out("ok:   "); out(what); out("\n"); }
}

// ------------------------------------------------------------------------------------------------------
// measurement
// ------------------------------------------------------------------------------------------------------
// Shared (MAP_SHARED) slots, so the forked child can publish what it measured.
#define SLOT_PRE 0
#define SLOT_RSP 1
#define SLOT_RIP 2
static volatile u64 *shared;

// The handler records the signal-frame RSP/RIP and exits the whole process; nothing re-enters the guest, so no resume
// path (and no compiler frame) can disturb the measurement.
static void handler(int sig, void *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  (void)sig; (void)info;
  shared[SLOT_RSP] = uc->uc_mcontext.rsp;
  shared[SLOT_RIP] = uc->uc_mcontext.rip;
  sc1(SYS_exit_group, 0);
  for (;;) {}
}

__asm__(
  ".text\n"
  ".globl pop_fault_restorer\n"
  "pop_fault_restorer:\n"
  "  mov $15, %eax\n"      // SYS_rt_sigreturn
  "  syscall\n");

extern void pop_fault_restorer(void);

static void install_handler(void) {
  sigaction_t sa;
  sa.handler = (void *)handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTORER;
  sa.sa_restorer = (void *)pop_fault_restorer;
  sa.sa_mask = 0;
  sc4(SYS_rt_sigaction, SIGSEGV, (i64)&sa, 0, 8);
}

// Run `bytes` in a fresh child and return via *out_rsp / *out_pre the fault-time RSP and the pre-fault RSP.
// Returns 1 if the child faulted and published, 0 otherwise.
static int measure(const unsigned char *bytes, int n, u64 *out_pre, u64 *out_rsp) {
  unsigned char *page = (unsigned char *)sc6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if ((long)page < 0) return 0;
  for (int i = 0; i < n; i++) page[i] = bytes[i];

  shared[SLOT_PRE] = 0;
  shared[SLOT_RSP] = 0;
  shared[SLOT_RIP] = 0;

  i64 pid = sc2(SYS_fork, 0, 0);
  if (pid == 0) {
    install_handler();
    // rbx -> shared[0]: the code page stores the pre-fault RSP there.
    __asm__ volatile("mov %0, %%rbx" :: "r"((void *)shared) : "rbx");
    ((code_fn)page)();
    sc1(SYS_exit_group, 3);     // reached only if the code did not fault
    for (;;) {}
  }
  if (pid < 0) return 0;
  sc4(SYS_wait4, pid, 0, 0, 0);

  *out_pre = shared[SLOT_PRE];
  *out_rsp = shared[SLOT_RSP];
  return shared[SLOT_RSP] != 0;
}

// ------------------------------------------------------------------------------------------------------
// the two faulting pages
// ------------------------------------------------------------------------------------------------------
// mov %rsp,%rax ; mov %rax,(%rbx) ; xor %eax,%eax ; <fault> ; ret
static const unsigned char pop_page[11] = {0x48, 0x89, 0xe0, 0x48, 0x89, 0x03, 0x31, 0xc0, 0x8f, 0x00, 0xc3};
static const unsigned char mov_page[11] = {0x48, 0x89, 0xe0, 0x48, 0x89, 0x03, 0x31, 0xc0, 0x89, 0x00, 0xc3};

__attribute__((noreturn, used)) void entry(void) {
  out("pop-fault: fault-time RSP of `pop r/m` (0x8F)\n");

  shared = (volatile u64 *)sc6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if ((long)shared < 0) { out("FAIL: shared mmap\n"); sc1(SYS_exit_group, 1); for (;;) {} }

  u64 pre, pop_rsp, mov_pre, mov_rsp;
  int pop_faulted = measure(pop_page, sizeof(pop_page), &pre, &pop_rsp);
  int mov_faulted = measure(mov_page, sizeof(mov_page), &mov_pre, &mov_rsp);

  out("  popq (%rax): pre RSP "); out_hex(pre);
  out("  popq (%rax): fault RSP "); out_hex(pop_rsp);
  out("  mov  (%rax): pre RSP "); out_hex(mov_pre);
  out("  mov  (%rax): fault RSP "); out_hex(mov_rsp);

  // Control: the equivalent store must fault, and must fault with RSP unchanged on real x86.
  check(mov_faulted, "mov %eax,(%rax) faults");
  check(mov_faulted && mov_pre == mov_rsp, "mov %eax,(%rax): RSP is unchanged at the fault (control)");

  // The regression: pop must fault and must leave RSP unchanged.
  check(pop_faulted, "popq (%rax) faults");
  check(pop_faulted && pre == pop_rsp, "popq (%rax): RSP is unchanged at the fault");
  check(pop_faulted && mov_faulted && pop_rsp == mov_rsp,
        "popq (%rax) and mov %eax,(%rax) fault at the same RSP");

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc1(SYS_exit_group, failures);
  for (;;) {}
}

__attribute__((naked, used)) void _start(void) { __asm__ volatile("and $-16, %rsp\n call entry\n"); }
