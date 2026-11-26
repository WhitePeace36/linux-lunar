/*
 * User-side cost of one Windows syscall in Wine, with Wine's real
 * __wine_syscall_dispatcher (extracted from signal_x86_64.c) running against
 * a fake TEB, user_shared_data page and syscall table.  The kernel part of
 * an SUD trap is not included: it is the same for every variant here.
 *
 *   direct     the Unix NtQueryPerformanceCounter itself (clock_gettime)
 *   stub       Wine's normal path: stub -> __wine_syscall_dispatcher
 *   upcall     what follows an SUD trap with PUSH_RET: upcall_push -> dispatcher
 * "+" = the same with the XINUSE-guided save instead of xsavec64
 */
#define _GNU_SOURCE
#include <cpuid.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include <time.h>
#include <unistd.h>

#define __ASM_NAME(n) n
#define __ASM_LOCAL_LABEL(l) ".L" l
#define __ASM_CFI(s) ""
#define __ASM_CFI_REG_IS_AT1(a, b, c) ""
#define __ASM_CFI_REG_IS_AT2(a, b, c, d) ""
#define __ASM_CFI_CFA_IS_AT1(a, b) ""
#define __ASM_CFI_CFA_IS_AT2(a, b, c) ""
#define __ASM_GLOBAL_FUNC(name, code) __asm__(".text\n.globl " #name "\n" #name ":\n" code);

int syscall_dispatch_enabled = 1;
char xinuse_fast_save;
unsigned int xinuse_mxcsr_mask = 0xffbf;
void trace_syscall(void) { abort(); }
void trace_sysret(void) { abort(); }

#include "dispatcher.inc"

#define QPC_ID 0x31
#define TICKSPERSEC 10000000ULL

/* the Unix side, as in dlls/ntdll/unix/sync.c */
long fake_NtQueryPerformanceCounter(uint64_t *counter, uint64_t *freq)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	*counter = ts.tv_sec * TICKSPERSEC + ts.tv_nsec / 100;
	if (freq) *freq = TICKSPERSEC;
	return 0;
}

struct service_table { void **table; void *counters; uint64_t limit; unsigned char *args; };
static void *services[0x100];
static unsigned char service_args[0x100];
static struct service_table tables[4];
static unsigned char teb[0x2000] __attribute__((aligned(4096)));
static unsigned char kstack[0x40000] __attribute__((aligned(64)));
static void *instr_cb;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* "Windows" call sites; clobbers follow the Win64 volatile set */
#define WIN_CLOBBERS "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc"

static inline long via_stub(uint64_t *c, uint64_t *f)
{
	long ret;
	register uint64_t r10 __asm__("r10") = (uint64_t)c;
	register uint64_t rdx __asm__("rdx") = (uint64_t)f;
	/* the dispatcher returns to (pushed address - 0xb) */
	__asm__ volatile("leaq 1f+0xb(%%rip),%%r11\n\tpushq %%r11\n\t"
			 "jmp __wine_syscall_dispatcher\n1:"
			 : "=a"(ret), "+r"(r10), "+r"(rdx) : "0"((long)QPC_ID) : "rcx", "r8", "r9", "r11",
			   "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
	return ret;
}

/* enter an upcall handler exactly as the kernel does in PUSH_RET mode */
#define VIA_PUSH(name, handler)							\
static inline long name(uint64_t *c, uint64_t *f)				\
{										\
	long ret;								\
	register uint64_t r10 __asm__("r10") = (uint64_t)c;			\
	register uint64_t rdx __asm__("rdx") = (uint64_t)f;			\
	__asm__ volatile("leaq 1f(%%rip),%%rcx\n\t"				\
			 "movq %%rcx,-136(%%rsp)\n\t"				\
			 "leaq -136(%%rsp),%%rsp\n\t"				\
			 "pushfq\n\tpopq %%r11\n\t"				\
			 "leaq " #handler "(%%rip),%%rcx\n\t"			\
			 "jmp " #handler "\n1:"					\
			 : "=a"(ret), "+r"(r10), "+r"(rdx) : "0"((long)QPC_ID)	\
			 : "rcx", "r8", "r9", "r11", "xmm0", "xmm1", "xmm2", "xmm3",\
			   "xmm4", "xmm5", "memory", "cc");			\
	return ret;								\
}
VIA_PUSH(via_upcall, __wine_syscall_dispatcher_upcall_push)

#define N 2000000
#define BENCH(label, call) do {						\
	uint64_t c = 0, f = 0; double t;					\
	for (int i = 0; i < N / 10; i++) call;				\
	t = now();								\
	for (int i = 0; i < N; i++) call;					\
	t = (now() - t) / N;							\
	printf("%-8s %6.1f ns   (counter %llu, freq %llu)\n", label, t,	\
	       (unsigned long long)c, (unsigned long long)f);			\
} while (0)

int main(void)
{
	unsigned int a, b, cx, d;
	unsigned char *usd = mmap((void *)0x7ffe0000, 4096, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (usd != (void *)0x7ffe0000) { perror("user_shared_data"); return 1; }
	__cpuid_count(0xd, 1, a, b, cx, d);
	usd[0x285] = 1;				/* PF_XSAVE_ENABLED */
	*(uint32_t *)(usd + 0x3d8) = 7;		/* XState.EnabledFeatures: x87, SSE, AVX */
	*(uint32_t *)(usd + 0x3ec) = (a & 2) ? 2 : 0;	/* XState.CompactionEnabled (XSAVEC) */

	services[QPC_ID] = (void *)fake_NtQueryPerformanceCounter;
	memset(service_args, 0x10, sizeof(service_args));
	tables[0] = (struct service_table){ services, NULL, 0x100, service_args };

	void *self = teb, *frame = kstack + sizeof(kstack) - 0x1000, *ip = &instr_cb, *tp = tables;
	uint32_t mxcsr = 0x1f80;
	memcpy(teb + 0x30, &self, 8);
	memcpy(teb + 0x330, &ip, 8);
	memcpy(teb + 0x33c, &mxcsr, 4);
	teb[0x340] = 1;
	memcpy(teb + 0x370, &tp, 8);
	memcpy(teb + 0x378, &frame, 8);
	if (syscall(SYS_arch_prctl, ARCH_SET_GS, teb)) { perror("ARCH_SET_GS"); return 1; }

	printf("XSAVEC %s, XGETBV(1) %s\n", (a & 2) ? "yes" : "no", (a & 4) ? "yes" : "no");
	unsigned short cw = 0x27f;		/* Windows default x87 control word */
	__asm__ volatile("fldcw %0" :: "m"(cw));
	for (int r = 0; r < 3; r++) {
		BENCH("direct", fake_NtQueryPerformanceCounter(&c, &f));
		xinuse_fast_save = 0;
		BENCH("stub", via_stub(&c, &f));
		BENCH("upcall", via_upcall(&c, &f));
		xinuse_fast_save = 1;
		BENCH("stub+", via_stub(&c, &f));
		BENCH("upcall+", via_upcall(&c, &f));
	}
	return 0;
}
