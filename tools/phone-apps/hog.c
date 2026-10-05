// hog <MiB>: mlock MiB of memory that is not zero and sleep, so that the phone has that much less RAM
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc, char** argv) {
    size_t n = (size_t)atol(argv[1]) << 20;
    unsigned char* p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 1;
    for (size_t i = 0; i < n; i += 4096) memset(p + i, (int)(i >> 12) | 1, 4096);
    if (mlock(p, n)) { perror("mlock"); return 1; }
    printf("hog %zu MiB locked\n", n >> 20);
    fflush(stdout);
    for (;;) pause();
}
