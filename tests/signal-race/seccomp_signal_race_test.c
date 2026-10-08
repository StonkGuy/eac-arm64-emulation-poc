// SPDX-License-Identifier: MIT
//
// Two-thread race between a seccomp SECCOMP_RET_TRAP and an asynchronous signal.
//
// Reproduces the shape Wine uses for NtGetContextThread on x86_64: a worker thread issues raw system calls from code a
// seccomp filter traps (dlls/ntdll/unix/signal_x86_64.c, install_bpf), i.e. every one of them becomes a SIGSYS, while
// another thread repeatedly delivers an unrelated signal (SIGUSR1) to it -- the way Wine's NtGetContextThread signals
// the target thread to read its context. In the real repro (worker in raw NT syscalls, main looping
// NtGetContextThread) that pair hangs the emulator: the worker spins forever and the main thread sleeps.
//
// In-process version: the worker loops one raw syscall from inside the trapped range (each iteration is trap -> SIGSYS
// -> handler -> resume); the main thread hammers it with SIGUSR1. Both signals land on the same handler, which records
// how many arrived and whether the interrupted ucontext RIP was the trapped syscall (i.e. the racing signal was taken
// while the thread was inside FEX's seccomp/SIGSYS path). The test FAILS if, once the race is over, the worker cannot
// be stopped -- the hang signature.
//
// The same source builds for x86-64 (run under FEX) and aarch64 (a real kernel carries the identical filter and signal
// handling): both must finish and stop the worker. Build: tests/signal-race/build.sh [clang]  or  build.sh native
// Run:   ./seccomp_signal_race_test   (exit status = number of failed checks)

typedef unsigned long u64;
typedef long i64;

// ------------------------------------------------------------------------------------------------------
// raw syscalls and per-architecture constants (no libc)
// ------------------------------------------------------------------------------------------------------
#if defined(__x86_64__)
static inline i64 sc6(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  i64 ret;
  register i64 r10 __asm__("r10") = d;
  register i64 r8 __asm__("r8") = e;
  register i64 r9 __asm__("r9") = f;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return ret;
}
enum { SYS_write = 1, SYS_rt_sigaction = 13, SYS_getpid = 39, SYS_clone = 56, SYS_exit = 60, SYS_futex = 202,
       SYS_gettid = 186, SYS_tgkill = 234, SYS_prctl = 157, SYS_seccomp = 317, SYS_nanosleep = 35, SYS_exit_group = 231 };
enum { GREGS_OFF = 40, RIP_SLOT = 16 };
enum { AUDIT_ARCH = 0xC000003Eu };
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
enum { SYS_write = 64, SYS_rt_sigaction = 134, SYS_getpid = 172, SYS_clone = 220, SYS_exit = 93, SYS_futex = 98,
       SYS_gettid = 178, SYS_tgkill = 131, SYS_prctl = 167, SYS_seccomp = 277, SYS_nanosleep = 101, SYS_exit_group = 94 };
enum { GREGS_OFF = 184, PC_SLOT = 32 };
enum { AUDIT_ARCH = 0xC00000B7u };
struct ksigaction { void* handler; u64 flags; u64 mask; };
#define MAKE_ACTION(h) { (void*)(h), SA_SIGINFO, 0 }
#else
#error "x86-64 and aarch64 only"
#endif
#define sc2(n, a, b) sc6(n, (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sc3(n, a, b, c) sc6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sc5(n, a, b, c, d, e) sc6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), (i64)(e), 0)

enum { SIGSYS = 31, SIGUSR1 = 10 };
#define SA_SIGINFO 0x4
#define SA_RESTORER 0x04000000
enum { PR_SET_NO_NEW_PRIVS = 38, PR_SET_SECCOMP = 22, SECCOMP_MODE_FILTER = 2 };
enum { SECCOMP_SET_MODE_FILTER = 1 };
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW (1UL << 2)
enum { SECCOMP_RET_ALLOW = 0x7fff0000, SECCOMP_RET_TRAP = 0x00030000 };
enum { SYS_SECCOMP = 1 };
enum { CLONE_THREAD_FLAGS_X86 = 0x10F00, CLONE_THREAD_FLAGS_ARM = 0x10F00 };

// ------------------------------------------------------------------------------------------------------
// helpers (no libc)
// ------------------------------------------------------------------------------------------------------
static u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sc3(SYS_write, 1, s, slen(s)); }
static int failures;
static void check(int ok, const char* what) {
  out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) ++failures;
}

// ------------------------------------------------------------------------------------------------------
// RWX section. rng_start..rng_end is trapped by the filter. hsafe is the shared signal handler: it copies the
// interrupted ucontext's RIP out, counts the signal, and works out whether the interrupted code was the trap stub
// (the racing signal was taken while the thread was inside FEX's seccomp/SIGSYS path). trapped_probe issues one raw
// syscall, so each call is one trap.
// ------------------------------------------------------------------------------------------------------
#if defined(__x86_64__)
__asm__(".section .safecode,\"awx\",@progbits\n.balign 16\n"
        ".globl rng_start\nrng_start:\n"
        ".balign 16\n.globl hsafe\nhsafe:\n"
        "  mov g_gregs_off(%rip), %rcx\n"        // rdx = ucontext, gregs at +GREGS_OFF
        "  lea (%rdx, %rcx), %rcx\n"
        "  mov 128(%rcx), %rax\n"                // gregs[16] is RIP (16*8 = 128)
        "  mov %rax, h_rip(%rip)\n"
        "  lea trapped_after(%rip), %rsi\n"
        "  cmp %rsi, %rax\n"
        "  jne 1f\n"
        "  incq h_in_probe(%rip)\n"
        "  jmp 2f\n"
        "1:\n  incq h_outside(%rip)\n"
        "2:\n  incq h_count(%rip)\n"
        "  ret\n"
        ".balign 16\n.globl trapped_probe\ntrapped_probe:\n"   // one raw x86-64 syscall, at most 6 args
        "  push %rbp\n  mov %rsp, %rbp\n"
        "  mov %rdi, %rax\n  mov %rsi, %rdi\n  mov %rdx, %rsi\n  mov %rcx, %rdx\n"
        "  mov %r8, %r10\n  mov %r9, %r8\n  mov 16(%rbp), %r9\n"
        "  syscall\n"
        ".globl trapped_after\ntrapped_after:\n"
        "  leave\n  ret\n"
        ".balign 8\n.globl h_count\n.globl h_in_probe\n.globl h_outside\n.globl h_rip\n.globl g_gregs_off\n"
        "h_count: .quad 0\nh_in_probe: .quad 0\nh_outside: .quad 0\nh_rip: .quad 0\ng_gregs_off: .quad 0\n"
        ".globl rng_end\nrng_end:\n"
        ".text\n");
// spawn_thread is deliberately outside the trapped range: with the filter installed, a clone() issued from inside the
// range would itself be trapped and the child would never run.
__asm__(".text\n.balign 16\n.globl spawn_thread\nspawn_thread:\n"      // i64(stacktop, fn)
        "  mov %rsi, %r9\n"
        "  mov %rdi, %rsi\n"
        "  mov $0x10F00, %edi\n"
        "  xor %edx, %edx\n  xor %r10d, %r10d\n  xor %r8d, %r8d\n"
        "  mov $56, %eax\n  syscall\n"
        "  test %rax, %rax\n  jnz 3f\n"
        "  movl $1, w_started(%rip)\n"
        "  call *%r9\n"
        "  mov $60, %eax\n  xor %edi, %edi\n  syscall\n"
        "3:\n  ret\n");
#else
__asm__(".section .safecode,\"awx\",@progbits\n.balign 16\n"
        ".globl rng_start\nrng_start:\n"
        ".balign 16\n.globl hsafe\nhsafe:\n"
        "  adr x6, g_gregs_off\n  ldr x6, [x6]\n"     // x2 = ucontext, gregs at +GREGS_OFF
        "  add x6, x2, x6\n"
        "  ldr x7, [x6, #256]\n"                      // regs[32] is PC (32*8 = 256)
        "  adr x5, h_rip\n  str x7, [x5]\n"
        "  adr x5, trapped_after\n  cmp x5, x7\n"
        "  b.ne 1f\n"
        "  adr x5, h_in_probe\n  ldr x7, [x5]\n  add x7, x7, #1\n  str x7, [x5]\n"
        "  b 2f\n"
        "1:\n  adr x5, h_outside\n  ldr x7, [x5]\n  add x7, x7, #1\n  str x7, [x5]\n"
        "2:\n  adr x5, h_count\n  ldr x7, [x5]\n  add x7, x7, #1\n  str x7, [x5]\n"
        "  ret\n"
        ".balign 16\n.globl trapped_probe\ntrapped_probe:\n"
        "  mov x9, x0\n  mov x0, x1\n  mov x1, x2\n  mov x2, x3\n  mov x3, x4\n  mov x4, x5\n  mov x5, x6\n"
        "  mov x8, x9\n  svc 0\n"
        ".globl trapped_after\ntrapped_after:\n  ret\n"
        ".balign 8\n.globl h_count\n.globl h_in_probe\n.globl h_outside\n.globl h_rip\n.globl g_gregs_off\n"
        "h_count: .quad 0\nh_in_probe: .quad 0\nh_outside: .quad 0\nh_rip: .quad 0\ng_gregs_off: .quad 0\n"
        ".globl rng_end\nrng_end:\n"
        ".text\n");
// spawn_thread is deliberately outside the trapped range: a clone() issued from inside the range would itself be
// trapped, so the child would never run.
__asm__(".text\n.balign 16\n.globl spawn_thread\nspawn_thread:\n"      // i64(stacktop, fn)
        "  mov x9, x1\n"
        "  mov x1, x0\n"
        "  movz x0, #0x0F00\n  movk x0, #0x1, lsl #16\n"        // 0x10F00
        "  mov x2, #0\n  mov x3, #0\n  mov x4, #0\n"
        "  mov x8, #220\n  svc 0\n"
        "  cbnz x0, 3f\n"
        "  adr x6, w_started\n  mov w7, #1\n  str w7, [x6]\n"
        "  blr x9\n"
        "  mov x0, #0\n  mov x8, #93\n  svc 0\n"
        "3:\n  ret\n");
#endif

extern unsigned char rng_start[], rng_end[], trapped_after[];
extern void hsafe(void);
extern i64 trapped_probe(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f);
extern i64 spawn_thread(u64 stacktop, void (*fn)(void));
extern volatile u64 h_count, h_in_probe, h_outside, h_rip, g_gregs_off;

// filter: trap a syscall whose instruction pointer is inside [rng_start, rng_end) and whose arch matches.
struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter* filter; };
#define BPF_LD 0x00
#define BPF_W 0x00
#define BPF_ABS 0x20
#define BPF_JMP 0x05
#define BPF_JEQ 0x10
#define BPF_JGE 0x30
#define BPF_RET 0x06
#define BPF_K 0x00
#define IP_HI_OFF 12
#define IP_LO_OFF 8
#define ARCH_OFF 4

static struct sock_filter filter[9];
static i64 install_filter(void) {
  const unsigned int lo = (unsigned int)(u64)rng_start, hi = (unsigned int)(u64)rng_end;
  struct sock_fprog prog = {9, filter};
  struct sock_filter f[9] = {
    {BPF_LD  | BPF_W | BPF_ABS, 0, 0, IP_HI_OFF},
    {BPF_JMP | BPF_JEQ | BPF_K, 0, 5, 0},
    {BPF_LD  | BPF_W | BPF_ABS, 0, 0, IP_LO_OFF},
    {BPF_JMP | BPF_JGE | BPF_K, 0, 3, lo},
    {BPF_JMP | BPF_JGE | BPF_K, 2, 0, hi},
    {BPF_LD  | BPF_W | BPF_ABS, 0, 0, ARCH_OFF},
    {BPF_JMP | BPF_JEQ | BPF_K, 1, 0, AUDIT_ARCH},
    {BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW},
    {BPF_RET | BPF_K, 0, 0, SECCOMP_RET_TRAP},
  };
  for (int i = 0; i < 9; ++i) filter[i] = f[i];
  if (sc5(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
  i64 r = sc3(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_SPEC_ALLOW, &prog);
  if (r != 0) r = sc5(SYS_prctl, PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0);
  return r;
}

// ------------------------------------------------------------------------------------------------------
// the race
// ------------------------------------------------------------------------------------------------------
struct timespec { long tv_sec, tv_nsec; };
static volatile int w_stop;                 // main -> worker: finish
static volatile int w_active;               // 1 while the worker runs
volatile int w_started;                     // child shim sets this before entering the worker
static volatile u64 probe_calls;            // how many trapped syscalls the worker issued
static u64 thread_stack[16384] __attribute__((aligned(16)));   // the worker's stack (grows down from the top)

static void worker(void) {
  __atomic_store_n(&w_active, 1, __ATOMIC_RELEASE);
  for (;;) {
    for (int i = 0; i < 2000; ++i) {
      if (w_stop) goto done;
      trapped_probe(0x1000, 0, 0, 0, 0, 0, 0);   // one trap -> SIGSYS -> handler -> resume
      ++probe_calls;
    }
    if (w_stop) break;
    sc2(SYS_getpid, 0, 0);                        // a syscall from outside the trapped range
  }
done:
  __atomic_store_n(&w_active, 0, __ATOMIC_RELEASE);
  for (;;) sc6(SYS_nanosleep, (i64)&(struct timespec){0, 1000000}, 0, 0, 0, 0, 0);
}

__attribute__((used)) static void entry_c(void) {
  struct ksigaction a = MAKE_ACTION(hsafe);
  i64 h1 = sc6(SYS_rt_sigaction, SIGSYS, (i64)&a, 0, 8, 0, 0);
  i64 h2 = sc6(SYS_rt_sigaction, SIGUSR1, (i64)&a, 0, 8, 0, 0);
  check(h1 == 0 && h2 == 0, "install the SIGSYS and SIGUSR1 handlers");

  g_gregs_off = GREGS_OFF;
  check(install_filter() == 0, "install the SECCOMP_RET_TRAP filter");

  // Worker thread: trapped syscalls in a loop. Main thread: SIGUSR1 at it, the way NtGetContextThread signals a thread.
  i64 tid = spawn_thread((u64)&thread_stack[16384], worker);
  check(tid > 0, "spawn the worker thread");
  if (tid <= 0) { out("RESULT: FAIL\n"); sc2(SYS_exit_group, 1, 0); }

  // wait until the worker is running its loop before racing it
  for (int i = 0; i < 5000 && !w_started; ++i) sc6(SYS_nanosleep, (i64)&(struct timespec){0, 100000}, 0, 0, 0, 0, 0);
  check(w_started, "the worker thread started");

  i64 pid = sc2(SYS_getpid, 0, 0);
  const int rounds = 40000;
  for (int i = 0; i < rounds; ++i)
    sc3(SYS_tgkill, pid, tid, SIGUSR1);           // racing signal while the worker is in its trap loop

  u64 h_at_race_end = h_count;
  u64 in_probe = h_in_probe;
  check(h_at_race_end > 0, "the racing signals were handled");
  check(in_probe > 0, "a racing signal was taken while the worker was in the seccomp/SIGSYS path");

  // Stop the worker. If the race wedged the emulator's signal/seccomp state (the repro's hang), it never observes
  // w_stop and this bounded wait expires -- the failure this test exists to catch.
  __atomic_store_n(&w_stop, 1, __ATOMIC_RELEASE);
  int stopped = 0;
  for (int i = 0; i < 3000 && !stopped; ++i) {
    sc6(SYS_nanosleep, (i64)&(struct timespec){0, 1000000}, 0, 0, 0, 0, 0);
    if (__atomic_load_n(&w_active, __ATOMIC_ACQUIRE) == 0) stopped = 1;
  }
  check(stopped, "the worker thread stops when asked after the race (no hang)");
  check(probe_calls > 0, "the worker kept making trapped syscalls through the race");

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc2(SYS_exit_group, failures, 0);
}
#if defined(__x86_64__)
__attribute__((naked, used)) void _start(void) { __asm__ volatile("and $-16, %rsp\n call entry_c\n"); }
#else
__asm__(".text\n.globl _start\n_start:\n bl entry_c\n");
#endif
