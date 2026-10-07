// SPDX-License-Identifier: MIT
//
// Synthetic regression test for the ptrace emulation FEX needs to run injector-style launchers.
//
// It reproduces, with a freestanding static x86-64 program and nothing from any game or anti-cheat vendor, the
// ptrace conversation the Easy Anti-Cheat Linux launcher has with the process it injects its client into:
//
//   child : PTRACE_TRACEME, SIGSTOP itself, copy its own image to a file, execve() it
//   tracer: wait for the SIGSTOP, PTRACE_SETOPTIONS(TRACESYSGOOD|TRACEEXEC), step syscalls with PTRACE_SYSCALL,
//           read/modify registers (GETREGSET/SETREGSET NT_PRSTATUS, PEEKUSER, POKEUSER, GETREGS/SETREGS),
//           catch PTRACE_EVENT_EXEC and readlink(/proc/<pid>/exe), plant an int3 with POKETEXT in code that was
//           already executed (the translator must notice), call a function inside the tracee by rewriting
//           registers with a fake return address of 9 (SIGSEGV at rip=9), restore registers, PTRACE_DETACH.
//
// The same binary is both roles (argv[0] / environment select it). It is written to pass on native x86-64 Linux (not yet run there) and must pass under
// FEX; every check prints PASS/FAIL and the exit status is the number of failures.
//
// Register access is only emulated at the stops FEX itself produces (syscall entry/exit, guest int3/SIGSEGV), which is
// what injectors use; a real SIGSTOP (kill(self, SIGSTOP)) is only used here as a place to poke memory.
//
// Build:  see tests/ptrace-inject/build.sh

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef unsigned char u8;

// ------------------------------------------------------------------------------------------------------
// raw syscalls
// ------------------------------------------------------------------------------------------------------
static inline i64 sc6(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  i64 ret;
  register i64 r10 __asm__("r10") = d;
  register i64 r8 __asm__("r8") = e;
  register i64 r9 __asm__("r9") = f;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return ret;
}
#define sc1(n, a) sc6(n, (i64)(a), 0, 0, 0, 0, 0)
#define sc2(n, a, b) sc6(n, (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sc3(n, a, b, c) sc6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sc4(n, a, b, c, d) sc6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), 0, 0)

enum {
  SYS_read = 0, SYS_write = 1, SYS_open = 2, SYS_close = 3, SYS_getpid = 39, SYS_fork = 57, SYS_execve = 59, SYS_wait4 = 61,
  SYS_kill = 62, SYS_readlink = 89, SYS_chmod = 90, SYS_getuid = 102, SYS_ptrace = 101, SYS_getppid = 110, SYS_unlink = 87,
  SYS_exit_group = 231,
};
enum {
  PT_TRACEME = 0, PT_PEEKTEXT = 1, PT_POKETEXT = 4, PT_POKEUSER = 6, PT_PEEKUSER = 3, PT_CONT = 7, PT_GETREGS = 12, PT_SETREGS = 13,
  PT_DETACH = 17, PT_SYSCALL = 24, PT_SETOPTIONS = 0x4200, PT_GETREGSET = 0x4204, PT_SETREGSET = 0x4205,
};
enum { SIGTRAP_ = 5, SIGSEGV_ = 11, SIGSTOP_ = 19 };
enum { NT_PRSTATUS_ = 1 };
enum {
  R15, R14, R13, R12, RBP, RBX, R11, R10, R9, R8, RAX, RCX, RDX, RSI, RDI, ORIG_RAX, RIP, CS, EFLAGS, RSP, SS, FS_BASE, GS_BASE, DS, ES, FS, GS, NREGS
};

struct iov { void* base; u64 len; };

// ------------------------------------------------------------------------------------------------------
// tiny helpers (no libc)
// ------------------------------------------------------------------------------------------------------
static u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sc3(SYS_write, 1, s, slen(s)); }
static void outhex(u64 v) {
  char b[19]; b[0] = '0'; b[1] = 'x'; int i;
  for (i = 0; i < 16; ++i) { int d = (v >> (60 - 4 * i)) & 15; b[2 + i] = d < 10 ? '0' + d : 'a' + d - 10; }
  b[18] = 0; out(b);
}
static int seq(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static int ends_with(const char* s, const char* suf) {
  u64 n = slen(s), m = slen(suf); return n >= m && seq(s + n - m, suf);
}
static void ucopy(char* d, const char* s) { while ((*d++ = *s++)) {} }
static void u64dec(char* d, u64 v) {
  char t[24]; int n = 0; if (!v) t[n++] = '0'; while (v) { t[n++] = '0' + v % 10; v /= 10; }
  while (n) *d++ = t[--n]; *d = 0;
}

static int failures;
static void check(int ok, const char* what) {
  out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) ++failures;
}

// ------------------------------------------------------------------------------------------------------
// code the tracer pokes at / calls inside the tracee. noinline + fixed addresses (static, non-PIE binary).
// ------------------------------------------------------------------------------------------------------
__attribute__((noinline, used)) u64 breakpoint_target(u64 x) { __asm__ volatile("" ::: "memory"); return x + 1; }
__attribute__((noinline, used)) u64 injected_call(u64 x) { __asm__ volatile("" ::: "memory"); return x * 2 + 0; }

static i64 ptrace(i64 req, i64 pid, i64 addr, i64 data) { return sc4(SYS_ptrace, req, pid, addr, data); }
static i64 peek(i64 pid, u64 addr) { u64 w = 0; i64 r = sc4(SYS_ptrace, PT_PEEKTEXT, pid, addr, &w); return r < 0 ? 0 : (i64)w; }

static int get_regs(i64 pid, u64* regs) {
  struct iov v = {regs, NREGS * 8};
  return ptrace(PT_GETREGSET, pid, NT_PRSTATUS_, (i64)&v) == 0 && v.len == NREGS * 8;
}
static int set_regs(i64 pid, u64* regs) {
  struct iov v = {regs, NREGS * 8};
  return ptrace(PT_SETREGSET, pid, NT_PRSTATUS_, (i64)&v) == 0;
}
static int wait_status(i64 pid, int* st) { return sc4(SYS_wait4, pid, st, 0x40000000 /* __WALL */, 0) == pid; }
#define STOPPED(st) (((st) & 0xff) == 0x7f)
#define STOPSIG(st) (((st) >> 8) & 0xff)
#define EVENT(st) ((st) >> 16)

// ------------------------------------------------------------------------------------------------------
// payload role: runs after the tracer made us execve() ourselves
// ------------------------------------------------------------------------------------------------------
static void payload(void) {
  // 1. syscalls the tracer inspects and rewrites
  i64 pid = sc1(SYS_getpid, 0);
  i64 ppid_forged = sc1(SYS_getpid, 0);   // tracer rewrites this entry into getppid()
  i64 uid_forged = sc1(SYS_getuid, 0);    // tracer overwrites the result at the exit stop
  // 2. run the breakpoint target once so it is translated/cached, then park so the tracer can plant an int3 there
  u64 r1 = breakpoint_target(10);
  sc2(SYS_kill, pid, SIGSTOP_);
  u64 r2 = breakpoint_target(20);          // tracer put an int3 here: SIGTRAP stop, rip fixed up by the tracer
  __asm__ volatile("int3");                 // tracer calls injected_call() with a fake return address of 9, then resumes us here
  // 3. report what we saw through the exit status (the tracer cross-checks via PEEK/regs as well)
  int ok = r1 == 11 && r2 == 21 && ppid_forged != pid && uid_forged == 0x1234;
  sc1(SYS_exit_group, ok ? 7 : 40);
}

// ------------------------------------------------------------------------------------------------------
// tracer role
// ------------------------------------------------------------------------------------------------------
static char payload_path[64];

static int copy_self_and_exec(void) {
  // child: become a tracee, stop, copy /proc/self/exe to a private file, exec it as the payload role
  ptrace(PT_TRACEME, 0, 0, 0);
  sc2(SYS_kill, sc1(SYS_getpid, 0), SIGSTOP_);
  char src[] = "/proc/self/exe";
  i64 in = sc2(SYS_open, src, 0);
  i64 outfd = sc3(SYS_open, payload_path, 0101 /* O_WRONLY|O_CREAT */ | 01000 /* O_TRUNC */, 0755);
  if (in < 0 || outfd < 0) sc1(SYS_exit_group, 90);
  char buf[4096]; i64 n;
  while ((n = sc3(SYS_read, in, buf, sizeof buf)) > 0) sc3(SYS_write, outfd, buf, n);
  sc1(SYS_close, in); sc1(SYS_close, outfd);
  sc3(SYS_chmod, payload_path, 0755, 0);
  char* argv[] = {payload_path, 0};
  char* envp[] = {"FEXPT_ROLE=payload", 0};
  sc3(SYS_execve, payload_path, argv, envp);
  sc1(SYS_exit_group, 91);
  return 0;
}

static void tracer(void) {
  char num[24]; u64dec(num, (u64)sc1(SYS_getpid, 0));
  ucopy(payload_path, "/tmp/fexpt-payload-"); ucopy(payload_path + slen(payload_path), num);

  i64 child = sc1(SYS_fork, 0);
  if (child == 0) { copy_self_and_exec(); sc1(SYS_exit_group, 92); }
  check(child > 0, "fork");

  int st = 0;
  check(wait_status(child, &st) && STOPPED(st) && STOPSIG(st) == SIGSTOP_, "initial SIGSTOP of the PTRACE_TRACEME child");
  check(ptrace(PT_SETOPTIONS, child, 0, 0x11 /* TRACESYSGOOD | TRACEEXEC */) == 0, "PTRACE_SETOPTIONS");

  // ---- step syscalls until the exec event, checking entry/exit alternation and register contents
  int entries = 0, exits = 0, expect_entry = 1, saw_exec = 0, saw_open_self = 0, regs_ok = 1, alt_ok = 1;
  u64 regs[NREGS];
  for (int i = 0; i < 4000 && !saw_exec; ++i) {
    if (ptrace(PT_SYSCALL, child, 0, 0) != 0) break;
    if (!wait_status(child, &st) || !STOPPED(st)) break;
    if (EVENT(st) == 4) { saw_exec = 1; break; }
    if (STOPSIG(st) != (SIGTRAP_ | 0x80)) continue;   // signal-delivery stop: ignore here
    if (!get_regs(child, regs)) { regs_ok = 0; continue; }
    if (expect_entry) {
      ++entries;
      if (regs[RAX] != (u64)-38) alt_ok = 0;         // -ENOSYS at syscall entry
      if (regs[ORIG_RAX] == SYS_open && !saw_open_self) saw_open_self = 1;
      if (regs[CS] != 0x33 || regs[SS] != 0x2b) regs_ok = 0;
    } else {
      ++exits;
    }
    expect_entry = !expect_entry;
  }
  check(entries > 5 && (entries == exits || entries == exits + 1), "syscall stops alternate entry/exit");
  check(alt_ok, "rax is -ENOSYS at syscall entry");
  check(regs_ok, "NT_PRSTATUS segment registers are the x86-64 user selectors");
  check(saw_open_self, "orig_rax carries x86-64 syscall numbers (saw open)");
  check(saw_exec, "PTRACE_EVENT_EXEC stop");

  // ---- /proc/<pid>/exe must name the guest binary that was exec()ed, not the emulator
  char exe[256]; char link[40] = "/proc/"; u64dec(link + 6, (u64)child); ucopy(link + slen(link), "/exe");
  i64 n = sc3(SYS_readlink, link, exe, sizeof(exe) - 1); if (n > 0) exe[n] = 0; else exe[0] = 0;
  check(n > 0 && seq(exe, payload_path), "readlink(/proc/<pid>/exe) names the exec()ed image");

  // ---- the payload starts: inspect and rewrite syscalls
  int did_getpid_rewrite = 0, did_uid_forge = 0, getpid_exit_ok = 0, ppid_exit_ok = 0, uid_seen = 0;
  u64 tracer_pid = (u64)sc1(SYS_getpid, 0);
  int first_getpid_seen = 0;
  for (int i = 0; i < 200 && !(did_getpid_rewrite && did_uid_forge); ++i) {
    if (ptrace(PT_SYSCALL, child, 0, 0) != 0) break;
    if (!wait_status(child, &st) || !STOPPED(st)) break;
    if (STOPSIG(st) != (SIGTRAP_ | 0x80)) continue;
    if (!get_regs(child, regs)) continue;
    u64 nr = regs[ORIG_RAX];
    if (regs[RAX] == (u64)-38) {                      // entry
      if (nr == SYS_getpid && first_getpid_seen == 1 && !did_getpid_rewrite) {
        regs[ORIG_RAX] = SYS_getppid; regs[RAX] = SYS_getppid;       // turn the 2nd getpid() into getppid()
        did_getpid_rewrite = set_regs(child, regs);
        first_getpid_seen = 2;
      } else if (nr == SYS_getpid && !first_getpid_seen) {
        first_getpid_seen = 1;
      } else if (nr == SYS_getuid) { uid_seen = 1; }
    } else {                                           // exit
      if (nr == SYS_getpid && first_getpid_seen == 1 && !getpid_exit_ok) getpid_exit_ok = regs[RAX] == (u64)child;
      else if ((nr == SYS_getppid || nr == SYS_getpid) && first_getpid_seen == 2 && !ppid_exit_ok) ppid_exit_ok = regs[RAX] == tracer_pid;
      if (nr == SYS_getuid && uid_seen) { regs[RAX] = 0x1234; did_uid_forge = set_regs(child, regs); }
    }
  }
  check(getpid_exit_ok, "syscall exit stop returns the real result (getpid == child pid)");
  check(did_getpid_rewrite, "SETREGSET at entry accepted (getpid -> getppid)");
  check(ppid_exit_ok, "rewriting orig_rax/rax at syscall entry changes the syscall that runs");
  check(did_uid_forge, "SETREGSET at exit stop accepted (forged rax)");

  // ---- breakpoint injection into already-translated code
  u64 bp = (u64)&breakpoint_target;
  i64 orig = peek(child, bp);
  i64 patched = (orig & ~0xffLL) | 0xcc;
  // keep going until the payload stops itself (SIGSTOP after the first breakpoint_target call)
  int got_stop = 0;
  for (int i = 0; i < 100 && !got_stop; ++i) {
    if (ptrace(PT_SYSCALL, child, 0, 0) != 0) break;
    if (!wait_status(child, &st) || !STOPPED(st)) break;
    if (STOPSIG(st) == SIGSTOP_) got_stop = 1;
  }
  check(got_stop, "payload parked itself with SIGSTOP");
  check(ptrace(PT_POKETEXT, child, bp, patched) == 0, "POKETEXT int3 into code that was already executed");
  // Resume from the SIGSTOP signal-delivery stop (suppressing it); the payload now calls breakpoint_target() again
  // and must hit the int3 even though that function was already translated/cached.
  int got_trap = 0;
  if (ptrace(PT_CONT, child, 0, 0) == 0 && wait_status(child, &st) && STOPPED(st) && STOPSIG(st) == SIGTRAP_ && EVENT(st) == 0) got_trap = 1;
  if (!got_trap) { out("  (tracer saw wait status "); outhex((u64)st); out(")\n"); }
  check(got_trap, "int3 planted with POKETEXT raises SIGTRAP in the tracee (translated code was invalidated)");
  if (got_trap && get_regs(child, regs)) {
    check(regs[RIP] == bp + 1, "rip is the address after the int3");
    check(ptrace(PT_POKETEXT, child, bp, orig) == 0, "restore the original bytes");
    regs[RIP] = bp;
    check(set_regs(child, regs), "SETREGSET rip back to the breakpoint address");
  }

  // ---- call a function in the tracee through registers (the way a launcher calls dlopen): fake return address 9
  int got_stop2 = 0;
  for (int i = 0; i < 10 && !got_stop2; ++i) {
    if (ptrace(PT_CONT, child, 0, 0) != 0) break;
    if (!wait_status(child, &st) || !STOPPED(st)) break;
    if (STOPSIG(st) == SIGTRAP_ && EVENT(st) == 0) got_stop2 = 1;
  }
  check(got_stop2, "payload parked again (int3) after the second breakpoint_target call");
  u64 saved[NREGS];
  if (got_stop2 && get_regs(child, saved)) {
    u64 call[NREGS]; for (int i = 0; i < NREGS; ++i) call[i] = saved[i];
    u64 sp = ((saved[RSP] - 256) & ~0xfULL) - 8;                    // misaligned like after a call
    check(ptrace(PT_POKETEXT, child, sp, 9) == 0, "write fake return address 9 on the tracee stack");
    call[RSP] = sp; call[RIP] = (u64)&injected_call; call[RDI] = 21; call[ORIG_RAX] = (u64)-1;
    check(set_regs(child, call), "SETREGSET to call a function in the tracee");
    int got_segv = 0;
    ptrace(PT_CONT, child, 0, 0);
    if (wait_status(child, &st) && STOPPED(st) && STOPSIG(st) == SIGSEGV_) got_segv = 1;
    if (!got_segv) { out("  (tracer saw wait status "); outhex((u64)st); out(")\n"); }
    check(got_segv, "returning to address 9 raises SIGSEGV");
    u64 after[NREGS];
    if (got_segv && get_regs(child, after)) {
      check(after[RIP] == 9, "rip == 9 at the SIGSEGV stop");
      check(after[RAX] == 42, "the injected function ran and returned 42");
    }
    // restore the original register state and let the payload continue; the SIGSEGV must be suppressed
    check(set_regs(child, saved), "restore the saved registers");
    check(ptrace(PT_DETACH, child, 0, 0) == 0, "PTRACE_DETACH");
  }

  // ---- the payload finishes by itself
  int status = 0;
  i64 w = sc4(SYS_wait4, child, &status, 0x40000000, 0);
  check(w == child && (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 7, "payload exits 7 (all in-tracee checks passed)");
  sc1(SYS_unlink, payload_path);

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc1(SYS_exit_group, failures);
}

// ------------------------------------------------------------------------------------------------------
// entry point: the role comes from the environment (the payload is exec()ed with FEXPT_ROLE=payload)
// ------------------------------------------------------------------------------------------------------
__attribute__((used)) static void entry_c(u64* sp) {
  u64 argc = sp[0]; char** argv = (char**)(sp + 1); char** envp = argv + argc + 1;
  (void)argv;
  for (char** e = envp; *e; ++e) {
    if (seq(*e, "FEXPT_ROLE=payload")) { payload(); }
  }
  tracer();
}
__attribute__((naked, used)) void _start(void) {
  __asm__ volatile("mov %rsp, %rdi\n and $-16, %rsp\n call entry_c\n");
}
