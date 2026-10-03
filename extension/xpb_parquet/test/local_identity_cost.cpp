/*
 * What the per-read identity check costs, as a measurement that can actually
 * resolve it.
 *
 * A wall-clock before/after comparison cannot: the effect is tens of
 * microseconds against several milliseconds of run-to-run variance, and the
 * guarded build measured FASTER in three of four shapes, which is noise and
 * not an improvement. So the cost is reported as the one extra syscall per
 * read that it is, timed in isolation.
 */
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    const char *p = argc > 1 ? argv[1] : "/etc/hostname";
    int fd = ::open(p, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st;
    const int N = 2000000;
    /* warm */
    for (int i = 0; i < 10000; i++) ::fstat(fd, &st);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < N; i++) ::fstat(fd, &st);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = ((double)(b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / N;
    printf("fstat on an open fd: %.0f ns per call (%d calls)\n", ns, N);
    printf("402 of them (one full scan of the benchmark file): %.3f ms\n", ns * 402 / 1e6);
    printf("as a share of the measured 163 ms full scan: %.4f %%\n", ns * 402 / 1e6 / 163 * 100);
    ::close(fd);
    return 0;
}
