/*
 * sudtrap: cost of one Syscall User Dispatch trap, the way Wine pays it for
 * every Windows-style syscall stub that reaches a real `syscall`.
 *
 *   native     getpid through SYSCALL/SYSRET (the floor)
 *   sigsys     SUD -> SIGSYS -> handler -> rt_sigreturn (stock kernels)
 *   upcall     SUD -> handler, return address in %rcx, IRET exit (patch 0014)
 *   upcall-push SUD -> handler entered like a CALL, SYSRET exit (patch 0014)
 *
 * Each mode also checks that the trapped syscall comes back with the
 * emulated result and every other register intact.
 *
 *   gcc -O2 -o tools/sudtrap tools/sudtrap.c && tools/sudtrap [iterations]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#ifndef PR_SET_SYSCALL_USER_DISPATCH
#define PR_SET_SYSCALL_USER_DISPATCH 59
#define PR_SYS_DISPATCH_OFF 0
#define PR_SYS_DISPATCH_ON 1
#define SYSCALL_DISPATCH_FILTER_ALLOW 0
#define SYSCALL_DISPATCH_FILTER_BLOCK 1
#endif
#define PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON 0x80000003UL
#define PR_SET_SYSCALL_USER_DISPATCH_HANDLER 0x53554448
#define PR_SYS_DISPATCH_HANDLER_PUSH_RET (1UL << 0)

#define FAKE_NR 0x1d	/* the "Windows" syscall number, any value works */
#define RESULT 0x4242	/* what the handlers return for it */

static volatile char selector = SYSCALL_DISPATCH_FILTER_ALLOW;

/* upcall handlers: emulate the syscall (return RESULT) and go back */
extern char upcall_rcx[], upcall_push[];
__asm__(".text\n"
	"upcall_rcx:\n"
	"	endbr64\n"
	"	movl $0x4242,%eax\n"
	"	jmp *%rcx\n"			/* %rcx = address after the syscall */
	"upcall_push:\n"
	"	endbr64\n"
	"	movl $0x4242,%eax\n"
	"	popq %rcx\n"			/* address after the syscall */
	"	leaq 128(%rsp),%rsp\n"		/* skip the red zone the kernel left */
	"	jmp *%rcx\n");

static void sigsys(int sig, siginfo_t *si, void *uc_)
{
	ucontext_t *uc = uc_;

	uc->uc_mcontext.gregs[REG_RAX] = RESULT;
	selector = SYSCALL_DISPATCH_FILTER_ALLOW;	/* let rt_sigreturn through */
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* one trapped syscall; returns 0 if result and registers are right */
static int trap_once(void)
{
	uint64_t rax = FAKE_NR, rdi = 0x1111, rsi = 0x2222, rdx = 0x3333;
	uint64_t r8v = 0x8888, r9v = 0x9999, r10v = 0xaaaa;
	register uint64_t r8 __asm__("r8") = r8v, r9 __asm__("r9") = r9v, r10 __asm__("r10") = r10v;

	selector = SYSCALL_DISPATCH_FILTER_BLOCK;
	__asm__ volatile("syscall"
			 : "+a"(rax), "+D"(rdi), "+S"(rsi), "+d"(rdx), "+r"(r8), "+r"(r9), "+r"(r10)
			 : : "rcx", "r11", "memory");
	selector = SYSCALL_DISPATCH_FILTER_ALLOW;
	return !(rax == RESULT && rdi == 0x1111 && rsi == 0x2222 && rdx == 0x3333 &&
		 r8 == r8v && r9 == r9v && r10 == r10v);
}

static double run(long iters)
{
	double t;
	long i;

	for (i = 0; i < iters / 100; i++)
		trap_once();
	t = now();
	for (i = 0; i < iters; i++)
		trap_once();
	return (now() - t) / iters;
}

static int arm(unsigned long mode)
{
	return prctl(PR_SET_SYSCALL_USER_DISPATCH, mode, 0UL, 0UL, &selector);
}

static void disarm(void)
{
	selector = SYSCALL_DISPATCH_FILTER_ALLOW;
	prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_OFF, 0UL, 0UL, 0UL);
	prctl(PR_SET_SYSCALL_USER_DISPATCH_HANDLER, 0UL, 0UL, 0UL, 0UL);
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 2000000;
	struct sigaction sa = { .sa_sigaction = sigsys, .sa_flags = SA_SIGINFO };
	double native, t;
	long i;
	int bad = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	sigaction(SIGSYS, &sa, NULL);

	t = now();
	for (i = 0; i < iters; i++)
		syscall(SYS_getpid);
	native = (now() - t) / iters;
	printf("%-12s %8.1f ns\n", "native", native);

	if (arm(PR_SYS_DISPATCH_ON)) {
		printf("no Syscall User Dispatch: %s\n", strerror(errno));
		return 1;
	}
	if (trap_once()) { printf("sigsys: wrong result or registers\n"); bad = 1; }
	t = run(iters);
	printf("%-12s %8.1f ns  (%.1fx native)\n", "sigsys", t, t / native);
	disarm();

	struct { const char *name; void *h; unsigned long flags; } m[] = {
		{ "upcall", upcall_rcx, 0 },
		{ "upcall-push", upcall_push, PR_SYS_DISPATCH_HANDLER_PUSH_RET },
	};
	for (int k = 0; k < 2; k++) {
		if (prctl(PR_SET_SYSCALL_USER_DISPATCH_HANDLER, (unsigned long)m[k].h, m[k].flags, 0UL, 0UL)) {
			printf("%-12s not supported by this kernel (%s)\n", m[k].name, strerror(errno));
			continue;
		}
		if (arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON)) {
			printf("%-12s cannot arm: %s\n", m[k].name, strerror(errno));
			disarm();
			bad = 1;
			continue;
		}
		if (trap_once()) { printf("%s: wrong result or registers\n", m[k].name); bad = 1; }
		t = run(iters);
		printf("%-12s %8.1f ns  (%.1fx native)\n", m[k].name, t, t / native);
		disarm();
	}
	return bad;
}
