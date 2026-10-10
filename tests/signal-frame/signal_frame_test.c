// SPDX-License-Identifier: MIT
//
// Regression test for the x86-64 guest signal frame FEX hands to a handler, and for what rt_sigreturn does with it.
// A freestanding static program that only uses raw system calls; the interesting steps are hand-written assembly so no
// compiler register allocation or label arithmetic can get in the way.
//
// The frame is part of the Linux UAPI (arch/x86/include/uapi/asm/sigcontext.h, arch/x86/kernel/signal_64.c), so a guest
// that inspects it, or a runtime that compares it against a real kernel, must see what a Linux kernel writes. Five
// properties are checked:
//
//   1. single-step (TF). Once TF is set, each instruction raises exactly one SIGTRAP before the next instruction
//      executes, and the handler's EFLAGS.TF is authoritative: clearing TF in the ucontext stops single-stepping.
//      POP SS / MOV SS carry an instruction shadow that suppresses the trap for the following instruction.
//      (arch/x86/kernel/traps.c do_debug -> exc_debug_user -> send_sigtrap)
//   2. the frame's CS/SS. Linux writes __USER_CS = 0x33 and __USER_DS = 0x2b for 64-bit code (signal_64.c:123-126,
//      231-233), which is what `mov %cs`/`mov %ss` return, and sigreturn forces CPL3 with `| 0x03` (signal_64.c:81-82).
//   3. the frame's fpstate. FP_XSTATE_MAGIC1 in sw_reserved with extended_size = xstate_size + 4; with XSAVE advertised
//      (CPUID.1:ECX[26]) the fpstate is an XSAVE area with FP_XSTATE_MAGIC2 at fpstate + xstate_size, without it the
//      512-byte FXSAVE area and no MAGIC2.
//      (arch/x86/kernel/fpu/signal.c save_sw_bytes / save_xstate_epilog)
//   4. ud2 reports SIGILL with si_code ILL_ILLOPN = 2, not ILL_ILLOPC. (traps.c handle_invalid_op)
//   5. the frame's uc_sigmask: setup_rt_frame writes sigmask_to_save() and rt_sigreturn installs the sigset_t the
//      handler leaves there. (arch/x86/kernel/signal_64.c x64_setup_rt_frame / rt_sigreturn)
//   6. a signal delivered with RSP not pointing at a live, writable guest stack. The kernel's setup_rt_frame faults
//      copying the frame, takes its Efault path and calls force_sigsegv(SIGSEGV, current) (arch/x86/kernel/signal.c),
//      which forces SIG_DFL and kills the thread with SIGSEGV -- the guest's handler is bypassed. The check is that the
//      process dies from SIGSEGV, not from FEX faulting inside its own host signal handler.
//
// Build: tests/signal-frame/build.sh [clang]   (x86-64 binary, run it under FEX)   or   build.sh native (host architecture)
// Run:   ./signal_frame_test   (exit status = number of failed checks)

typedef unsigned long u64;
typedef long i64;

// ------------------------------------------------------------------------------------------------------
// raw syscalls
// ------------------------------------------------------------------------------------------------------
#if defined(__x86_64__)
static inline i64 sc4(i64 nr, i64 a, i64 b, i64 c, i64 d) {
  i64 ret;
  register i64 r10 __asm__("r10") = d;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
  return ret;
}
enum { SYS_write = 1, SYS_rt_sigaction = 13, SYS_rt_sigprocmask = 14, SYS_tgkill = 234, SYS_getpid = 39, SYS_gettid = 186, SYS_exit_group = 231, SYS_fork = 57, SYS_wait4 = 61 };
enum { GREGS_OFF = 40 };
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
enum { SYS_write = 64, SYS_rt_sigaction = 134, SYS_tgkill = 131, SYS_gettid = 178, SYS_exit_group = 94, SYS_waitid = 95 };
enum { GREGS_OFF = 184 };
struct ksigaction { void* handler; u64 flags; u64 mask; };
#define MAKE_ACTION(h) { (void*)(h), SA_SIGINFO, 0 }
#else
#error "x86-64 and aarch64 only"
#endif
#define SA_SIGINFO 0x4
#define SA_RESTORER 0x04000000
enum { SIGTRAP = 5, SIGILL = 4, SIGSEGV = 11, SIGUSR1 = 10, SIGUSR2 = 12 };

// ------------------------------------------------------------------------------------------------------
// helpers (no libc)
// ------------------------------------------------------------------------------------------------------
static u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sc4(SYS_write, 1, (i64)s, slen(s), 0); }
static int failures;
static void check(int ok, const char* what) {
  out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) ++failures;
}

// ------------------------------------------------------------------------------------------------------
// data owned by the assembly
// ------------------------------------------------------------------------------------------------------
extern u64 g_n;           // traps seen
extern u64 g_first_rip;   // RIP of the first trap
extern u64 g_last_rip;    // RIP of the last trap
extern u64 g_second_rip;  // RIP of the second trap
extern u64 g_efl_first;   // EFLAGS in the first trap frame
extern u64 g_clear_tf;    // non-zero: the handler clears TF in the frame
extern u64 g_kind;        // which handler ran
extern u64 g_segv;        // a SIGSEGV arrived
extern u64 g_resume;      // set by a trampoline after it returns
extern u64 g_saved_sp;    // RSP saved before the bad-SP test
extern u64 g_cs, g_ss;    // live `mov %cs`, `mov %ss`
extern u64 g_csgsfs;      // the frame's CSGSFS
extern u64 g_fp;          // the frame's fpregs pointer
extern u64 g_ucmask;      // the frame's uc_sigmask (low 64 bits)
extern u64 g_setmask;     // non-zero: the handler installs this into the frame's uc_sigmask
extern u64 g_sigo[5];     // si_signo, si_code, ...
extern u64 g_gregs[22];   // a copy of the ucontext gregs
extern u64 g_gregs_off;
extern void htrap(void);
extern void hstep(void);
extern void hud2(void);
extern void hsegv(void);
extern void do_trap(void);
extern void do_ud2(void);
extern void do_step(void);
extern void step_nop(void);
extern void step_after(void);
extern void do_badsp(void);
extern void badsp_after(void);

#if defined(__x86_64__)
__asm__(".section .rwx,\"awx\",@progbits\n.balign 16\n"
        // Entry: rdi=signal, rsi=siginfo*, rdx=ucontext*.
        ".globl htrap\nhtrap:\n"
        "  movq $1, g_kind(%rip)\n  jmp collect\n"
        ".balign 16\n.globl hstep\nhstep:\n"
        "  movq $1, g_kind(%rip)\n  jmp collect\n"
        ".balign 16\n.globl hud2\nhud2:\n"
        "  movq $3, g_kind(%rip)\n  jmp collect\n"
        ".balign 16\n.globl hsegv\nhsegv:\n"
        "  movq $5, g_kind(%rip)\n"
        "  movq $1, g_segv(%rip)\n"
        "  incq g_n(%rip)\n"
        // Resume the bad-SP trampoline with the stack it started from.
        "  lea badsp_after(%rip), %rax\n  mov %rax, 168(%rdx)\n"
        "  mov g_saved_sp(%rip), %rax\n  mov %rax, 160(%rdx)\n"
        "  ret\n"
        ".balign 16\ncollect:\n"
        "  push %rax\n push %rcx\n push %rsi\n push %rdi\n"
        "  incq g_n(%rip)\n"
        "  mov 168(%rdx), %rax\n"
        "  cmpq $1, g_n(%rip)\n"
        "  jne 0f\n"
        "  mov %rax, g_first_rip(%rip)\n"
        "  mov 176(%rdx), %rax\n  mov %rax, g_efl_first(%rip)\n"
        "  mov 168(%rdx), %rax\n"
        "0:\n  cmpq $2, g_n(%rip)\n"
        "  jne 1f\n"
        "  mov %rax, g_second_rip(%rip)\n"
        "1:\n  mov %rax, g_last_rip(%rip)\n"
        // si_signo / si_code
        "  mov (%rsi), %rax\n  mov %rax, g_sigo(%rip)\n"
        "  mov 8(%rsi), %rax\n  mov %rax, g_sigo+8(%rip)\n"
        // ucontext gregs (22 qwords)
        "  mov g_gregs_off(%rip), %rcx\n"
        "  lea (%rdx, %rcx), %rsi\n"
        "  lea g_gregs(%rip), %rdi\n"
        "  mov $22, %ecx\n"
        "2:\n  mov (%rsi), %rax\n  mov %rax, (%rdi)\n  add $8, %rsi\n  add $8, %rdi\n  dec %rcx\n  jnz 2b\n"
        // frame CSGSFS and fpstate
        "  mov 184(%rdx), %rax\n  mov %rax, g_csgsfs(%rip)\n"
        "  mov 224(%rdx), %rax\n  mov %rax, g_fp(%rip)\n"       // mcontext.fpregs (40 + 23*8)
        "  mov 296(%rdx), %rax\n  mov %rax, g_ucmask(%rip)\n"   // ucontext.uc_sigmask (40 + 256 + 0)
        // live mov %cs, %ss
        "  xor %eax, %eax\n  mov %cs, %ax\n  mov %rax, g_cs(%rip)\n"
        "  xor %eax, %eax\n  mov %ss, %ax\n  mov %rax, g_ss(%rip)\n"
        // install g_setmask into the frame's uc_sigmask when asked to (rt_sigreturn must honour it)
        "  cmpq $0, g_setmask(%rip)\n  je 6f\n"
        "  mov g_setmask(%rip), %rax\n  mov %rax, 296(%rdx)\n"
        "6:\n"
        // clear TF in the frame when asked to (used to take over from a single-step)
        "  cmpq $0, g_clear_tf(%rip)\n  je 3f\n"
        "  andq $~0x100, 176(%rdx)\n"
        "3:\n"
        // #UD resumes at the faulting instruction, so step over the 2-byte ud2 (0f 0b)
        "  cmpq $3, g_kind(%rip)\n  jne 5f\n"
        "  addq $2, 168(%rdx)\n"
        "5:\n"
        // storm guard: after 8 traps, clear TF in the frame so a broken continuation cannot loop forever.
        "  cmpq $8, g_n(%rip)\n  jle 4f\n"
        "  andq $~0x100, 176(%rdx)\n"
        "4:\n"
        "  pop %rdi\n pop %rsi\n pop %rcx\n pop %rax\n"
        "  ret\n"
        // trampolines
        ".balign 16\n.globl do_trap\ndo_trap:\n"
        "  int $3\n"
        "  movq $1, g_resume(%rip)\n  ret\n"
        ".balign 16\n.globl do_ud2\ndo_ud2:\n"
        "  .byte 0x0f, 0x0b\n"                       // ud2
        "  movq $1, g_resume(%rip)\n  ret\n"
        ".balign 16\n.globl do_step\ndo_step:\n"
        "  pushfq\n  orq $0x100, (%rsp)\n  popfq\n"   // set TF
        ".globl step_nop\nstep_nop:\n"
        "  nop\n"
        ".globl step_after\nstep_after:\n"
        "  movq $1, g_resume(%rip)\n  ret\n"
        // int3 with RSP parked on a non-canonical, unmapped address: the signal frame cannot be written.
        ".balign 16\n.globl do_badsp\ndo_badsp:\n"
        "  mov %rsp, g_saved_sp(%rip)\n"
        "  movabs $0x8000000000000000, %rsp\n"
        "  int $3\n"
        ".globl badsp_after\nbadsp_after:\n"
        "  movq $1, g_resume(%rip)\n"
        "  mov g_saved_sp(%rip), %rsp\n"
        "  ret\n"
        ".section .text\n");
#else
__asm__(".section .rwx,\"awx\",@progbits\n.balign 16\n"
        ".globl htrap\nhtrap:\n  ret\n"
        ".globl hstep\nhstep:\n  ret\n"
        ".globl hud2\nhud2:\n  ret\n"
        ".globl hsegv\nhsegv:\n  ret\n"
        ".globl do_trap\ndo_trap:\n  ret\n"
        ".globl do_ud2\ndo_ud2:\n  ret\n"
        ".globl do_step\ndo_step:\n  ret\n"
        ".globl step_nop\nstep_nop:\n  ret\n"
        ".globl step_after\nstep_after:\n  ret\n"
        ".globl do_badsp\ndo_badsp:\n  ret\n"
        ".globl badsp_after\nbadsp_after:\n  ret\n"
        ".section .text\n");
#endif

// Data, in its own tiny section so both the C and the asm halves can reach it.
#if defined(__x86_64__)
__asm__(".section .rwx,\"awx\",@progbits\n.balign 8\n"
        ".globl g_n\ng_n: .quad 0\n"
        ".globl g_first_rip\ng_first_rip: .quad 0\n"
        ".globl g_last_rip\ng_last_rip: .quad 0\n.globl g_second_rip\ng_second_rip: .quad 0\n"
        ".globl g_efl_first\ng_efl_first: .quad 0\n"
        ".globl g_clear_tf\ng_clear_tf: .quad 0\n"
        ".globl g_kind\ng_kind: .quad 0\n"
        ".globl g_segv\ng_segv: .quad 0\n"
        ".globl g_resume\ng_resume: .quad 0\n"
        ".globl g_saved_sp\ng_saved_sp: .quad 0\n"
        ".globl g_csgsfs\ng_csgsfs: .quad 0\n"
        ".globl g_fp\ng_fp: .quad 0\n"
        ".globl g_ucmask\ng_ucmask: .quad 0\n"
        ".globl g_setmask\ng_setmask: .quad 0\n"
        ".globl g_sigo\ng_sigo: .zero 40\n"
        ".globl g_gregs\ng_gregs: .zero 22*8\n"
        ".globl g_cs\ng_cs: .quad 0\n"
        ".globl g_ss\ng_ss: .quad 0\n"
        ".globl g_gregs_off\ng_gregs_off: .quad 40\n"
        ".section .text\n");
#else
__asm__(".section .rwx,\"awx\",@progbits\n.balign 8\n"
        ".globl g_n\ng_n: .quad 0\n"
        ".globl g_first_rip\ng_first_rip: .quad 0\n"
        ".globl g_last_rip\ng_last_rip: .quad 0\n.globl g_second_rip\ng_second_rip: .quad 0\n"
        ".globl g_efl_first\ng_efl_first: .quad 0\n"
        ".globl g_clear_tf\ng_clear_tf: .quad 0\n"
        ".globl g_kind\ng_kind: .quad 0\n"
        ".globl g_segv\ng_segv: .quad 0\n"
        ".globl g_resume\ng_resume: .quad 0\n"
        ".globl g_saved_sp\ng_saved_sp: .quad 0\n"
        ".globl g_csgsfs\ng_csgsfs: .quad 0\n"
        ".globl g_fp\ng_fp: .quad 0\n"
        ".globl g_sigo\ng_sigo: .zero 40\n"
        ".globl g_gregs\ng_gregs: .zero 22*8\n"
        ".globl g_cs\ng_cs: .quad 0\n"
        ".globl g_ss\ng_ss: .quad 0\n"
        ".globl g_gregs_off\ng_gregs_off: .quad 184\n"
        ".section .text\n");
#endif

#define SI_SIGNO() (g_sigo[0] & 0xffffffffUL)
#define SI_CODE()  (g_sigo[1] & 0xffffffffUL)
#define G(slot)    (g_gregs[(slot)])
#define FRAME_CS() (g_csgsfs & 0xffffUL)
#define FRAME_SS() ((g_csgsfs >> 48) & 0xffffUL)

static void install(int sig, void (*h)(void)) {
  struct ksigaction a = MAKE_ACTION(h);
  sc4(SYS_rt_sigaction, sig, (i64)&a, 0, 8);
}

#if defined(__x86_64__)
// Signal-mask helpers (Linux rt_sigprocmask/rt_sigreturn).
enum { SIG_BLOCK = 0, SIG_SETMASK = 2 };
#define BIT(s) (1UL << ((s) - 1))
static u64 getmask(void) { u64 m = 0; sc4(SYS_rt_sigprocmask, SIG_BLOCK, 0, (i64)&m, 8); return m; }
static void setmask(u64 set) { sc4(SYS_rt_sigprocmask, SIG_SETMASK, (i64)&set, 0, 8); }
static void raise_self(int sig) { sc4(SYS_tgkill, sc4(SYS_getpid, 0, 0, 0, 0), sc4(SYS_gettid, 0, 0, 0, 0), sig, 0); }
#endif

static void reset(void) {
  g_n = 0; g_first_rip = 0; g_last_rip = 0; g_efl_first = 0; g_clear_tf = 0; g_kind = 0; g_segv = 0; g_resume = 0;
  g_cs = 0; g_ss = 0; g_csgsfs = 0; g_fp = 0; g_ucmask = 0; g_setmask = 0;
  for (int i = 0; i < 22; ++i) g_gregs[i] = 0;
  for (int i = 0; i < 5; ++i) g_sigo[i] = 0;
}

__attribute__((used)) static void entry_c(void) {
  out("signal-frame test\n");

  install(SIGTRAP, htrap);
  install(SIGILL, hud2);
  install(SIGSEGV, hsegv);

  // ------------------------------------------------------------------------------------------------
  // 1. single-step (TF)
  // ------------------------------------------------------------------------------------------------
  // (a) A signal delivered while TF is set: the handler's frame has TF, and Linux delivers exactly one TRAP_TRACE
  //     before the following instruction runs.
  reset();
  do_step();                                    // set TF, nop, return
  check(g_n >= 1 && SI_SIGNO() == SIGTRAP, "TF: a single-step SIGTRAP is delivered");
  check((g_efl_first & 0x100) != 0, "TF: the trap frame's EFLAGS has TF set (bit 8)");
  check(SI_CODE() == 2, "TF: si_code is TRAP_TRACE (2)");
  check(g_first_rip == (u64)step_after, "TF: the trap's RIP is the instruction after the one single-stepped");

#if defined(__x86_64__)
  // (b) A handler that clears TF in its frame takes over from the single-step: no second trap, so no trap storm. The
  //     handler's storm guard (clear TF after 8 traps) is the backstop if the continuation is broken.
  reset();
  g_clear_tf = 1;
  do_step();
  g_clear_tf = 0;
  check(g_n == 1, "TF: a handler that clears TF in its ucontext stops single-stepping (no second trap)");
#endif

  // ------------------------------------------------------------------------------------------------
  // 2. frame CS/SS
  // ------------------------------------------------------------------------------------------------
#if defined(__x86_64__)
  reset();
  do_trap();                                    // int3

  check(g_cs == 0x33, "live `mov %cs` is 0x33 (__USER_CS)");
  check(g_ss == 0x2b, "live `mov %ss` is 0x2b (__USER_DS)");
  check(FRAME_CS() == 0x33, "the frame's CS is 0x33");
  check(FRAME_SS() == 0x2b, "the frame's SS is 0x2b");
  check(g_cs == FRAME_CS(), "the frame's CS matches the live CS");
  check(g_ss == FRAME_SS(), "the frame's SS matches the live SS");
#endif

  // ------------------------------------------------------------------------------------------------
  // 3. frame fpstate
  // ------------------------------------------------------------------------------------------------
#if defined(__x86_64__)
  reset();
  do_trap();
  {
    u64 fp = g_fp;
    check(fp != 0, "fpstate: the frame has an fpregs pointer");
    if (fp) {
      // fpx_sw_bytes: magic1(0), extended_size(4), xfeatures(8), xstate_size(16) -- all relative to sw_reserved at +464.
      u64 magic1 = *(volatile unsigned*)(fp + 464);
      u64 extended_size = *(volatile unsigned*)(fp + 468);
      u64 xstate_size = *(volatile unsigned*)(fp + 480);
      check(magic1 == 0x46505853, "fpstate: FP_XSTATE_MAGIC1 is present in sw_reserved (+464)");
      // The kernel sets extended_size = user_size + 4 on every frame (save_sw_bytes).
      check(extended_size == xstate_size + 4, "fpstate: extended_size is xstate_size + 4");
      unsigned c1_ecx;
      __asm__ volatile("cpuid" : "=c"(c1_ecx) : "a"(1), "c"(0) : "ebx", "edx");
      if (c1_ecx & (1u << 26)) {
        // XSAVE: FP_XSTATE_MAGIC2 follows the user state, at fpstate + user_size (save_xstate_epilog).
        u64 magic2 = 0;
        if (xstate_size >= 512 && xstate_size <= 16384) magic2 = *(volatile unsigned*)(fp + xstate_size);
        check(magic2 == 0x46505845, "fpstate: FP_XSTATE_MAGIC2 is present at fpstate + xstate_size");
      } else {
        // No XSAVE (FEX: FEX_HOSTFEATURES=disableavx): the 512-byte FXSAVE frame, no xsave header and no MAGIC2
        // (save_xstate_epilog returns before writing them). sigreturn from this frame must still work.
        check(xstate_size == 512, "fpstate: without XSAVE, xstate_size is the 512-byte FXSAVE area");
      }
    }
  }
#endif

  // ------------------------------------------------------------------------------------------------
  // 4. ud2 si_code
  // ------------------------------------------------------------------------------------------------
  out("case4\n");
  reset();
  do_ud2();
  check(SI_SIGNO() == SIGILL, "ud2: signo is SIGILL");
  // Linux handle_invalid_op() reports ILL_ILLOPN (2) for #UD (traps.c); ILL_ILLOPC (1) is only the FPU/XFD path.
  check(SI_CODE() == 2, "ud2: si_code is ILL_ILLOPN (2), as Linux reports");

  // ------------------------------------------------------------------------------------------------
  // 4b. the frame's uc_sigmask
  // ------------------------------------------------------------------------------------------------
#if defined(__x86_64__)
  {
    // Linux setup_rt_frame writes sigmask_to_save() into frame->uc.uc_sigmask (signal_64.c:166,190) and rt_sigreturn
    // installs whatever sigset_t the handler leaves there (signal_64.c:256 -> set_current_blocked). So a handler must
    // see the interrupted mask, and sigreturn must put it back.
    u64 saved = getmask();
    setmask(BIT(SIGUSR2));
    reset();
    do_trap();                                    // int3 -> SIGTRAP handler -> rt_sigreturn
    check(g_ucmask == BIT(SIGUSR2), "uc_sigmask: the frame carries the interrupted mask (SIGUSR2)");
    check(getmask() == BIT(SIGUSR2), "uc_sigmask: rt_sigreturn restores the interrupted mask");
    setmask(saved);

    // A handler that rewrites uc_sigmask has the rewritten mask installed by rt_sigreturn.
    setmask(0);
    reset();
    g_setmask = BIT(SIGUSR1);
    do_trap();
    g_setmask = 0;
    check(getmask() == BIT(SIGUSR1), "uc_sigmask: rt_sigreturn installs the handler's rewritten mask");
    setmask(0);
  }
#endif

  // ------------------------------------------------------------------------------------------------
  // 5. a signal delivered with RSP not on a live guest stack
  // ------------------------------------------------------------------------------------------------
#if defined(__x86_64__)
  out("case5\n");
  {
    // int3 with RSP parked on a non-canonical, unmapped address: the frame cannot be written there. Run it in a child
    // so that FEX dying inside its own host signal handler is reported rather than taking the whole test down.
    //
    // Linux: setup_rt_frame's copy to the guest frame faults, signal_64.c takes the Efault path and calls
    // force_sigsegv(SIGSEGV, current) (arch/x86/kernel/signal.c). That forces the disposition to SIG_DFL and kills the
    // thread with SIGSEGV -- whatever handler the guest installed is bypassed, so the process is terminated by signal
    // 11, not resumed (measured: wait status 139 on a bare-metal x86-64 kernel). FEX instead performed the raw store
    // and faulted inside its own host signal handler, in unrelated host memory; it must detect the bad SP and end the
    // guest the same way.
    i64 pid = sc4(SYS_fork, 0, 0, 0, 0);
    if (pid == 0) {
      reset();
      do_badsp();
      sc4(SYS_exit_group, 1, 0, 0, 0);
      __builtin_unreachable();
    }
    int status = -1;
    sc4(SYS_wait4, pid, (i64)&status, 0, 0);
    check(pid > 0 && (status & 0x7f) == SIGSEGV, "bad SP: the process is terminated by SIGSEGV, as Linux does");
  }
#endif

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc4(SYS_exit_group, failures, 0, 0, 0);
}
#if defined(__x86_64__)
__attribute__((naked, used)) void _start(void) { __asm__ volatile("and $-16, %rsp\n call entry_c\n"); }
#else
__asm__(".text\n.globl _start\n_start:\n bl entry_c\n");
#endif
