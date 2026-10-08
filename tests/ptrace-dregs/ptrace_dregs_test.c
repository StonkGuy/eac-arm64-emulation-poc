// SPDX-License-Identifier: MIT
//
// Regression test for the hardware debug registers through PTRACE_POKEUSER / PTRACE_PEEKUSER under FEX.
//
// Expectations follow Linux x86-64 exactly (arch/x86/kernel/ptrace.c PTRACE_PEEKUSR/PTRACE_POKEUSR,
// ptrace_set_debugreg/ptrace_set_breakpoint_addr; arch/x86/kernel/hw_breakpoint.c; kernel/events/hw_breakpoint.c),
// and every expectation here was checked against a native kernel:
//
//  * The debug registers live inside `struct user` (arch/x86/include/asm/user_64.h): DR0-DR3 at byte 848, DR4/DR5
//    which do not exist, DR6, DR7. The kernel's `struct user` is 928 bytes (u_debugreg[8] + error_code +
//    fault_address; glibc's is 912 because it omits the last two). PEEKUSR/POKEUSR reject any slot-unaligned
//    offset and anything at or past 928 with -EIO. An in-struct offset with no handler (i387, u_tsize, u_comm,
//    error_code, ...) reads back 0.
//  * DR0-DR3 POKEUSER validates the address (ptrace_set_breakpoint_addr): it must be below TASK_SIZE_MAX
//    (0x7ffffffff000) or the write is -EINVAL. A DR whose next breakpoint is a fresh 1-byte write has no
//    alignment requirement, so e.g. `1` is accepted and only kernel/non-canonical values fail. A PEEK before a
//    successful write reads 0.
//  * DR4/DR5: POKEUSER is -EIO ("There are no DR4 or DR5 registers"), PEEKUSER reads 0.
//  * DR6 POKEUSER always succeeds and round-trips (raw value). DR7 POKEUSER is validated and the raw value is
//    stored on success: a field whose enable bits are clear and that has no breakpoint is not validated at all,
//    an enabled (or already-registered) field whose length/type encoding is invalid is -EINVAL, and an address
//    that does not meet the alignment implied by its length is -EINVAL. A failed DR7 write leaves the previous
//    value in place.
//
// FEX used to accept every out-of-image POKEUSER and silently discard it, and to return -EPERM from PEEKUSER,
// so a tracer that set a hardware breakpoint and read it back saw a silent no-op instead of the value it wrote.
// This test makes all of those calls and checks the values and the errnos.
//
// Build: tests/ptrace-dregs/build.sh [clang]   (x86-64 binary, run it under FEX)
// Run:   ./ptrace_dregs_test   (exit status = number of failed checks)

typedef unsigned long u64;
typedef long i64;
typedef unsigned char u8;

#if defined(__x86_64__)
static inline i64 sc6(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  i64 ret;
  register i64 r10 __asm__("r10") = d;
  register i64 r8 __asm__("r8") = e;
  register i64 r9 __asm__("r9") = f;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return ret;
}
enum { SYS_read = 0, SYS_write = 1, SYS_fork = 57, SYS_wait4 = 61, SYS_kill = 62, SYS_ptrace = 101, SYS_getpid = 39, SYS_mmap = 9, SYS_exit_group = 231 };
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
enum { SYS_read = 63, SYS_write = 64, SYS_fork = 220, SYS_wait4 = 260, SYS_kill = 129, SYS_ptrace = 117, SYS_getpid = 172, SYS_mmap = 222, SYS_exit_group = 94 };
#endif

#define sc1(n, a) sc6(n, (i64)(a), 0, 0, 0, 0, 0)
#define sc2(n, a, b) sc6(n, (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sc3(n, a, b, c) sc6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sc4(n, a, b, c, d) sc6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), 0, 0)
#define sc6_(n, a, b, c, d, e, f) sc6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), (i64)(e), (i64)(f))

enum { PT_TRACEME = 0, PT_PEEKUSER = 3, PT_POKEUSER = 6, PT_CONT = 7 };
enum { SIGSTOP_ = 19 };
enum { EIO = 5, EINVAL = 22 };
// offsets inside the kernel's `struct user`
enum { OFF_DR0 = 848, OFF_DR4 = 880, OFF_DR5 = 888, OFF_DR6 = 896, OFF_DR7 = 904, OFF_END = 928 };
#define STOPPED(st) (((st) & 0xff) == 0x7f)
#define STOPSIG(st) (((st) >> 8) & 0xff)

static u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (i64)s, slen(s)); }
static void outdec(i64 v) {
  char b[24]; int n = 0, neg = v < 0; u64 u = neg ? (u64)(-v) : (u64)v;
  if (!u) b[n++] = '0'; while (u) { b[n++] = '0' + (u % 10); u /= 10; }
  if (neg) b[n++] = '-'; char r[24]; int k = 0; while (n) r[k++] = b[--n]; r[k] = 0; out(r);
}
static void outhex(u64 v) {
  char b[19]; b[0] = '0'; b[1] = 'x';
  for (int i = 0; i < 16; ++i) b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
  b[18] = 0; out(b);
}

static int failures;
static void check(int ok, const char* what) {
  out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) ++failures;
}

static i64 ptrace(i64 req, i64 pid, i64 addr, i64 data) { return sc4(SYS_ptrace, req, pid, addr, data); }
static i64 poke_user(i64 pid, u64 off, u64 val) { return ptrace(PT_POKEUSER, pid, (i64)off, (i64)val); }
static i64 peek_user(i64 pid, u64 off, u64* val) { return ptrace(PT_PEEKUSER, pid, (i64)off, (i64)val); }
static int wait_status(i64 pid, int* st) { return sc4(SYS_wait4, pid, (i64)st, 0x40000000 /* __WALL */, 0) == pid; }

// A live traced child stopped on SIGSTOP, ready for POKEUSER/PEEKUSER.
static i64 spawn_traced(void) {
  i64 child = sc1(SYS_fork, 0);
  if (child == 0) {
    if (ptrace(PT_TRACEME, 0, 0, 0) != 0) sc1(SYS_exit_group, 90);
    sc2(SYS_kill, sc1(SYS_getpid, 0), SIGSTOP_);
    sc1(SYS_exit_group, 3);
  }
  int st = 0;
  if (!wait_status(child, &st) || !STOPPED(st) || STOPSIG(st) != SIGSTOP_) return -1;
  return child;
}

static void resume_and_reap(i64 child, const char* what) {
  check(ptrace(PT_CONT, child, 0, 0) == 0, what);
  int st = 0;
  check(wait_status(child, &st) && STOPPED(st) == 0 && ((st >> 8) & 0xff) == 3, "the child exits 3 after being resumed");
}

__attribute__((used)) static void entry_c(void) {
  // A valid user address for DR0-DR3: an anonymous page inherited across fork, so parent and child see the
  // same address. mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) -> prot 3, flags 0x22.
  i64 page = sc6_(SYS_mmap, 0, 4096, 3, 0x22, -1, 0);
  const u64 VA = (u64)page;
  const u64 VA2 = VA + 64;

  // ---- DR0-DR3/DR6/DR7 round-trip through POKEUSER/PEEKUSER (valid user addresses) ----
  {
    i64 child = spawn_traced();
    check(child > 0, "fork");
    const u64 vals[6] = {VA, VA2, VA + 128, VA + 256, 0x0000000000000000UL, 0x0000000000000002UL};
    const u64 offs[6] = {OFF_DR0, OFF_DR0 + 8, OFF_DR0 + 16, OFF_DR0 + 24, OFF_DR6, OFF_DR7};
    const char* names[6] = {"DR0", "DR1", "DR2", "DR3", "DR6", "DR7"};
    for (int i = 0; i < 6; ++i) {
      u64 got = 0;
      i64 w = poke_user(child, offs[i], vals[i]);
      i64 r = peek_user(child, offs[i], &got);
      int ok = (w == 0) && (r == 0) && (got == vals[i]);
      out(ok ? "PASS: " : "FAIL: "); out(names[i]); out(" round-trips through POKEUSER/PEEKUSER");
      if (!ok) { out("  (write="); outdec(w); out(" read="); outdec(r); out(" value="); outhex(got); out(")"); ++failures; }
      out("\n");
    }
    resume_and_reap(child, "PTRACE_CONT");
  }

  // ---- an invalid address for DR0-DR3 is -EINVAL and leaves the value unchanged ----
  {
    i64 child = spawn_traced();
    check(poke_user(child, OFF_DR0, VA) == 0, "POKEUSER of a valid DR0 address succeeds");
    const u64 bad[5] = {0xdeadbeefcafe1234UL, 0x0000800000000000UL /* non-canonical */, 0xffff800000000000UL /* kernel */, ~0UL, 0x00007ffffffffffeUL /* at TASK_SIZE_MAX-1 */};
    const char* bnames[5] = {"0xdeadbeefcafe1234", "0x800000000000 (non-canonical)", "0xffff800000000000 (kernel)", "~0", "0x7ffffffffffe (at TASK_SIZE_MAX)"};
    int all_inval = 1;
    for (int i = 0; i < 5; ++i) {
      i64 w = poke_user(child, OFF_DR0 + 8, bad[i]);
      u64 got = 0;
      i64 r = peek_user(child, OFF_DR0 + 8, &got);
      if (w != -EINVAL || r != 0 || got != 0) {
        all_inval = 0;
        out("FAIL: POKEUSER of invalid DR1 address "); out(bnames[i]); out(" should be -EINVAL and leave 0 (write=");
        outdec(w); out(" value="); outhex(got); out(")\n"); ++failures;
      }
    }
    check(all_inval, "POKEUSER of an invalid DR0-DR3 address fails with -EINVAL");
    check(poke_user(child, OFF_DR0 + 8, VA2) == 0, "a later valid DR1 address still succeeds");
    u64 got = 0;
    check(peek_user(child, OFF_DR0, &got) == 0 && got == VA, "a failed DR write leaves an earlier DR0 unchanged");
    resume_and_reap(child, "PTRACE_CONT after the address checks");
  }

  // ---- DR4/DR5 do not exist: PEEKUSER answers 0, POKEUSER is -EIO ----
  {
    i64 child = spawn_traced();
    u64 v4 = ~0UL, v5 = ~0UL;
    check(peek_user(child, OFF_DR4, &v4) == 0 && v4 == 0, "PEEKUSER of DR4 returns 0 (the register does not exist)");
    check(peek_user(child, OFF_DR5, &v5) == 0 && v5 == 0, "PEEKUSER of DR5 returns 0 (the register does not exist)");
    check(poke_user(child, OFF_DR4, 0x1234) == -EIO, "POKEUSER of DR4 fails with -EIO");
    check(poke_user(child, OFF_DR5, 0x1234) == -EIO, "POKEUSER of DR5 fails with -EIO");
    resume_and_reap(child, "PTRACE_CONT after the DR4/DR5 checks");
  }

  // ---- in-struct non-register offsets read back 0; passing the struct end is -EIO ----
  {
    i64 child = spawn_traced();
    u64 v = ~0UL;
    check(peek_user(child, 1024, &v) == -EIO, "PEEKUSER past sizeof(struct user) fails with -EIO");
    check(poke_user(child, 1024, 1) == -EIO, "POKEUSER past sizeof(struct user) fails with -EIO");
    check(peek_user(child, OFF_END, &v) == -EIO, "PEEKUSER at sizeof(struct user) fails with -EIO");
    check(poke_user(child, OFF_END, 1) == -EIO, "POKEUSER at sizeof(struct user) fails with -EIO");
    check(poke_user(child, 5120, 1) == -EIO, "POKEUSER of a wild offset fails with -EIO");

    const u64 nohand[4] = {216, 816, 832, 920}; // just past user_regs_struct, u_comm, u_comm end, error_code
    const char* nnames[4] = {"register-image end (216)", "u_comm (816)", "u_comm end (832)", "error_code (920)"};
    for (int i = 0; i < 4; ++i) {
      u64 gv = ~0UL;
      i64 r = peek_user(child, nohand[i], &gv);
      i64 w = poke_user(child, nohand[i], 1);
      int ok = (r == 0) && (gv == 0) && (w == -EIO);
      out(ok ? "PASS: " : "FAIL: "); out("in-struct non-register offset reads 0 and rejects writes ");
      out(nnames[i]);
      if (!ok) { out("  (read="); outdec(r); out(" value="); outhex(gv); out(" write="); outdec(w); out(")"); ++failures; }
      out("\n");
    }

    u64 vun = 0;
    check(peek_user(child, OFF_DR0 + 4, &vun) == -EIO, "PEEKUSER of an unaligned offset fails with -EIO");
    check(poke_user(child, OFF_DR0 + 4, 1) == -EIO, "POKEUSER of an unaligned offset fails with -EIO");
    resume_and_reap(child, "PTRACE_CONT after the offset checks");
  }

  // ---- DR7 validation and normalisation. Each case uses a fresh child so the DRs start clean. ----
  {
    // Accepted: enable DR0 with an execute / len-X field (0x0); the raw value is stored back.
    i64 child = spawn_traced();
    check(poke_user(child, OFF_DR0, VA) == 0, "DR7 case: valid DR0 address");
    u64 got = ~0UL;
    i64 w = poke_user(child, OFF_DR7, 0x402UL);
    i64 r = peek_user(child, OFF_DR7, &got);
    check(w == 0 && r == 0 && got == 0x402UL, "DR7 with a valid execute field for an enabled DR0 round-trips");
    resume_and_reap(child, "PTRACE_CONT after the valid-DR7 case");

    // Rejected: an enabled field with an invalid length/type encoding, e.g. execute + len 2 (0x4).
    child = spawn_traced();
    poke_user(child, OFF_DR0, VA);
    w = poke_user(child, OFF_DR7, 0x40002UL); // field0 = 0x4, enable DR0
    got = ~0UL;
    r = peek_user(child, OFF_DR7, &got);
    check(w == -EINVAL, "DR7 with an invalid execute length (field 0x4) fails with -EINVAL");
    check(r == 0 && got == 0, "a failed DR7 write leaves the previous DR7 value (0) in place");
    resume_and_reap(child, "PTRACE_CONT after the invalid-encoding DR7 case");

    // Rejected: the address does not meet the alignment implied by the field's length (len 4 -> align 3).
    child = spawn_traced();
    poke_user(child, OFF_DR0, VA | 1);
    w = poke_user(child, OFF_DR7, 0x4D0002UL); // field0 = 0xD (write, len 4), enable DR0
    got = ~0UL;
    r = peek_user(child, OFF_DR7, &got);
    check(w == -EINVAL, "DR7 needing a 4-byte-aligned DR0 address fails with -EINVAL for a misaligned one");
    check(r == 0 && got == 0, "the misaligned-address DR7 write also leaves DR7 unchanged");
    resume_and_reap(child, "PTRACE_CONT after the misaligned DR7 case");

    // Accepted: write / len 4 with an aligned address.
    child = spawn_traced();
    poke_user(child, OFF_DR0, VA);
    w = poke_user(child, OFF_DR7, 0x4D0002UL);
    got = ~0UL;
    r = peek_user(child, OFF_DR7, &got);
    check(w == 0 && r == 0 && got == 0x4D0002UL, "DR7 with a valid write/len-4 field for an aligned DR0 round-trips");
    resume_and_reap(child, "PTRACE_CONT after the len-4 DR7 case");

    // Accepted: a field in the DR range with no enable bits and no registered breakpoint is not validated.
    child = spawn_traced();
    w = poke_user(child, OFF_DR7, 0x400000UL); // field1 = 0x4 (invalid), no enable bits, DR1 never written
    got = ~0UL;
    r = peek_user(child, OFF_DR7, &got);
    check(w == 0 && r == 0 && got == 0x400000UL, "a disabled DR7 field with no breakpoint is stored without validation");
    resume_and_reap(child, "PTRACE_CONT after the unvalidated-field DR7 case");

    // Accepted: the reserved bits (8,9,10) are kept in the stored value, as Linux does.
    child = spawn_traced();
    poke_user(child, OFF_DR0, VA);
    w = poke_user(child, OFF_DR7, 0x300UL); // bits 8,9 set, no enable bits
    got = ~0UL;
    r = peek_user(child, OFF_DR7, &got);
    check(w == 0 && r == 0 && got == 0x300UL, "DR7 reserved bits (0x300) are stored in the read-back value");
    resume_and_reap(child, "PTRACE_CONT after the reserved-bits DR7 case");
  }

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc1(SYS_exit_group, failures);
}
#if defined(__x86_64__)
__attribute__((naked, used)) void _start(void) { __asm__ volatile("and $-16, %rsp\n call entry_c\n"); }
#else
__asm__(".text\n.globl _start\n_start:\n bl entry_c\n");
#endif
