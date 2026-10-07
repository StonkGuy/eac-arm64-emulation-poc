#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/prctl.h>
#ifndef PR_SET_MEM_MODEL
#define PR_SET_MEM_MODEL 0x4d4d444c
#define PR_GET_MEM_MODEL 0x4d4d444d
#define PR_SET_MEM_MODEL_DEFAULT 0
#define PR_SET_MEM_MODEL_TSO 1
#endif
int main(void) {
  int r = prctl(PR_GET_MEM_MODEL, 0, 0, 0, 0);
  printf("GET_MEM_MODEL -> %d (%s)\n", r, r < 0 ? strerror(errno) : "ok");
  if (r == 0) {
    int s = prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO, 0, 0, 0);
    printf("SET TSO -> %d (%s)\n", s, s < 0 ? strerror(errno) : "ok");
    if (s == 0) { printf("now GET -> %d\n", prctl(PR_GET_MEM_MODEL, 0, 0, 0, 0)); }
  }
  return 0;
}
