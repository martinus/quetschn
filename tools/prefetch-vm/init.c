// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * /init of the VM of run.sh: system calls whose kernel paths call prefetch(), each timed in a loop, as
 * many as a clang-built x86-64 kernel without ../kernel-port/x86-prefetcht0.patch has no prefetcht0 for.
 * Prints per benchmark "BENCH <name> <ns per operation>", the median of REPS runs, then powers off.
 *
 *   cputime        clock_gettime(CLOCK_THREAD_CPUTIME_ID): task_sched_runtime()
 *   times          times(): the same through the process's threads
 *   getcwd         getcwd() 40 directories deep: prepend_path()
 *   udp_rr         a datagram of 64 bytes to a socket on lo and one back: __alloc_skb(), __dev_queue_xmit(),
 *                  net_rx_action(), udp_queue_rcv_one_skb(), __skb_recv_udp(), udp_skb_destructor()
 *   udp_stream     64 datagrams of 64 bytes, then 64 received, per datagram
 *   tcp_rr         64 bytes over a TCP connection on lo and 64 back: tcp_ack(), __release_sock()
 *   tcp_stream     16 KiB written and read on that connection
 *   nullb_pread    pread() of 4 KiB with O_DIRECT from /dev/nullb0: __blk_mq_alloc_requests()
 *   nullb_iopoll   io_uring with IORING_SETUP_IOPOLL, 32 reads of 4 KiB at a time from /dev/nullb0, per
 *                  read: blk_mq_end_request_batch()
 *
 * The VM has one CPU, so each loop runs alone; null_blk is set up by the kernel's command line, see run.sh.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

#define REPS 9

static void die(const char* what) {
    printf("BENCH-ERROR %s: %s\n", what, strerror(errno));
    fflush(stdout);
    reboot(RB_POWER_OFF);
    exit(1);
}

static double now_ns(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;

    return (x > y) - (x < y);
}

/* runs f(arg) REPS times, each n operations, and prints the median ns per operation */
static void bench(const char* name, void (*f)(void*, long), void* arg, long n) {
    double v[REPS];
    int r;

    f(arg, n / 10); /* warm up */
    for (r = 0; r < REPS; r++) {
        double t0 = now_ns();

        f(arg, n);
        v[r] = (now_ns() - t0) / (double)n;
    }
    qsort(v, REPS, sizeof(v[0]), cmp_double);
    printf("BENCH %s %.2f\n", name, v[REPS / 2]);
    fflush(stdout);
}

static void cputime(void* arg, long n) {
    struct timespec t;

    (void)arg;
    while (n--)
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
}

static void times_loop(void* arg, long n) {
    struct tms t;

    (void)arg;
    while (n--)
        times(&t);
}

static void getcwd_loop(void* arg, long n) {
    char buf[4096];

    (void)arg;
    while (n--)
        if (!getcwd(buf, sizeof(buf)))
            die("getcwd");
}

/* a pair of connected sockets on lo, udp or tcp */
struct pair {
    int a, b;
};

static void udp_pair(struct pair* p) {
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct sockaddr_in sb = sa;
    socklen_t len = sizeof(sa);

    p->a = socket(AF_INET, SOCK_DGRAM, 0);
    p->b = socket(AF_INET, SOCK_DGRAM, 0);
    if (p->a < 0 || p->b < 0 || bind(p->a, (void*)&sa, sizeof(sa)) || bind(p->b, (void*)&sb, sizeof(sb)) ||
        getsockname(p->a, (void*)&sa, &len) || getsockname(p->b, (void*)&sb, &len) || connect(p->a, (void*)&sb, sizeof(sb)) ||
        connect(p->b, (void*)&sa, sizeof(sa)))
        die("udp sockets");
}

static void tcp_pair(struct pair* p) {
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(sa);
    int l = socket(AF_INET, SOCK_STREAM, 0), one = 1;

    if (l < 0 || bind(l, (void*)&sa, sizeof(sa)) || listen(l, 1) || getsockname(l, (void*)&sa, &len))
        die("tcp listen");
    p->a = socket(AF_INET, SOCK_STREAM, 0);
    if (p->a < 0 || connect(p->a, (void*)&sa, sizeof(sa)) || (p->b = accept(l, 0, 0)) < 0)
        die("tcp connect");
    setsockopt(p->a, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(p->b, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    close(l);
}

/* all n bytes from fd */
static void read_all(int fd, char* buf, size_t n) {
    while (n) {
        ssize_t k = read(fd, buf, n);

        if (k <= 0)
            die("read");
        buf += k;
        n -= (size_t)k;
    }
}

static void rr(void* arg, long n) {
    struct pair* p = arg;
    char buf[64] = {0};

    while (n--) {
        if (write(p->a, buf, 64) != 64)
            die("write");
        read_all(p->b, buf, 64);
        if (write(p->b, buf, 64) != 64)
            die("write");
        read_all(p->a, buf, 64);
    }
}

static void udp_stream(void* arg, long n) {
    struct pair* p = arg;
    char buf[64] = {0};
    long k;

    for (; n > 0; n -= 64) {
        for (k = 0; k < 64; k++)
            if (send(p->a, buf, 64, 0) != 64)
                die("send");
        for (k = 0; k < 64; k++)
            if (recv(p->b, buf, 64, 0) != 64)
                die("recv");
    }
}

static void tcp_stream(void* arg, long n) {
    struct pair* p = arg;
    static char buf[16384];

    while (n--) {
        if (write(p->a, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
            die("write");
        read_all(p->b, buf, sizeof(buf));
    }
}

/* /dev/nullb0 with O_DIRECT, and a buffer of 32 aligned blocks */
struct nullb {
    int fd;
    char* buf;
};

static void nullb_pread(void* arg, long n) {
    struct nullb* nb = arg;
    long k;

    for (k = 0; k < n; k++)
        if (pread(nb->fd, nb->buf, 4096, (off_t)(k % 1024) * 4096) != 4096)
            die("pread");
}

/* an io_uring of 32 entries with IORING_SETUP_IOPOLL, its rings mapped */
struct ring {
    struct nullb* nb;
    int fd;
    unsigned *sq_tail, *sq_mask, *sq_array, *cq_head, *cq_tail, *cq_mask;
    struct io_uring_sqe* sqes;
    struct io_uring_cqe* cqes;
};

static void ring_setup(struct ring* r, struct nullb* nb) {
    struct io_uring_params p = {.flags = IORING_SETUP_IOPOLL};
    size_t sq_len, cq_len;
    char *sq, *cq;

    r->nb = nb;
    r->fd = (int)syscall(__NR_io_uring_setup, 32, &p);
    if (r->fd < 0)
        die("io_uring_setup");
    sq_len = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    cq_len = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP)
        sq_len = cq_len = sq_len > cq_len ? sq_len : cq_len;
    sq = mmap(0, sq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
    cq = p.features & IORING_FEAT_SINGLE_MMAP
             ? sq
             : mmap(0, cq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_CQ_RING);
    r->sqes = mmap(0,
                   p.sq_entries * sizeof(struct io_uring_sqe),
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE,
                   r->fd,
                   IORING_OFF_SQES);
    if (sq == MAP_FAILED || cq == MAP_FAILED || r->sqes == MAP_FAILED)
        die("io_uring mmap");
    r->sq_tail = (unsigned*)(sq + p.sq_off.tail);
    r->sq_mask = (unsigned*)(sq + p.sq_off.ring_mask);
    r->sq_array = (unsigned*)(sq + p.sq_off.array);
    r->cq_head = (unsigned*)(cq + p.cq_off.head);
    r->cq_tail = (unsigned*)(cq + p.cq_off.tail);
    r->cq_mask = (unsigned*)(cq + p.cq_off.ring_mask);
    r->cqes = (struct io_uring_cqe*)(cq + p.cq_off.cqes);
}

static void nullb_iopoll(void* arg, long n) {
    struct ring* r = arg;
    long done = 0;

    while (done < n) {
        unsigned tail = *r->sq_tail, head, k;

        for (k = 0; k < 32; k++, tail++) {
            unsigned idx = tail & *r->sq_mask;
            struct io_uring_sqe* s = &r->sqes[idx];

            memset(s, 0, sizeof(*s));
            s->opcode = IORING_OP_READ;
            s->fd = r->nb->fd;
            s->addr = (uint64_t)(uintptr_t)(r->nb->buf + 4096 * k);
            s->len = 4096;
            s->off = (uint64_t)((done + k) % 1024) * 4096;
            r->sq_array[idx] = idx;
        }
        __atomic_store_n(r->sq_tail, tail, __ATOMIC_RELEASE);
        if (syscall(__NR_io_uring_enter, r->fd, 32, 32, IORING_ENTER_GETEVENTS, 0, 0) != 32)
            die("io_uring_enter");
        head = *r->cq_head;
        for (k = 0; k < 32;) {
            if (head == __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE)) {
                if (syscall(__NR_io_uring_enter, r->fd, 0, 32 - k, IORING_ENTER_GETEVENTS, 0, 0) < 0)
                    die("io_uring_enter");
                continue;
            }
            if (r->cqes[head & *r->cq_mask].res != 4096) {
                errno = -r->cqes[head & *r->cq_mask].res;
                die("io_uring read");
            }
            head++;
            k++;
        }
        __atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
        done += 32;
    }
}

/* lo up, which gives it 127.0.0.1 */
static void lo_up(void) {
    struct ifreq ifr = {0};
    int s = socket(AF_INET, SOCK_DGRAM, 0);

    strcpy(ifr.ifr_name, "lo");
    if (s < 0 || ioctl(s, SIOCGIFFLAGS, &ifr))
        die("lo flags");
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(s, SIOCSIFFLAGS, &ifr))
        die("lo up");
    close(s);
}

int main(void) {
    struct pair udp, tcp;
    struct nullb nb;
    struct ring ring;
    int k;

    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    mount("proc", "/proc", "proc", 0, 0);
    mount("tmpfs", "/tmp", "tmpfs", 0, 0);

    bench("cputime", cputime, 0, 2000000);
    bench("times", times_loop, 0, 2000000);

    if (chdir("/tmp"))
        die("chdir");
    for (k = 0; k < 40; k++)
        if (mkdir("directory", 0755) || chdir("directory"))
            die("mkdir");
    bench("getcwd", getcwd_loop, 0, 500000);
    if (chdir("/"))
        die("chdir");

    lo_up();
    udp_pair(&udp);
    bench("udp_rr", rr, &udp, 100000);
    bench("udp_stream", udp_stream, &udp, 200000);
    tcp_pair(&tcp);
    bench("tcp_rr", rr, &tcp, 100000);
    bench("tcp_stream", tcp_stream, &tcp, 20000);

    nb.fd = open("/dev/nullb0", O_RDONLY | O_DIRECT);
    if (nb.fd < 0 || posix_memalign((void**)&nb.buf, 4096, 32 * 4096))
        die("/dev/nullb0");
    bench("nullb_pread", nullb_pread, &nb, 500000);
    ring_setup(&ring, &nb);
    bench("nullb_iopoll", nullb_iopoll, &ring, 1000000);

    printf("BENCH-DONE\n");
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return 0;
}
