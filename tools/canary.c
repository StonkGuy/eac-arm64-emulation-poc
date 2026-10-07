// Fixed-work CPU speed probe: a dependent integer chain (~10 ms at full speed) timed every 0.5 s. Run it pinned to a performance core
// (taskset -c 6 ./canary 120 > canary.txt) next to the game. Output lines: "<epoch> <nanoseconds>". Build: gcc -O1 -o canary canary.c
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char** argv) {
  double secs = argc > 1 ? atof(argv[1]) : 60; (void)secs;
  struct timespec t0, t1, rt; uint64_t x = 1;
  clock_gettime(CLOCK_REALTIME, &rt); double end = rt.tv_sec + rt.tv_nsec / 1e9 + (argc > 1 ? atof(argv[1]) : 60);
  for (;;) {
    clock_gettime(CLOCK_REALTIME, &rt); double now = rt.tv_sec + rt.tv_nsec / 1e9; if (now > end) break;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint64_t i = 0; i < 30000000ULL; i++) { x = x * 3 + i; asm volatile("" : "+r"(x)); }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("%.2f %lld\n", now, (long long)((t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec)));
    fflush(stdout);
    usleep(440000);
  }
  return (int)(x & 1) * 0;
}
