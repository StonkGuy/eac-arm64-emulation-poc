// SPDX-License-Identifier: MIT
//
// Regression test for a seccomp SECCOMP_RET_TRAP that must *suppress* the syscall.
//
// Linux never runs a syscall that a filter traps: the kernel builds a SIGSYS siginfo_t for the instruction after the
// syscall and enters the handler without executing anything. FEX emulates Wine's filter and, for RET_TRAP, used to let
// the syscall dispatcher fall through and run the trapped number as a real Linux syscall anyway. Wine traps every
// syscall issued from its preloaded NT stubs, and those stubs carry small Linux numbers -- NtClose(0xf) is rt_sigreturn
// -- so executing one re-enters Wine's interrupted syscall dispatcher mid-flight.
//
// The check needs a trapped syscall whose execution is observable *independently of the return register*, because
// rt_sigreturn overwrites RAX with whatever the handler left in the ucontext -- so a wrong return value cannot tell
// "the syscall ran and was then masked" from "the syscall never ran". A syscall with a memory side effect can:
//   * uname(buf) -- if it runs it fills buf; if suppressed buf stays zero
// The handler writes a fixed marker into the ucontext's return register as well, so the return-value path is checked at
// the same time.
//
// The same source builds for x86-64 (run under FEX, which emulates the filter) and for aarch64 (run on a real kernel,
// which installs the identical filter and enforces the semantics natively): both must return the marker.
// Build: tests/seccomp-trap-noexec/build.sh [clang]   (x86-64 binary, run it under FEX)   or   build.sh native
// Run:   ./seccomp_trap_noexec_test   (exit status = number of failed checks)

typedef unsigned long u64;
typedef long i64;

// ------------------------------------------------------------------------------------------------------
// raw syscalls (no libc)
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
enum { SYS_write = 1, SYS_rt_sigaction = 13, SYS_getpid = 39, SYS_uname = 63, SYS_prctl = 157, SYS_seccomp = 317, SYS_exit_group = 231 };
enum { RAX = 13, RIP_SLOT = 16 };
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
enum { SYS_write = 64, SYS_rt_sigaction = 134, SYS_uname = 160, SYS_getpid = 172, SYS_prctl = 167, SYS_seccomp = 277, SYS_exit_group = 94 };
enum { RAX = 0, PC_SLOT = 32 };
enum { AUDIT_ARCH = 0xC00000B7u };
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

#if defined(__x86_64__)
enum { GREGS_OFF = 40 };
#else
enum { GREGS_OFF = 184 };
#endif

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
// RWX section: rng_start..rng_end is the range the filter traps. hsafe is the SIGSYS handler (copies siginfo and the
// ucontext registers out, then writes the marker into the ucontext return register) and trap_probe makes one raw
// syscall for its argument. Both live in the range so the probe's own syscall is what traps.
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
        "  mov $27, %ecx\n"
        "2:\n  mov (%rsi), %rax\n  mov %rax, (%rdi)\n  add $8, %rsi\n  add $8, %rdi\n  dec %rcx\n  jnz 2b\n"
        "  incq trap_count(%rip)\n"
        "  mov $144, %ecx\n"                            // ucontext.rax (gregs[13]) = the marker
        "  mov marker(%rip), %rax\n"
        "  mov %rax, (%rdx, %rcx)\n"
        "  ret\n"
        ".balign 16\n.globl trap_probe\ntrap_probe:\n"  // i64(nr, a, b, c, d, e, f): one raw x86-64 syscall
        "  push %rbp\n  mov %rsp, %rbp\n"
        "  mov %rdi, %rax\n  mov %rsi, %rdi\n  mov %rdx, %rsi\n  mov %rcx, %rdx\n"
        "  mov %r8, %r10\n  mov %r9, %r8\n  mov 16(%rbp), %r9\n"
        "  syscall\n"
        ".globl trap_probe_after\ntrap_probe_after:\n"
        "  leave\n  ret\n"
        ".balign 8\n.globl sigo_buf\nsigo_buf: .zero 40\n"
        ".globl gregs_buf\ngregs_buf: .zero 27*8\n"
        ".globl trap_count\n.globl marker\n.globl g_gregs_off\n"
        "trap_count: .quad 0\nmarker: .quad 0\ng_gregs_off: .quad 0\n"
        ".globl rng_end\nrng_end:\n"
        ".text\n");
#else
__asm__(".section .safecode,\"awx\",@progbits\n.balign 16\n"
        ".globl rng_start\nrng_start:\n"
        ".balign 16\n.globl hsafe\nhsafe:\n"
        "  mov x4, x1\n"
        "  adr x5, sigo_buf\n"
        "  mov x3, #5\n"
        "1:\n  ldr x6, [x4]\n  str x6, [x5]\n  add x4, x4, #8\n  add x5, x5, #8\n  subs x3, x3, #1\n  b.ne 1b\n"
        "  adr x6, g_gregs_off\n  ldr x6, [x6]\n"
        "  add x4, x2, x6\n"
        "  adr x5, gregs_buf\n"
        "  mov x6, #34\n"
        "2:\n  ldr x7, [x4]\n  str x7, [x5]\n  add x4, x4, #8\n  add x5, x5, #8\n  subs x6, x6, #1\n  b.ne 2b\n"
        "  adr x6, trap_count\n  ldr x7, [x6]\n  add x7, x7, #1\n  str x7, [x6]\n"
        "  mov x6, #184\n  add x6, x2, x6\n"            // ucontext x0 (regs[0]) = the marker
        "  adr x7, marker\n  ldr x7, [x7]\n  str x7, [x6]\n"
        "  ret\n"
        ".balign 16\n.globl trap_probe\ntrap_probe:\n"  // i64(nr, a, b, c, d, e, f): one raw svc
        "  mov x9, x0\n  mov x0, x1\n  mov x1, x2\n  mov x2, x3\n  mov x3, x4\n  mov x4, x5\n  mov x5, x6\n"
        "  mov x8, x9\n"
        "  svc 0\n"
        ".globl trap_probe_after\ntrap_probe_after:\n"
        "  ret\n"
        ".balign 8\n.globl sigo_buf\nsigo_buf: .zero 40\n"
        ".globl gregs_buf\ngregs_buf: .zero 34*8\n"
        ".globl trap_count\n.globl marker\n.globl g_gregs_off\n"
        "trap_count: .quad 0\nmarker: .quad 0\ng_gregs_off: .quad 0\n"
        ".globl rng_end\nrng_end:\n"
        ".text\n");
#endif

extern unsigned char rng_start[], rng_end[], trap_probe_after[];
extern void hsafe(void);
extern i64 trap_probe(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f);
extern volatile u64 trap_count, marker, g_gregs_off;
extern u64 gregs_buf[34], sigo_buf[5];

// BPF program / sock_fprog, the shape Wine builds: trap when the syscall's instruction pointer is inside
// [rng_start, rng_end) and the arch matches, allow everything else.
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

static struct sock_filter g_filter[9];
static i64 install_filter(void) {
  const unsigned int lo = (unsigned int)(u64)rng_start, hi = (unsigned int)(u64)rng_end;
  struct sock_fprog prog = {9, g_filter};
  struct sock_filter f[9] = {
    {BPF_LD  | BPF_W | BPF_ABS, 0, 0, IP_HI_OFF},          // A = ip >> 32
    {BPF_JMP | BPF_JEQ | BPF_K, 0, 5, 0},                  // low address -> 2, else ALLOW (native)
    {BPF_LD  | BPF_W | BPF_ABS, 0, 0, IP_LO_OFF},          // A = ip
    {BPF_JMP | BPF_JGE | BPF_K, 0, 3, lo},                 // ip >= lo -> 4, else ALLOW
    {BPF_JMP | BPF_JGE | BPF_K, 2, 0, hi},                 // ip >= hi -> ALLOW, else 5
    {BPF_LD  | BPF_W | BPF_ABS, 0, 0, ARCH_OFF},           // A = arch
    {BPF_JMP | BPF_JEQ | BPF_K, 1, 0, AUDIT_ARCH},         // match -> TRAP, else ALLOW
    {BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW},
    {BPF_RET | BPF_K, 0, 0, SECCOMP_RET_TRAP},
  };
  for (int i = 0; i < 9; ++i) g_filter[i] = f[i];
  if (sc5(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
  i64 r = sc3(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_SPEC_ALLOW, &prog);
  if (r != 0) r = sc5(SYS_prctl, PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0);
  return r;
}

#define SI_SIGNO() ((u64)(unsigned int)sigo_buf[0])
#define SI_CODE() ((u64)(unsigned int)sigo_buf[1])
#define SI_CALL_ADDR() (sigo_buf[2])
#define G(slot) (gregs_buf[(slot)])

__attribute__((used)) static void entry_c(void) {
  struct ksigaction a = MAKE_ACTION(hsafe);
  check(sc6(SYS_rt_sigaction, SIGSYS, (i64)&a, 0, 8, 0, 0) == 0, "install the SIGSYS handler (SA_SIGINFO)");

  g_gregs_off = GREGS_OFF;
  i64 ir = install_filter();
  check(ir == 0, "install the SECCOMP_RET_TRAP filter");

  // The handler reports a marker no real syscall returns, so "the syscall did not run" is observable.
  marker = 0x5A5A5A5AUL;

  // 1. uname(): its only visible effect is that it writes a utsname into the buffer. The return value cannot distinguish
  // "ran" from "never ran" (rt_sigreturn overwrites RAX with the handler's marker either way), so the buffer is the test.
  u64 buf[24];   // struct utsname is 6 * 65 = 390 bytes; 24 u64 is plenty of slack, all pre-zeroed
  for (int i = 0; i < 24; ++i) buf[i] = 0;
  trap_count = 0;
  i64 got = trap_probe(SYS_uname, (i64)buf, 0, 0, 0, 0, 0);
  int buf_untouched = 1;
  for (int i = 0; i < 24; ++i) if (buf[i] != 0) buf_untouched = 0;
  check(trap_count == 1, "the probe's syscall raises SIGSYS");
  check(SI_SIGNO() == SIGSYS && SI_CODE() == SYS_SECCOMP, "si_signo/si_code describe a seccomp trap");
  check(SI_CALL_ADDR() == (u64)trap_probe_after, "si_call_addr points just past the syscall");
  check(G(RAX) == (u64)SYS_uname, "ucontext carries the trapped syscall number");
  check(got == (i64)marker, "the handler's value is what the trapped call returns");
  check(buf_untouched, "a trapped syscall is not executed: uname() did not write its result buffer");

  // 2. A trapped unknown number: executing it would report -ENOSYS, suppressing it returns the marker.
  got = trap_probe(0x1000, 0, 0, 0, 0, 0, 0);
  check(got == (i64)marker, "a trapped unknown syscall is not executed");

  // 3. A syscall from outside the trapped range (this C code) is unaffected.
  u64 real_pid = sc2(SYS_getpid, 0, 0);
  check(real_pid > 0, "a syscall outside the trapped range runs normally");
  u64 ubuf[24];
  for (int i = 0; i < 24; ++i) ubuf[i] = 0;
  check(sc2(SYS_uname, (i64)ubuf, 0) == 0 && ubuf[0] != 0, "an untrapped uname() runs and fills its buffer");

  // 4. Stability.
  int stable = 1;
  for (int i = 0; i < 50; ++i)
    if (trap_probe(0x1000, 0, 0, 0, 0, 0, 0) != (i64)marker) stable = 0;
  check(trap_count == 52 && stable, "50 consecutive trapped calls stay suppressed and stable");

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc2(SYS_exit_group, failures, 0);
}
#if defined(__x86_64__)
__attribute__((naked, used)) void _start(void) { __asm__ volatile("and $-16, %rsp\n call entry_c\n"); }
#else
__asm__(".text\n.globl _start\n_start:\n bl entry_c\n");
#endif
