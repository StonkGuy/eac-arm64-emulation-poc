// SPDX-License-Identifier: MIT
//
// Native check of the SKIP path of cpuid_fault_test: runs a program as if the CPU had no CPUID faulting, by making
// arch_prctl(ARCH_SET_CPUID, ...) fail with ENODEV through a seccomp filter (the kernel answer on such a CPU).
//   cc -o without_faulting without_faulting.c && ./without_faulting ./cpuid_fault_test   -> SKIP, RESULT: PASS
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
int main(int argc, char** argv) {
  struct sock_filter f[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_arch_prctl, 0, 3),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x1012 /* ARCH_SET_CPUID */, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENODEV),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog p = {sizeof(f) / sizeof(f[0]), f};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &p)) { perror("seccomp"); return 2; }
  execv(argv[1], argv + 1); perror("execv"); return 2;
}
