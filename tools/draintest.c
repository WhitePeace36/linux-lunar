// SPDX-License-Identifier: GPL-2.0
/*
 * draintest - provoke and measure lru_add_drain_all() stalls.
 *
 *   draintest hog CPU SECONDS   pin to CPU, become SCHED_FIFO, keep faulting
 *                               in fresh pages so that CPU's LRU add batch is
 *                               never empty, and never yield.
 *   draintest drop COUNT        write 1 to /proc/sys/vm/drop_caches COUNT
 *                               times (a bounded drain caller), print timing.
 *   draintest compact           write 1 to /proc/sys/vm/compact_memory (a
 *                               strict drain caller), print how long it took.
 *
 * Build: cc -O2 -static -o draintest draintest.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int hog(int cpu, double seconds)
{
	const size_t pg = 4096, npages = 16384;	/* 64 MiB window */
	struct sched_param sp = { .sched_priority = 50 };
	cpu_set_t set;
	double end;
	size_t i = 0;
	char *buf;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set)) {
		perror("sched_setaffinity");
		return 1;
	}
	/* Small folios only: large ones bypass the per-CPU LRU batches. */
	if (prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0))
		perror("PR_SET_THP_DISABLE");
	buf = mmap(NULL, npages * pg, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	if (madvise(buf, npages * pg, MADV_NOHUGEPAGE))
		perror("MADV_NOHUGEPAGE");
	if (sched_setscheduler(0, SCHED_FIFO, &sp)) {
		perror("sched_setscheduler");
		return 1;
	}
	fprintf(stderr, "hog: SCHED_FIFO on CPU %d for %.1fs\n", cpu, seconds);
	end = now_ms() + seconds * 1e3;
	while (now_ms() < end) {
		/* A new anon page goes into this CPU's lru_add folio batch. */
		buf[i * pg] = 1;
		if (++i == npages) {
			/* Recycle the window: drop the pages, fault them again. */
			madvise(buf, npages * pg, MADV_DONTNEED);
			i = 0;
		}
		for (volatile int spin = 0; spin < 20000; spin++)
			;
	}
	return 0;
}

static int drop(int count)
{
	double max = 0, sum = 0;
	char times[512] = "";
	int slow = 0;

	for (int n = 0; n < count; n++) {
		int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
		double t0, dt;

		if (fd < 0) {
			perror("drop_caches");
			return 1;
		}
		t0 = now_ms();
		if (write(fd, "1", 1) != 1)
			perror("write");
		dt = now_ms() - t0;
		close(fd);
		sum += dt;
		if (dt > max)
			max = dt;
		if (dt > 50)
			slow++;
		snprintf(times + strlen(times), sizeof(times) - strlen(times),
			 " %.0f", dt);
		usleep(100000);
	}
	printf("drop: %d calls, avg %.1f ms, max %.1f ms, %d over 50 ms [ms:%s]\n",
	       count, sum / count, max, slow, times);
	return 0;
}

static int compact(void)
{
	int fd = open("/proc/sys/vm/compact_memory", O_WRONLY);
	double t0, dt;

	if (fd < 0) {
		perror("compact_memory");
		return 1;
	}
	t0 = now_ms();
	if (write(fd, "1", 1) != 1)
		perror("write");
	dt = now_ms() - t0;
	close(fd);
	printf("compact: took %.1f ms\n", dt);
	return 0;
}

static int seal(void)
{
	int fd = memfd_create("draintest", MFD_ALLOW_SEALING);
	char buf[65536];
	double t0, dt;
	int ret;

	if (fd < 0) {
		perror("memfd_create");
		return 1;
	}
	memset(buf, 1, sizeof(buf));
	if (write(fd, buf, sizeof(buf)) != sizeof(buf))
		perror("write");
	t0 = now_ms();
	ret = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE);
	dt = now_ms() - t0;
	printf("seal: F_SEAL_WRITE %s after %.1f ms\n",
	       ret ? strerror(errno) : "ok", dt);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "hog"))
		return hog(atoi(argv[2]), atof(argv[3]));
	if (argc == 3 && !strcmp(argv[1], "drop"))
		return drop(atoi(argv[2]));
	if (argc == 2 && !strcmp(argv[1], "compact"))
		return compact();
	if (argc == 2 && !strcmp(argv[1], "seal"))
		return seal();
	fprintf(stderr, "usage: %s hog CPU SECONDS | drop COUNT | compact | seal\n",
		argv[0]);
	return 2;
}
