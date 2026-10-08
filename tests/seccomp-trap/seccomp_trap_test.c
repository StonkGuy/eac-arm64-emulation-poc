// SPDX-License-Identifier: MIT
//
// Regression test for the seccomp/SIGSYS path Wine's ntdll installs on x86_64 (dlls/ntdll/unix/signal_x86_64.c,
// install_bpf / sigsys_handler). Wine cannot trap guest system calls itself, so it installs a seccomp filter that returns
// SECCOMP_RET_TRAP for any syscall whose instruction pointer lies inside a chosen code range (Windows code is loaded low,
// native libraries high), and handles the resulting SIGSYS with SA_SIGINFO.
//
// This is the same filter, built by hand with raw system calls in a freestanding static program, and it checks the whole
// contract FEX has to reproduce:
//   1. the filter installs (prctl(PR_SET_NO_NEW_PRIVS) first, then seccomp(SECCOMP_SET_MODE_FILTER, flags) like Wine,
//      falling back to prctl(PR_SET_SECCOMP)); a failure is reported with its errno
//   2. a syscall made from outside the trapped range runs normally
//   3. a raw `syscall` from inside the range raises SIGSYS with si_code == SYS_SECCOMP, the right si_syscall/si_arch and
//      si_call_addr pointing just past the syscall; the handler can read the syscall number and all six arguments (rdi,
//      rsi, rdx, r10, r8, r9) out of the ucontext
//   4. the handler writes a return value into the ucontext's rax and execution resumes after the syscall with it
//   5. 1000 consecutive traps behave identically (stability)
//
// The same source builds for x86-64 (the binary to run under FEX) and for aarch64 (the reference run on the host's own
// Linux kernel): only the system-call glue, the register slots and AUDIT_ARCH differ.
// Expected: passes on a Linux kernel. Under FEX the filter install is the interesting part: it prints the errno if
// seccomp(SECCOMP_SET_MODE_FILTER) is rejected (EINVAL).
// Build: tests/seccomp-trap/build.sh [clang]   (x86-64 binary, run it under FEX)   or   build.sh native (host architecture)
// Run:   ./seccomp_trap_test   (exit status = number of failed checks)

typedef unsigned long u64;
typedef long i64;

// ------------------------------------------------------------------------------------------------------
// raw syscalls
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
enum { SYS_write = 1, SYS_rt_sigaction = 13, SYS_getpid = 39, SYS_prctl = 157, SYS_seccomp = 317, SYS_exit_group = 231 };
enum { RAX = 13, RDI = 8, RSI = 9, RDX = 12, R10 = 2, R8 = 0, R9 = 1, RIP_SLOT = 16 };
enum { AUDIT_ARCH = 0xC000003Eu };
static const int arg_slot[6] = {RDI, RSI, RDX, R10, R8, R9};
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
enum { SYS_write = 64, SYS_rt_sigaction = 134, SYS_getpid = 172, SYS_prctl = 167, SYS_seccomp = 277, SYS_exit_group = 94 };
enum { AUDIT_ARCH = 0xC00000B7u };
static const int arg_slot[6] = {0, 1, 2, 3, 4, 5};
struct ksigaction { void* handler; u64 flags; u64 mask; };
#define MAKE_ACTION(h) { (void*)(h), SA_SIGINFO, 0 }
#else
#error "x86-64 and aarch64 only"
#endif
#define sc2(n, a, b) sc6(n, (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sc3(n, a, b, c) sc6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sc5(n, a, b, c, d, e) sc6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), (i64)(e), 0)

enum { SIGSYS = 31 };
#define SA_SIGINFO 0x4
#define SA_RESTORER 0x04000000
enum { PR_SET_NO_NEW_PRIVS = 38, PR_SET_SECCOMP = 22, SECCOMP_MODE_FILTER = 2 };
enum { SECCOMP_SET_MODE_FILTER = 1 };
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW (1UL << 2)   // Wine's flags
enum { SECCOMP_RET_ALLOW = 0x7fff0000, SECCOMP_RET_TRAP = 0x00030000 };
enum { SYS_SECCOMP = 1 };

// ------------------------------------------------------------------------------------------------------
// offsets inside the kernel ucontext_t, from the Linux ABI layout. The gregs offsets are the ones the other tests in
// tests/ already rely on (signal-regs writes ucontext.rip as 168(%rdx) on x86-64 and ucontext.pc as [x2,#440] on
// aarch64, i.e. gregs at 40 and 184); x86-64 gregs[REG_RIP=16], aarch64 regs[31]+sp+pc+pstate with pc at index 32.
// ------------------------------------------------------------------------------------------------------
#if defined(__x86_64__)
enum { GREGS_OFF = 40, NR_SLOT = RAX, RET_SLOT = RAX, PC_SLOT = RIP_SLOT };
#else
enum { GREGS_OFF = 184, NR_SLOT = 8, RET_SLOT = 0, PC_SLOT = 32 };
#endif

// ------------------------------------------------------------------------------------------------------
// helpers (no libc)
// ------------------------------------------------------------------------------------------------------
static u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sc3(SYS_write, 1, s, slen(s)); }
static void outhex(u64 v) {
  char b[19]; b[0] = '0'; b[1] = 'x'; int i;
  for (i = 0; i < 16; ++i) { int d = (v >> (60 - 4 * i)) & 15; b[2 + i] = d < 10 ? '0' + d : 'a' + d - 10; }
  b[18] = 0; out(b);
}
static int failures;
static void check(int ok, const char* what) {
  out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) ++failures;
}

// ------------------------------------------------------------------------------------------------------
// trapped code range: the SIGSYS handler and the raw-syscall stub both live in one RWX section, so the filter traps
// exactly the syscall the stub makes and nothing the C half of the program does. Everything is hand-written assembly;
// the handler copies the siginfo and the ucontext registers out first, then writes the return value into the guest rax
// (aarch64 x0), so the checks afterward read the same state Wine's handler sees.
// ------------------------------------------------------------------------------------------------------
#if defined(__x86_64__)
__asm__(".section .safecode,\"awx\",@progbits\n.balign 16\n"
        ".globl rng_start\nrng_start:\n"
        ".balign 16\n.globl hsafe\nhsafe:\n"
        "  lea sigo_buf(%rip), %r11\n"                 // copy 40 bytes of siginfo_t (through si_arch)
        "  mov $5, %rcx\n"
        "1:\n  mov (%rsi), %rax\n  mov %rax, (%r11)\n  add $8, %rsi\n  add $8, %r11\n  dec %rcx\n  jnz 1b\n"
        "  mov g_gregs_off(%rip), %rcx\n"               // copy the ucontext registers
        "  lea (%rdx, %rcx), %rsi\n"
        "  lea gregs_buf(%rip), %rdi\n"
        "  mov $27, %ecx\n"                            // 27 x86-64 gregs
        "2:\n  mov (%rsi), %rax\n  mov %rax, (%rdi)\n  add $8, %rsi\n  add $8, %rdi\n  dec %rcx\n  jnz 2b\n"
        "  incq trap_count(%rip)\n"
        "  mov $144, %ecx\n"                           // ucontext.rax (gregs[13]) = the value the caller should see
        "  mov retval(%rip), %rax\n"
        "  mov %rax, (%rdx, %rcx)\n"
        "  ret\n"
        ".balign 16\n.globl trapped_syscall\ntrapped_syscall:\n"   // i64(nr, a, b, c, d, e, f): raw x86-64 syscall
        "  push %rbp\n  mov %rsp, %rbp\n"
        "  mov %rdi, %rax\n  mov %rsi, %rdi\n  mov %rdx, %rsi\n  mov %rcx, %rdx\n"
        "  mov %r8, %r10\n  mov %r9, %r8\n  mov 16(%rbp), %r9\n"
        "  syscall\n"
        ".globl trapped_after\ntrapped_after:\n"
        "  leave\n  ret\n"
        ".balign 8\n.globl sigo_buf\nsigo_buf: .zero 40\n"
        ".globl gregs_buf\ngregs_buf: .zero 27*8\n"
        ".globl trap_count\n.globl retval\n.globl g_gregs_off\n"
        "trap_count: .quad 0\nretval: .quad 0\ng_gregs_off: .quad 0\n"
        ".globl rng_end\nrng_end:\n"
        ".text\n");
#else
__asm__(".section .safecode,\"awx\",@progbits\n.balign 16\n"
        ".globl rng_start\nrng_start:\n"
        ".balign 16\n.globl hsafe\nhsafe:\n"
        "  mov x4, x1\n"                                // copy 40 bytes of siginfo_t (through si_arch)
        "  adr x5, sigo_buf\n"
        "  mov x3, #5\n"
        "1:\n  ldr x6, [x4]\n  str x6, [x5]\n  add x4, x4, #8\n  add x5, x5, #8\n  subs x3, x3, #1\n  b.ne 1b\n"
        "  adr x6, g_gregs_off\n  ldr x6, [x6]\n"      // copy the ucontext registers
        "  add x4, x2, x6\n"
        "  adr x5, gregs_buf\n"
        "  mov x6, #34\n"                              // 34 aarch64 regs
        "2:\n  ldr x7, [x4]\n  str x7, [x5]\n  add x4, x4, #8\n  add x5, x5, #8\n  subs x6, x6, #1\n  b.ne 2b\n"
        "  adr x6, trap_count\n  ldr x7, [x6]\n  add x7, x7, #1\n  str x7, [x6]\n"
        "  mov x6, #184\n"
        "  add x6, x2, x6\n"                            // ucontext x0 (regs[0]) = the value the caller should see
        "  adr x7, retval\n  ldr x7, [x7]\n  str x7, [x6]\n"
        "  ret\n"
        ".balign 16\n.globl trapped_syscall\ntrapped_syscall:\n"    // i64(nr, a, b, c, d, e, f): raw svc
        "  mov x9, x0\n  mov x0, x1\n  mov x1, x2\n  mov x2, x3\n  mov x3, x4\n  mov x4, x5\n  mov x5, x6\n"
        "  mov x8, x9\n  svc 0\n"
        ".globl trapped_after\ntrapped_after:\n  ret\n"
        ".balign 8\n.globl sigo_buf\nsigo_buf: .zero 40\n"
        ".globl gregs_buf\ngregs_buf: .zero 34*8\n"
        ".globl trap_count\n.globl retval\n.globl g_gregs_off\n"
        "trap_count: .quad 0\nretval: .quad 0\ng_gregs_off: .quad 0\n"
        ".globl rng_end\nrng_end:\n"
        ".text\n");
#endif

extern unsigned char rng_start[], rng_end[], trapped_after[];
extern void hsafe(void);
extern i64 trapped_syscall(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f);
extern volatile u64 trap_count, retval, g_gregs_off;
extern u64 gregs_buf[34], sigo_buf[5];

// BPF program / sock_fprog, exactly the shape Wine builds: trap when the syscall's instruction pointer is inside
// [rng_start, rng_end) and the arch matches, allow everything else (native code at high addresses included).
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
#define IP_HI_OFF 12    // offsetof(struct seccomp_data, instruction_pointer) + 4
#define IP_LO_OFF 8     // offsetof(struct seccomp_data, instruction_pointer)
#define ARCH_OFF 4      // offsetof(struct seccomp_data, arch)

static struct sock_filter filter[9];
static i64 install_filter(void) {
  const unsigned int lo = (unsigned int)(u64)rng_start, hi = (unsigned int)(u64)rng_end;
  struct sock_fprog prog = {9, filter};
  // Same shape as Wine's filter: allow syscalls whose instruction pointer is not in the range (and native code whose
  // high word is nonzero), trap those that are, allow other architectures.
  //  0 LD  ip_hi         A = instruction_pointer >> 32
  //  1 JEQ K=0           A==0 (low address) -> 2 ; else -> 7 (ALLOW, native library)
  //  2 LD  ip_lo
  //  3 JGE lo            A>=lo -> 4 ; else -> 7 (ALLOW)
  //  4 JGE hi            A>=hi -> 7 (ALLOW) ; else -> 5 (inside the range, check the arch)
  //  5 LD  arch
  //  6 JEQ AUDIT_ARCH    match -> 8 (TRAP) ; else -> 7 (ALLOW, e.g. i386)
  //  7 RET ALLOW
  //  8 RET TRAP
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
  // Wine tries seccomp(SECCOMP_SET_MODE_FILTER, flags) and falls back to prctl(PR_SET_SECCOMP).
  i64 r = sc3(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_SPEC_ALLOW, &prog);
  if (r != 0) r = sc5(SYS_prctl, PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0);
  return r;
}

// glibc/kernel siginfo_t: si_signo @0, si_code @4, then the 8-byte-aligned _sifields at 16, so the sigsys fields are
// si_call_addr @16 (the same slot as si_addr), si_syscall @24, si_arch @28.
#define SI_SIGNO() ((u64)(unsigned int)sigo_buf[0])
#define SI_CODE() ((u64)(unsigned int)sigo_buf[1])
#define SI_CALL_ADDR() (sigo_buf[2])
#define SI_SYSCALL() ((u64)(unsigned int)sigo_buf[3])
#define SI_ARCH() ((u64)(sigo_buf[3] >> 32))
#define G(slot) (gregs_buf[(slot)])

__attribute__((used)) static void entry_c(void) {
  u64 pre_pid = sc2(SYS_getpid, 0, 0);

  struct ksigaction a = MAKE_ACTION(hsafe);
  check(sc6(SYS_rt_sigaction, SIGSYS, (i64)&a, 0, 8, 0, 0) == 0, "install the SIGSYS handler (SA_SIGINFO)");

  g_gregs_off = GREGS_OFF;
  i64 ir = install_filter();
  check(ir == 0, "install the seccomp filter (seccomp(SECCOMP_SET_MODE_FILTER)/prctl)");
  if (ir != 0) { out("  filter install failed, errno "); outhex((u64)-ir); out("\n"); }

  // (2) a syscall from outside the trapped range is unaffected
  u64 now_pid = sc2(SYS_getpid, 0, 0);
  check(now_pid == pre_pid && now_pid > 0, "syscall outside the trapped range runs normally");

  // (3)(4) one trapped syscall, checked field by field against what Wine's handler reads
  const i64 nr = 0x1000, args[6] = {0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666};
  retval = 0x123456789UL;
  trap_count = 0;
  i64 got = trapped_syscall(nr, args[0], args[1], args[2], args[3], args[4], args[5]);
  check(trap_count == 1, "a raw syscall inside the range raises SIGSYS");
  check(SI_SIGNO() == SIGSYS, "si_signo == SIGSYS");
  check(SI_CODE() == SYS_SECCOMP, "si_code == SYS_SECCOMP");
  check(SI_SYSCALL() == (u64)nr, "si_syscall == the trapped syscall number");
  check(SI_ARCH() == (u64)AUDIT_ARCH, "si_arch == AUDIT_ARCH_*");
  check(SI_CALL_ADDR() == (u64)trapped_after, "si_call_addr points just past the syscall instruction");
  check(G(PC_SLOT) == (u64)trapped_after, "ucontext pc is just past the syscall instruction");
  check(G(NR_SLOT) == (u64)nr, "ucontext carries the syscall number (rax / x8)");
  int args_ok = 1;
  for (int i = 0; i < 6; ++i) if (G(arg_slot[i]) != (u64)args[i]) args_ok = 0;
  check(args_ok, "ucontext carries all six syscall arguments (rdi,rsi,rdx,r10,r8,r9)");
  check(got == (i64)retval, "the handler's ucontext rax is the return value and execution resumes after the syscall");

  // (5) 1000 traps in a row
  int stable = 1;
  for (int i = 0; i < 1000; ++i)
    if (trapped_syscall(nr, args[0], args[1], args[2], args[3], args[4], args[5]) != (i64)retval) stable = 0;
  check(trap_count == 1001 && stable, "1000 consecutive traps stay stable");

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc2(SYS_exit_group, failures, 0);
}
#if defined(__x86_64__)
__attribute__((naked, used)) void _start(void) { __asm__ volatile("and $-16, %rsp\n call entry_c\n"); }
#else
__asm__(".text\n.globl _start\n_start:\n bl entry_c\n");
#endif
