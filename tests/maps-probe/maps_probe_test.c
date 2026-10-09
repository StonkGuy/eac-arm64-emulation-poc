// SPDX-License-Identifier: MIT
//
// Regression test: a read of every inaccessible ("---p") mapping listed in /proc/self/maps must be delivered to the
// program as SIGSEGV, and the program must be able to resume past it.
//
// Anti-tamper scanners read /proc/self/maps and probe every listed range under a fault handler. Under FEX the guest's
// maps also list FEX's own host mappings, among them the guard pages around each thread's call/ret shadow stack
// ("[anon:FEXMem_CallRetStacks]"). FEX's SIGSEGV handler took any fault inside that allocation for a shadow-stack
// overflow, reset its shadow-stack register and resumed the faulting instruction, which faulted again: a guest load of
// the guard page spun forever inside FEX (one thread at 100% CPU, mostly kernel time, nothing delivered to the guest).
//
// Checks:
//   1. a PROT_NONE page of our own: the read faults, si_addr is the page, the handler resumes past the load;
//   2. every "---p" mapping in /proc/self/maps: the probe returns (fault delivered, or read allowed) - no hang;
//   3. every probe that faulted reported si_addr = the probed address.
// A hang is caught by alarm(): SIGALRM kills the process and no RESULT line is printed.
//
// On a real x86-64 kernel check 2 covers only the kernel's own guard regions; under FEX it covers FEX's mappings too.
//
// Build: tests/maps-probe/build.sh [clang]   (x86-64 binary; run it under FEX and on a real x86-64 Linux for reference)
// Run:   ./maps_probe_test   (exit status = number of failed checks)

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
enum { SYS_read = 0, SYS_write = 1, SYS_open = 2, SYS_close = 3, SYS_mmap = 9, SYS_rt_sigaction = 13, SYS_alarm = 37, SYS_exit_group = 231 };
enum { SIGSEGV = 11, SA_SIGINFO = 4, SA_RESTORER = 0x04000000, REG_RAX = 13, REG_RIP = 16 };

struct ksigaction { void* handler; u64 flags; void* restorer; u64 mask; };
__attribute__((naked, used)) static void restore_rt(void) { __asm__ volatile("mov $15, %eax\n syscall\n"); }

// probe(addr): returns 0 if the byte was read, 1 if the read faulted (the handler sets RAX = 1 and skips the load).
extern char probe_load[], probe_end[];
long probe(u64 addr);
__asm__(".text\n.globl probe\nprobe:\n xor %eax, %eax\n.globl probe_load\nprobe_load:\n movb (%rdi), %cl\n.globl probe_end\nprobe_end:\n ret\n");

static volatile u64 last_fault_addr;
static volatile u64 unexpected;

static void handler(int sig, void* info, void* uc) {
  u64* gregs = (u64*)((char*)uc + 40);
  (void)sig;
  if (gregs[REG_RIP] == (u64)probe_load) {
    last_fault_addr = *(u64*)((char*)info + 16);
    gregs[REG_RAX] = 1;
    gregs[REG_RIP] = (u64)probe_end;
  } else {
    unexpected = gregs[REG_RIP];
    sc1(SYS_exit_group, 99);
  }
}

static int failures;
static void out(const char* s) { u64 n = 0; while (s[n]) ++n; sc3(SYS_write, 1, s, n); }
static void outhex(u64 v) {
  char b[19]; b[0] = '0'; b[1] = 'x';
  for (int i = 0; i < 16; ++i) b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
  b[18] = 0; out(b);
}
static void outdec(u64 v) { char b[21]; int i = 20; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); out(b + i); }
static void check(int ok, const char* what) { out(ok ? "PASS: " : "FAIL: "); out(what); out("\n"); if (!ok) ++failures; }

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

__attribute__((used)) void entry_c(u64* sp) {
  (void)sp;
  struct ksigaction sa = {(void*)handler, SA_SIGINFO | SA_RESTORER, (void*)restore_rt, 0};
  sc6(SYS_rt_sigaction, SIGSEGV, (i64)&sa, 0, 8, 0, 0);
  sc1(SYS_alarm, 30);

  // 1. our own PROT_NONE page
  u64 page = (u64)sc6(SYS_mmap, 0, 4096, 0 /*PROT_NONE*/, 0x22 /*MAP_PRIVATE|MAP_ANONYMOUS*/, -1, 0);
  last_fault_addr = 0;
  long r = probe(page);
  check(r == 1 && last_fault_addr == page, "1. read of a PROT_NONE page faults with si_addr = the page and resumes");

  // 2./3. every inaccessible mapping listed in /proc/self/maps
  static char maps[1 << 18];
  long fd = sc3(SYS_open, "/proc/self/maps", 0, 0);
  u64 len = 0;
  for (;;) {
    long n = sc3(SYS_read, fd, maps + len, sizeof(maps) - 1 - len);
    if (n <= 0) break;
    len += n;
  }
  sc1(SYS_close, fd);
  maps[len] = 0;

  u64 probed = 0, faulted = 0, badaddr = 0;
  for (u64 i = 0; i < len;) {
    u64 start = 0, j = i;
    for (int h; (h = hexval(maps[j])) >= 0; ++j) start = start * 16 + h;
    while (j < len && maps[j] != ' ') ++j;
    const char* perm = maps + j + 1;
    u64 eol = j;
    while (eol < len && maps[eol] != '\n') ++eol;
    if (perm[0] == '-' && perm[1] == '-' && perm[2] == '-' && start != page) {
      last_fault_addr = 0;
      long f = probe(start);
      ++probed;
      if (f) {
        ++faulted;
        if (last_fault_addr != start) ++badaddr;
      }
      out("  probed "); outhex(start); out(f ? " fault " : " read  ");
      // the mapping's name, if any
      u64 k = eol;
      while (k > j && maps[k - 1] != ' ') --k;
      for (; k < eol; ++k) sc3(SYS_write, 1, maps + k, 1);
      out("\n");
    }
    i = eol + 1;
  }
  out("  inaccessible mappings probed: "); outdec(probed); out(", faulted: "); outdec(faulted); out("\n");
  check(1, "2. every inaccessible mapping in /proc/self/maps was probed without hanging");
  check(badaddr == 0, "3. every probe that faulted reported si_addr = the probed address");

  out(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  sc1(SYS_exit_group, failures);
}

__attribute__((naked, used)) void _start(void) { __asm__ volatile("mov %rsp, %rdi\n and $-16, %rsp\n call entry_c\n"); }
