/*
 * Runs the two Wine SUD upcall entry points the way the kernel enters them
 * and checks what reaches __wine_syscall_dispatcher_prolog_end (or the
 * re-executed syscall when dispatch is disabled).
 *
 *   make wine WINE=<patched signal_x86_64.c>  (see Makefile)
 */
#define _GNU_SOURCE
#include <asm/prctl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int syscall_dispatch_enabled = 1;
char __wine_sud_fast_qpc;			/* fast path off here, see test_fastqpc.c */
unsigned int __wine_sud_fast_qpc_id = ~0u;
void *__wine_sud_fast_qpc_func;
__asm__(".text\n.globl __wine_syscall_dispatcher_return\n__wine_syscall_dispatcher_return:\n ud2\n");

struct regs { uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15, rsp, flags; };
struct regs seen;		/* filled by the stubs below */
uint64_t host_rsp, host_save[6];

struct sim {
	uint64_t handler, push, ret, flags, rax, rbx, rdx, rsi, rdi, r8, r9, r10, sp, entry_sp, rcx;
} sim;

static unsigned char teb[0x1000] __attribute__((aligned(4096)));
static unsigned char frame[0x300] __attribute__((aligned(64)));
static void *instrumentation;
static uint64_t ustack[512] __attribute__((aligned(16)));

#include "handlers.inc"

/*
 * enter_upcall(): switch to the fake user stack and enter sim.handler with
 * the register state the kernel sets up (both contracts), RFLAGS = sim.flags.
 * test_prolog_end / retry_check record every register and return to the
 * caller of enter_upcall().
 */
__asm__(".text\n.globl enter_upcall\nenter_upcall:\n"
	"movq %rbx,host_save+0(%rip)\n movq %rbp,host_save+8(%rip)\n"
	"movq %r12,host_save+16(%rip)\n movq %r13,host_save+24(%rip)\n"
	"movq %r14,host_save+32(%rip)\n movq %r15,host_save+40(%rip)\n"
	"movq %rsp,host_rsp(%rip)\n"
	"pushq sim+24(%rip)\n popfq\n"	/* RFLAGS as SYSRET/IRET restore them (host stack) */
	"movq sim+32(%rip),%rax\n movq sim+40(%rip),%rbx\n movq sim+48(%rip),%rdx\n"
	"movq sim+56(%rip),%rsi\n movq sim+64(%rip),%rdi\n movq sim+72(%rip),%r8\n"
	"movq sim+80(%rip),%r9\n movq sim+88(%rip),%r10\n"
	"movq $0x1212,%r12\n movq $0x1313,%r13\n movq $0x1414,%r14\n movq $0x1515,%r15\n movq $0xbbbb,%rbp\n"
	"movq sim+24(%rip),%r11\n"
	"movq sim+112(%rip),%rcx\n"
	"movq sim+104(%rip),%rsp\n"		/* only movs from here: flags stay as loaded */
	"jmpq *sim+0(%rip)\n");

#define RECORD \
	"movq %rax,seen+0(%rip)\n movq %rbx,seen+8(%rip)\n movq %rcx,seen+16(%rip)\n" \
	"movq %rdx,seen+24(%rip)\n movq %rsi,seen+32(%rip)\n movq %rdi,seen+40(%rip)\n" \
	"movq %rbp,seen+48(%rip)\n movq %r8,seen+56(%rip)\n movq %r9,seen+64(%rip)\n" \
	"movq %r10,seen+72(%rip)\n movq %r11,seen+80(%rip)\n movq %r12,seen+88(%rip)\n" \
	"movq %r13,seen+96(%rip)\n movq %r14,seen+104(%rip)\n movq %r15,seen+112(%rip)\n" \
	"movq %rsp,seen+120(%rip)\n" \
	"leaq -256(%rsp),%rsp\n pushfq\n popq seen+128(%rip)\n" /* stay off the red zone */ \
	"movq host_rsp(%rip),%rsp\n" \
	"movq host_save+0(%rip),%rbx\n movq host_save+8(%rip),%rbp\n" \
	"movq host_save+16(%rip),%r12\n movq host_save+24(%rip),%r13\n" \
	"movq host_save+32(%rip),%r14\n movq host_save+40(%rip),%r15\n" \
	"cld\n ret\n"

__asm__(".text\n.globl test_prolog_end\ntest_prolog_end:\n" RECORD);
/* the "syscall" being retried: 2 bytes at retry_syscall, the return address after it */
__asm__(".text\n.globl retry_syscall\nretry_syscall:\n .byte 0xeb, 0x00\n"  /* jmp to the next insn */
	".globl retry_ret\nretry_ret:\n" RECORD);

extern void enter_upcall(void), __wine_syscall_dispatcher_upcall(void),
	__wine_syscall_dispatcher_upcall_push(void), retry_syscall(void), retry_ret(void);

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static uint64_t f64(unsigned off) { uint64_t v; memcpy(&v, frame + off, 8); return v; }
static uint32_t f32(unsigned off) { uint32_t v; memcpy(&v, frame + off, 4); return v; }

#define S ((uint64_t)&ustack[400])
#define FILL 0xa5a5a5a5a5a5a5a5ULL

static void setup(uint64_t handler, int push, uint64_t ret)
{
	for (int i = 0; i < 512; i++) ustack[i] = FILL;
	memset(frame, 0xcc, sizeof(frame));
	memset(&seen, 0, sizeof(seen));
	teb[0x340] = 1;			/* selector = BLOCK, as in PE code */
	sim = (struct sim){ handler, push, ret, 0x0e47 /* CF PF ZF IF DF OF + bit 1 */, 0x18, 0xb0b0,
			    0xd0d0, 0x5151, 0xd1d1, 0x8888, 0x9999, 0x1010, S, S, ret };
	if (push) {				/* what the kernel does in push-return mode */
		sim.entry_sp = S - 136;
		*(uint64_t *)sim.entry_sp = ret;
		sim.rcx = handler;
	}
}

static int redzone_intact(void)
{
	int ok = 1;
	for (int i = 400 - 16; i < 400; i++)
		if (ustack[i] != FILL) {
			ok = 0;
			printf("    changed: S-%d = %#llx\n", (400 - i) * 8, (unsigned long long)ustack[i]);
		}
	return ok;
}

static void check_prolog_end(const char *what, int instr)
{
	uint64_t ret = sim.ret;
	printf("  %s\n", what);
	CHECK(seen.rsp == S);			/* prolog_end needs %rsp of the syscall */
	CHECK(seen.rcx == (uint64_t)frame);	/* %rcx = syscall frame */
	CHECK(seen.rax == sim.rax);		/* syscall id */
	CHECK(seen.r11 == sim.flags);
	CHECK(seen.rbx == sim.rbx && seen.rdx == sim.rdx && seen.rsi == sim.rsi && seen.rdi == sim.rdi);
	CHECK(seen.r8 == sim.r8 && seen.r9 == sim.r9 && seen.r10 == sim.r10);
	CHECK(seen.r12 == 0x1212 && seen.r13 == 0x1313 && seen.r14 == 0x1414 && seen.r15 == 0x1515 && seen.rbp == 0xbbbb);
	CHECK(f64(0x70) == ret + 0xb);		/* frame->rip (prolog_end subtracts 0xb) */
	CHECK(f64(0x10) == ret);		/* frame->rcx */
	CHECK(f64(0x80) == sim.flags);		/* frame->eflags */
	CHECK(f32(0xb4) == (instr ? 0x10000u : 0));	/* frame->restore_flags */
}

int main(void)
{
	if (syscall(SYS_arch_prctl, ARCH_SET_GS, teb)) { perror("ARCH_SET_GS"); return 1; }
	void *fp = frame, *ip = &instrumentation;
	memcpy(teb + 0x378, &fp, 8);
	memcpy(teb + 0x330, &ip, 8);

	struct { const char *name; uint64_t h; int push; } modes[] = {
		{ "rcx contract (wine-cachyos #37)", (uint64_t)__wine_syscall_dispatcher_upcall, 0 },
		{ "push-return contract (v2)", (uint64_t)__wine_syscall_dispatcher_upcall_push, 1 },
	};

	for (int m = 0; m < 2; m++) {
		printf("%s\n", modes[m].name);
		for (int instr = 0; instr < 2; instr++) {
			syscall_dispatch_enabled = 1;
			instrumentation = instr ? (void *)0x1234 : NULL;
			setup(modes[m].h, modes[m].push, 0x140001000ULL);
			enter_upcall();
			check_prolog_end(instr ? "dispatch, instrumentation callback set" : "dispatch", instr);
			if (modes[m].push) CHECK(redzone_intact());	/* v2 writes nothing below %rsp */
			else CHECK(ustack[398] == 0x140001000ULL);	/* #37 uses 16 bytes below %rsp */
		}

		/* dispatch disabled (Unix .so builtin loaded): native retry */
		printf("  dispatch disabled, retry natively\n");
		syscall_dispatch_enabled = 0;
		instrumentation = NULL;
		setup(modes[m].h, modes[m].push, (uint64_t)retry_ret);
		enter_upcall();
		CHECK(seen.rsp == S);
		CHECK(seen.rax == sim.rax && seen.rdx == sim.rdx && seen.rsi == sim.rsi && seen.rdi == sim.rdi);
		CHECK(seen.r8 == sim.r8 && seen.r9 == sim.r9 && seen.r10 == sim.r10 && seen.rbx == sim.rbx);
		CHECK((seen.flags & 0xcd5) == (sim.flags & 0xcd5));	/* CF PF AF ZF SF DF OF restored */
		CHECK(redzone_intact());		/* SysV red zone of the native caller */
		CHECK(teb[0x340] == 1);			/* selector back to BLOCK */
	}
	printf("%d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
