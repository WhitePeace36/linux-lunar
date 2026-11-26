/*
 * Checks the XINUSE-guided save in __wine_syscall_dispatcher against xsavec64.
 *
 * Runs Wine's real dispatcher (extracted from the patched signal_x86_64.c)
 * against a fake TEB, user_shared_data and syscall table, in several x87 /
 * SSE / AVX states, once with xinuse_fast_save = 0 (xsavec64) and once with
 * it on.  For each state it compares the parts of the syscall frame that
 * NtGetContextThread reports (see get_thread_context in signal_x86_64.c:
 * xstate.Mask decides which parts count), and the registers the caller sees
 * after the syscall.  bench_qpc.c has the timing.
 *
 *   make wine WINE=<patched signal_x86_64.c>  (see Makefile)
 */
#define _GNU_SOURCE
#include <cpuid.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
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

#define ID 0x31
static unsigned char kstack[0x40000] __attribute__((aligned(64)));
static unsigned char *frame;
static unsigned char snap[0x1000] __attribute__((aligned(64)));

/* the "syscall": snapshot the frame the dispatcher just filled */
long fake_syscall(void)
{
	memcpy(snap, frame, 0x400);
	return 0;
}

struct service_table { void **table; void *counters; uint64_t limit; unsigned char *args; };
static void *services[0x100];
static unsigned char service_args[0x100];
static struct service_table tables[4];
static unsigned char teb[0x2000] __attribute__((aligned(4096)));
static void *instr_cb;

/* caller-visible state around one syscall */
struct state { uint64_t rbx, rbp, rsi, rdi, r12, r13, r14, r15, rsp, flags, rax; uint64_t xmm[16][2]; uint64_t ymmh[16][2]; };
struct state in, out;
uint64_t host[8], xinuse_before;
int setup_mode;	/* 0 SSE only, 1 x87 CW 0x27f, 2 AVX upper live, 3 x87 value pushed */

/*
 * call_syscall(): load `in`, put the x87/AVX state into setup_mode, call
 * Wine's dispatcher like a Wine syscall stub does, store what comes back.
 */
__asm__(".text\n.globl call_syscall\ncall_syscall:\n"
	"	movq %rbx,host+0(%rip)\n movq %rbp,host+8(%rip)\n movq %r12,host+16(%rip)\n"
	"	movq %r13,host+24(%rip)\n movq %r14,host+32(%rip)\n movq %r15,host+40(%rip)\n"
	"	movq %rsp,host+48(%rip)\n"
	"	vzeroupper\n fninit\n"
	"	cmpl $1,setup_mode(%rip)\n jne 1f\n fldcw cw27f(%rip)\n"
	"1:	cmpl $3,setup_mode(%rip)\n jne 1f\n fld1\n"
	"1:	leaq in+88(%rip),%rax\n"
	"	movdqu 0(%rax),%xmm0\n movdqu 16(%rax),%xmm1\n movdqu 32(%rax),%xmm2\n movdqu 48(%rax),%xmm3\n"
	"	movdqu 64(%rax),%xmm4\n movdqu 80(%rax),%xmm5\n movdqu 96(%rax),%xmm6\n movdqu 112(%rax),%xmm7\n"
	"	movdqu 128(%rax),%xmm8\n movdqu 144(%rax),%xmm9\n movdqu 160(%rax),%xmm10\n movdqu 176(%rax),%xmm11\n"
	"	movdqu 192(%rax),%xmm12\n movdqu 208(%rax),%xmm13\n movdqu 224(%rax),%xmm14\n movdqu 240(%rax),%xmm15\n"
	"	cmpl $2,setup_mode(%rip)\n jne 1f\n"
	"	vinsertf128 $1,%xmm7,%ymm6,%ymm6\n"	/* upper half of ymm6 = xmm7 */
	"1:	movl $1,%ecx\n xgetbv\n movl %eax,xinuse_before(%rip)\n"
	"	movq in+0(%rip),%rbx\n movq in+8(%rip),%rbp\n movq in+16(%rip),%rsi\n movq in+24(%rip),%rdi\n"
	"	movq in+32(%rip),%r12\n movq in+40(%rip),%r13\n movq in+48(%rip),%r14\n movq in+56(%rip),%r15\n"
	"	movl $0x31,%eax\n"
	"	leaq 2f+0xb(%rip),%r11\n pushq %r11\n"	/* dispatcher returns to (pushed - 0xb) */
	"	jmp __wine_syscall_dispatcher\n"
	"2:	movq %rax,out+80(%rip)\n"
	"	movq %rsp,out+64(%rip)\n"
	"	movq %rbx,out+0(%rip)\n movq %rbp,out+8(%rip)\n movq %rsi,out+16(%rip)\n movq %rdi,out+24(%rip)\n"
	"	movq %r12,out+32(%rip)\n movq %r13,out+40(%rip)\n movq %r14,out+48(%rip)\n movq %r15,out+56(%rip)\n"
	"	leaq out+88(%rip),%rax\n"
	"	movdqu %xmm6,96(%rax)\n movdqu %xmm7,112(%rax)\n movdqu %xmm8,128(%rax)\n movdqu %xmm9,144(%rax)\n"
	"	movdqu %xmm10,160(%rax)\n movdqu %xmm11,176(%rax)\n movdqu %xmm12,192(%rax)\n movdqu %xmm13,208(%rax)\n"
	"	movdqu %xmm14,224(%rax)\n movdqu %xmm15,240(%rax)\n"
	"	vzeroupper\n fninit\n"
	"	movq host+48(%rip),%rsp\n"
	"	movq host+0(%rip),%rbx\n movq host+8(%rip),%rbp\n movq host+16(%rip),%r12\n"
	"	movq host+24(%rip),%r13\n movq host+32(%rip),%r14\n movq host+40(%rip),%r15\n"
	"	ret\n"
	".data\ncw27f: .word 0x27f\n.text\n");
extern void call_syscall(void);

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("    FAIL line %d: %s ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t q(const unsigned char *b, int off) { uint64_t v; memcpy(&v, b + off, 8); return v; }
static uint16_t w(const unsigned char *b, int off) { uint16_t v; memcpy(&v, b + off, 2); return v; }

/* run state `mode` with fast save on/off, frame snapshot into `dst` */
static void run(int mode, int fast, unsigned char *dst, struct state *o, uint64_t *xin)
{
	static uint64_t stack[512] __attribute__((aligned(16)));
	memset(&in, 0, sizeof(in));
	in = (struct state){ 0xb0b0, 0xbbbb, 0x5151, 0xd1d1, 0x1212, 0x1313, 0x1414, 0x1515, (uint64_t)&stack[400] };
	for (int i = 0; i < 16; i++) { in.xmm[i][0] = 0x1100000000000000ULL * (i + 1) + 7; in.xmm[i][1] = ~(uint64_t)i; }
	setup_mode = mode;
	xinuse_fast_save = fast;
	memset(frame + 0xc0, 0xcc, 0x340);	/* poison the xsave area */
	call_syscall();
	memcpy(dst, snap, 0x400);
	*o = out;
	*xin = xinuse_before;
}

/* what NtGetContextThread would report from this frame (get_thread_context) */
static void compare(const char *name, int mode, int expect_fast)
{
	unsigned char a[0x400], b[0x400];
	struct state oa, ob;
	uint64_t xa, xb;

	run(mode, 0, a, &oa, &xa);
	run(mode, 1, b, &ob, &xb);
	uint64_t mask_a = q(a, 0x2c0), mask_b = q(b, 0x2c0);
	printf("%-34s XINUSE %#5llx  Mask xsavec %#llx fast %#llx\n", name,
	       (unsigned long long)xb, (unsigned long long)mask_a, (unsigned long long)mask_b);
	CHECK(mask_a == mask_b, "xstate.Mask");
	CHECK(q(a, 0x2c8) == q(b, 0x2c8), "CompactionMask %#llx vs %#llx", (unsigned long long)q(a, 0x2c8), (unsigned long long)q(b, 0x2c8));
	CHECK(q(a, 0xd8) == q(b, 0xd8), "MxCsr/MxCsr_Mask %#llx vs %#llx", (unsigned long long)q(a, 0xd8), (unsigned long long)q(b, 0xd8));
	if (mask_a & 1) {	/* x87 reported from the frame */
		CHECK(w(a, 0xc0) == w(b, 0xc0), "ControlWord %#x vs %#x", w(a, 0xc0), w(b, 0xc0));
		CHECK(w(a, 0xc2) == w(b, 0xc2), "StatusWord %#x vs %#x", w(a, 0xc2), w(b, 0xc2));
		CHECK(a[0xc4] == b[0xc4], "TagWord %#x vs %#x", a[0xc4], b[0xc4]);
		int st_same = !memcmp(a + 0xe0, b + 0xe0, 128), ip_same = !memcmp(a + 0xc6, b + 0xc6, 0x12);
		if (!st_same) printf("    note: empty x87 registers differ (xsavec stores stale contents, fast path zeros)\n");
		if (!ip_same) printf("    note: x87 FOP/FIP/FDP differ (xsavec stores them, fast path zeros)\n");
	}
	if (mask_a & 2) CHECK(!memcmp(a + 0x160, b + 0x160, 256), "XmmRegisters");
	if (mask_a & 4) CHECK(!memcmp(a + 0x2c0 + 0x40, b + 0x2c0 + 0x40, 256), "AVX upper halves");
	CHECK(!memcmp(&oa, &ob, offsetof(struct state, ymmh)), "caller-visible registers after the syscall");
	CHECK(ob.rbx == in.rbx && ob.rbp == in.rbp && ob.rsi == in.rsi && ob.rdi == in.rdi && ob.r12 == in.r12 && ob.r13 == in.r13 && ob.r14 == in.r14 && ob.r15 == in.r15 && !memcmp(ob.xmm[6], in.xmm[6], 160), "non-volatile registers");
	printf("    fast path %s\n", expect_fast ? "expected" : "not expected (falls back to xsavec)");
}

int main(void)
{
	unsigned int a, b, c, d;
	unsigned char *usd = mmap((void *)0x7ffe0000, 4096, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (usd != (void *)0x7ffe0000) { perror("user_shared_data"); return 1; }
	__cpuid_count(0xd, 1, a, b, c, d);
	if (!(a & 2) || !(a & 4)) { printf("needs XSAVEC and XGETBV(1)\n"); return 1; }
	usd[0x285] = 1;				/* PF_XSAVE_ENABLED */
	*(uint32_t *)(usd + 0x3d8) = 7;		/* x87, SSE, AVX */
	*(uint32_t *)(usd + 0x3ec) = 2;		/* XSAVEC */
	struct { unsigned char b[512]; } fx __attribute__((aligned(16)));
	__asm__ volatile("fxsave64 %0" : "=m"(fx));
	memcpy(&xinuse_mxcsr_mask, fx.b + 28, 4);

	services[ID] = (void *)fake_syscall;
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

	compare("SSE only (x87 initial)", 0, 1);
	compare("x87 CW 0x27f (Windows default)", 1, 1);
	compare("AVX upper halves live", 2, 0);
	compare("x87 register stack not empty", 3, 0);

	printf("%d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
