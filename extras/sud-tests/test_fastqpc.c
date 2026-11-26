/*
 * Runs __wine_syscall_dispatcher_upcall_push from the patched signal_x86_64.c
 * together with Wine's real __wine_syscall_dispatcher, against a fake TEB,
 * user_shared_data page and syscall table, entered the way the kernel's
 * PUSH_RET mode enters it.  Checks the NtQueryPerformanceCounter fast path
 * (WINE_SUD_FAST_QPC=1) against the full dispatcher: result, every Win64
 * non-volatile register including %xmm6-15, flags, stack, selector, and
 * that it falls back to the full path when it must.  bench_qpc.c times it.
 *
 *   make wine WINE=<patched signal_x86_64.c>  (see Makefile)
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
char __wine_sud_fast_qpc;
uint32_t __wine_sud_fast_qpc_id = ~0u;
void *__wine_sud_fast_qpc_func;
void trace_syscall(void) { abort(); }
void trace_sysret(void) { abort(); }

#include "dispatcher.inc"

#define QPC_ID 0x31
#define TICKSPERSEC 10000000ULL
static int qpc_calls;

/* as in dlls/ntdll/unix/sync.c */
long NtQueryPerformanceCounter(uint64_t *counter, uint64_t *freq)
{
	struct timespec ts;

	qpc_calls++;
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
static unsigned char *frame;
static void *instr_cb;

/* register state around one trapped syscall */
struct state { uint64_t rbx, rbp, rsi, rdi, r12, r13, r14, r15, rsp, flags, rax; uint64_t xmm[10][2]; };
struct state in, out;
uint64_t host[8];
uint64_t arg_counter, arg_freq;

/*
 * trap(): load "Windows" register state from `in`, enter the handler like the
 * kernel (return address pushed below the red zone, %rcx = handler, %r11 =
 * RFLAGS), and store the state the caller sees afterwards in `out`.
 */
__asm__(".text\n.globl trap\ntrap:\n"
	"	movq %rbx,host+0(%rip)\n movq %rbp,host+8(%rip)\n movq %r12,host+16(%rip)\n"
	"	movq %r13,host+24(%rip)\n movq %r14,host+32(%rip)\n movq %r15,host+40(%rip)\n"
	"	movq %rsp,host+48(%rip)\n"
	"	movq in+0(%rip),%rbx\n movq in+8(%rip),%rbp\n movq in+16(%rip),%rsi\n movq in+24(%rip),%rdi\n"
	"	movq in+32(%rip),%r12\n movq in+40(%rip),%r13\n movq in+48(%rip),%r14\n movq in+56(%rip),%r15\n"
	"	movdqu in+88(%rip),%xmm6\n movdqu in+104(%rip),%xmm7\n movdqu in+120(%rip),%xmm8\n"
	"	movdqu in+136(%rip),%xmm9\n movdqu in+152(%rip),%xmm10\n movdqu in+168(%rip),%xmm11\n"
	"	movdqu in+184(%rip),%xmm12\n movdqu in+200(%rip),%xmm13\n movdqu in+216(%rip),%xmm14\n"
	"	movdqu in+232(%rip),%xmm15\n"
	"	movq in+64(%rip),%rsp\n"		/* the game's stack */
	"	movq arg_counter(%rip),%r10\n movq arg_freq(%rip),%rdx\n"
	"	leaq 1f(%rip),%rcx\n movq %rcx,-136(%rsp)\n"	/* kernel: push the return address */
	"	pushq in+72(%rip)\n popfq\n"		/* RFLAGS at the syscall (writes below the red zone) */
	"	pushfq\n popq %r11\n"
	"	leaq -136(%rsp),%rsp\n"
	"	movl $0x31,%eax\n"
	"	leaq __wine_syscall_dispatcher_upcall_push(%rip),%rcx\n"
	"	jmp __wine_syscall_dispatcher_upcall_push\n"
	"1:	movq %rax,out+80(%rip)\n"
	"	pushfq\n popq out+72(%rip)\n"
	"	movq %rsp,out+64(%rip)\n"
	"	movq %rbx,out+0(%rip)\n movq %rbp,out+8(%rip)\n movq %rsi,out+16(%rip)\n movq %rdi,out+24(%rip)\n"
	"	movq %r12,out+32(%rip)\n movq %r13,out+40(%rip)\n movq %r14,out+48(%rip)\n movq %r15,out+56(%rip)\n"
	"	movdqu %xmm6,out+88(%rip)\n movdqu %xmm7,out+104(%rip)\n movdqu %xmm8,out+120(%rip)\n"
	"	movdqu %xmm9,out+136(%rip)\n movdqu %xmm10,out+152(%rip)\n movdqu %xmm11,out+168(%rip)\n"
	"	movdqu %xmm12,out+184(%rip)\n movdqu %xmm13,out+200(%rip)\n movdqu %xmm14,out+216(%rip)\n"
	"	movdqu %xmm15,out+232(%rip)\n"
	"	cld\n"
	"	movq host+48(%rip),%rsp\n"
	"	movq host+0(%rip),%rbx\n movq host+8(%rip),%rbp\n movq host+16(%rip),%r12\n"
	"	movq host+24(%rip),%r13\n movq host+32(%rip),%r14\n movq host+40(%rip),%r15\n"
	"	ret\n");
extern void trap(void);

static uint64_t ustack[1024] __attribute__((aligned(16)));
static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void run_one(const char *what, int fast_expected)
{
	uint64_t counter = 0, freq = 0, xcomp;
	int calls = qpc_calls;

	memset(&in, 0, sizeof(in));
	in = (struct state){ 0xb0b0, 0xbbbb, 0x5151, 0xd1d1, 0x1212, 0x1313, 0x1414, 0x1515,
			     (uint64_t)&ustack[800], 0x0ec7 /* CF PF ZF SF IF DF OF */ };
	for (int i = 0; i < 10; i++) { in.xmm[i][0] = 0x6600000000000000ULL + i; in.xmm[i][1] = ~(uint64_t)i; }
	arg_counter = (uint64_t)&counter;
	arg_freq = (uint64_t)&freq;
	memset(frame + 0x2c0, 0xee, 0x40);
	teb[0x340] = 1;
	trap();
	xcomp = *(uint64_t *)(frame + 0x2c8);

	printf("  %s\n", what);
	CHECK(qpc_calls == calls + 1);
	CHECK(out.rax == 0 && counter > 0 && freq == TICKSPERSEC);
	CHECK(out.rbx == in.rbx && out.rbp == in.rbp && out.rsi == in.rsi && out.rdi == in.rdi);
	CHECK(out.r12 == in.r12 && out.r13 == in.r13 && out.r14 == in.r14 && out.r15 == in.r15);
	CHECK(out.rsp == in.rsp);
	CHECK((out.flags & 0xcd5) == (in.flags & 0xcd5));
	CHECK(memcmp(out.xmm, in.xmm, sizeof(in.xmm)) == 0);
	CHECK(teb[0x340] == 1);				/* selector back to BLOCK */
	CHECK(*(uint64_t *)(frame + 0x70) == *(uint64_t *)(frame + 0x70));
	if (fast_expected) CHECK(xcomp == 0);		/* fast path: header cleared, no XSAVEC */
	else CHECK(xcomp != 0 && xcomp != 0xeeeeeeeeeeeeeeeeULL);	/* full dispatcher ran XSAVEC */
}

int main(void)
{
	unsigned int a, b, c, d;
	unsigned char *usd = mmap((void *)0x7ffe0000, 4096, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (usd != (void *)0x7ffe0000) { perror("user_shared_data"); return 1; }
	__cpuid_count(0xd, 1, a, b, c, d);
	if (!(a & 2)) { printf("needs XSAVEC\n"); return 1; }
	usd[0x285] = 1;				/* PF_XSAVE_ENABLED */
	*(uint32_t *)(usd + 0x3d8) = 7;		/* x87, SSE, AVX */
	*(uint32_t *)(usd + 0x3ec) = 2;		/* XSAVEC */

	services[QPC_ID] = (void *)NtQueryPerformanceCounter;
	memset(service_args, 0x10, sizeof(service_args));
	tables[0] = (struct service_table){ services, NULL, 0x100, service_args };

	frame = kstack + sizeof(kstack) - 0x1000;
	void *self = teb, *ip = &instr_cb, *tp = tables, *fp = frame;
	uint32_t mxcsr = 0x1f80;
	memcpy(teb + 0x30, &self, 8);
	memcpy(teb + 0x330, &ip, 8);
	memcpy(teb + 0x33c, &mxcsr, 4);
	memcpy(teb + 0x370, &tp, 8);
	memcpy(teb + 0x378, &fp, 8);
	if (syscall(SYS_arch_prctl, ARCH_SET_GS, teb)) { perror("ARCH_SET_GS"); return 1; }

	run_one("fast path off: full dispatcher", 0);

	__wine_sud_fast_qpc_id = QPC_ID;
	__wine_sud_fast_qpc_func = (void *)NtQueryPerformanceCounter;
	__wine_sud_fast_qpc = 1;
	run_one("fast path on", 1);

	/* falls back to the full dispatcher when it must */
	teb[0x380] = 0;
	uint64_t fsbase;
	syscall(SYS_arch_prctl, ARCH_GET_FS, &fsbase);
	memcpy(teb + 0x320, &fsbase, 8);		/* pthread_teb set: %fs switch needed */
	run_one("fast path on, pthread_teb set: full dispatcher", 0);
	memset(teb + 0x320, 0, 8);
	__wine_sud_fast_qpc = 0;			/* id matches, switch off */
	run_one("id set but fast path off: full dispatcher", 0);
	__wine_sud_fast_qpc = 1;

	printf("%d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
