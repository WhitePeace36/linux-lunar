/*
 * Userspace test (make kernel, see Makefile) of kernel/entry/syscall_user_dispatch.c with mocked kernel
 * APIs. Checks the upcall register contract, the push-return (SYSRET) mode,
 * every fallback to SIGSYS, prctl validation, and the ptrace round trip.
 */
#include <stdio.h>
#include <stdlib.h>
#include <linux/prctl.h>
#include <linux/syscall_user_dispatch.h>

struct task_struct task, *mock_current = &task;
bool mock_ia32;
unsigned long mock_fault_lo, mock_fault_hi;
struct kernel_siginfo mock_last_sig;
int mock_sig_count, mock_exit_sig;

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* the SYSRET conditions of do_syscall_64() in arch/x86/entry/syscall_64.c */
static bool sysret_ok(struct pt_regs *r)
{
	return r->cx == r->ip && r->r11 == r->flags && r->cs == __USER_CS &&
	       r->ss == __USER_DS && r->ip < TASK_SIZE_MAX &&
	       !(r->flags & (X86_EFLAGS_RF | X86_EFLAGS_TF));
}

static char selector;
static unsigned long stack[64];
#define HANDLER 0x7f0000401000UL
#define RET     0x140001234UL	/* address after the trapped syscall */
#define USER_FLAGS 0x246UL

static struct pt_regs regs;

static void reset(void)
{
	clear_syscall_work_syscall_user_dispatch(&task);
	task.syscall_dispatch.selector = NULL;
	task.syscall_dispatch.offset = task.syscall_dispatch.len = 0;
	mock_ia32 = false;
	mock_fault_lo = mock_fault_hi = 0;
	mock_sig_count = mock_exit_sig = 0;
	memset(&mock_last_sig, 0, sizeof(mock_last_sig));
	for (int i = 0; i < 64; i++) stack[i] = 0xa5a5a5a5a5a5a5a5UL;
	regs = (struct pt_regs){ .ip = RET, .sp = (unsigned long)&stack[48], .cx = RET,
				 .r11 = USER_FLAGS, .flags = USER_FLAGS, .ax = -ENOSYS,
				 .orig_ax = 0x31, .cs = __USER_CS, .ss = __USER_DS };
	task.regs = &regs;
	selector = SYSCALL_DISPATCH_FILTER_BLOCK;
}

/* arm dispatch for "everything outside [0x1000,0x2000)" with the selector */
static int arm(unsigned long mode)
{
	return set_syscall_user_dispatch(mode, 0x1000, 0x1000, &selector);
}

int main(void)
{
	struct pt_regs before;
	struct ptrace_sud_config cfg;

	/* 1. original contract (wine-cachyos #37): RCX = return, IRET */
	reset();
	CHECK(set_syscall_user_dispatch_handler(HANDLER, 0) == 0);
	CHECK(arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON) == 0);
	before = regs;
	CHECK(syscall_user_dispatch(&regs) == true);
	CHECK(mock_sig_count == 0 && mock_exit_sig == 0);
	CHECK(regs.ip == HANDLER && regs.cx == RET && regs.r11 == USER_FLAGS);
	CHECK(regs.ax == 0x31);			/* syscall number for the handler */
	CHECK(regs.sp == before.sp);		/* stack untouched */
	CHECK(stack[47] == 0xa5a5a5a5a5a5a5a5UL);
	CHECK(!sysret_ok(&regs));		/* IRET exit, as documented */
	CHECK(syscall_user_dispatch_clear_on_dispatch() == true);  /* exit work skipped */

	/* 2. push-return contract: return address on the stack, SYSRET */
	reset();
	CHECK(set_syscall_user_dispatch_handler(HANDLER, PR_SYS_DISPATCH_HANDLER_PUSH_RET) == 0);
	CHECK(arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON) == 0);
	before = regs;
	CHECK(syscall_user_dispatch(&regs) == true);
	CHECK(mock_sig_count == 0);
	CHECK(regs.ip == HANDLER && regs.cx == HANDLER && regs.r11 == USER_FLAGS);
	CHECK(regs.sp == before.sp - 128 - 8);
	CHECK(*(unsigned long *)regs.sp == RET);
	for (int i = 32; i < 48; i++)		/* the 128-byte red zone below the old RSP */
		CHECK(stack[i] == 0xa5a5a5a5a5a5a5a5UL);
	CHECK(sysret_ok(&regs));		/* now leaves through SYSRET */
	CHECK(regs.ax == 0x31);

	/* 3. push-return but the stack faults: SIGSYS, registers unchanged */
	reset();
	CHECK(set_syscall_user_dispatch_handler(HANDLER, PR_SYS_DISPATCH_HANDLER_PUSH_RET) == 0);
	CHECK(arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON) == 0);
	mock_fault_lo = (unsigned long)&stack[0];
	mock_fault_hi = (unsigned long)&stack[64];
	before = regs;
	CHECK(syscall_user_dispatch(&regs) == true);
	CHECK(mock_sig_count == 1 && mock_last_sig.si_signo == SIGSYS);
	CHECK(mock_last_sig.si_code == SYS_USER_DISPATCH);
	CHECK(mock_last_sig.si_call_addr == (void *)RET && mock_last_sig.si_syscall == 0x31);
	CHECK(regs.ip == before.ip && regs.sp == before.sp && regs.cx == before.cx);

	/* 4. TF set and ia32 syscalls keep SIGSYS in both modes */
	for (unsigned long fl = 0; fl <= PR_SYS_DISPATCH_HANDLER_PUSH_RET; fl++) {
		reset();
		CHECK(set_syscall_user_dispatch_handler(HANDLER, fl) == 0);
		CHECK(arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON) == 0);
		regs.flags |= X86_EFLAGS_TF;
		CHECK(syscall_user_dispatch(&regs) && mock_sig_count == 1 && regs.ip == RET);
		reset();
		CHECK(set_syscall_user_dispatch_handler(HANDLER, fl) == 0);
		CHECK(arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON) == 0);
		mock_ia32 = true;
		CHECK(syscall_user_dispatch(&regs) && mock_sig_count == 1 && regs.ip == RET);
		reset();
		CHECK(set_syscall_user_dispatch_handler(HANDLER, fl) == 0);
		CHECK(arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON) == 0);
		regs.cs = 0x23;			/* 32-bit code segment */
		CHECK(syscall_user_dispatch(&regs) && mock_sig_count == 1 && regs.ip == RET);
	}

	/* 5. allowed syscalls pass through untouched */
	reset();
	CHECK(set_syscall_user_dispatch_handler(HANDLER, PR_SYS_DISPATCH_HANDLER_PUSH_RET) == 0);
	CHECK(arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON) == 0);
	selector = SYSCALL_DISPATCH_FILTER_ALLOW;
	before = regs;
	CHECK(syscall_user_dispatch(&regs) == false);
	CHECK(memcmp(&regs, &before, sizeof(regs)) == 0 && mock_sig_count == 0);
	selector = SYSCALL_DISPATCH_FILTER_BLOCK;
	regs.ip = 0x1800;			/* inside the exclusive range */
	CHECK(syscall_user_dispatch(&regs) == false && mock_sig_count == 0);

	/* 6. inclusive upcall mode */
	reset();
	CHECK(set_syscall_user_dispatch_handler(HANDLER, 0) == 0);
	CHECK(set_syscall_user_dispatch(PR_SYS_DISPATCH_UPCALL_INCLUSIVE_ON, 0x140000000UL,
					0x10000, &selector) == 0);
	CHECK(syscall_user_dispatch(&regs) == true && regs.ip == HANDLER && mock_sig_count == 0);

	/* 7. plain SIGSYS mode is unchanged even with a handler registered */
	reset();
	CHECK(set_syscall_user_dispatch_handler(HANDLER, PR_SYS_DISPATCH_HANDLER_PUSH_RET) == 0);
	CHECK(arm(PR_SYS_DISPATCH_ON) == 0);
	CHECK(syscall_user_dispatch(&regs) && mock_sig_count == 1 && regs.ip == RET);

	/* 8. prctl validation */
	reset();
	CHECK(arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON) == -EINVAL);	/* no handler */
	CHECK(set_syscall_user_dispatch_handler(TASK_SIZE_MAX, 0) == -EINVAL);
	CHECK(set_syscall_user_dispatch_handler(~0UL, 0) == -EINVAL);
	CHECK(set_syscall_user_dispatch_handler(HANDLER, 2) == -EINVAL);	/* unknown flag */
	CHECK(set_syscall_user_dispatch_handler(0, PR_SYS_DISPATCH_HANDLER_PUSH_RET) == -EINVAL);
	CHECK(set_syscall_user_dispatch_handler(HANDLER, 0) == 0);
	CHECK(arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON) == 0);
	CHECK(set_syscall_user_dispatch_handler(0, 0) == -EBUSY);	/* armed */
	CHECK(set_syscall_user_dispatch_handler(HANDLER + 16, PR_SYS_DISPATCH_HANDLER_PUSH_RET) == 0);
	CHECK(set_syscall_user_dispatch(PR_SYS_DISPATCH_OFF, 0, 0, NULL) == 0);
	CHECK(set_syscall_user_dispatch_handler(0, 0) == 0);		/* disarmed: ok */
	CHECK(task.syscall_dispatch.handler == 0 && task.syscall_dispatch.handler_flags == 0);

	/* 9. clone/exec reset clears the handler and its flags */
	reset();
	CHECK(set_syscall_user_dispatch_handler(HANDLER, PR_SYS_DISPATCH_HANDLER_PUSH_RET) == 0);
	CHECK(arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON) == 0);
	clear_syscall_work_syscall_user_dispatch(&task);
	CHECK(task.syscall_dispatch.handler == 0 && task.syscall_dispatch.handler_flags == 0);
	CHECK(!task.syscall_dispatch.on_upcall && !test_task_syscall_work(&task, SYSCALL_USER_DISPATCH));

	/* 10. ptrace get/set round trip */
	reset();
	CHECK(set_syscall_user_dispatch_handler(HANDLER, PR_SYS_DISPATCH_HANDLER_PUSH_RET) == 0);
	CHECK(set_syscall_user_dispatch(PR_SYS_DISPATCH_UPCALL_INCLUSIVE_ON, 0x140000000UL,
					0x10000, &selector) == 0);
	CHECK(syscall_user_dispatch_get_config(&task, sizeof(cfg), &cfg) == 0);
	CHECK(cfg.mode == PR_SYS_DISPATCH_UPCALL_INCLUSIVE_ON);
	CHECK(cfg.offset == 0x140000000UL && cfg.len == 0x10000);	/* as armed */
	unsigned long off = task.syscall_dispatch.offset, len = task.syscall_dispatch.len;
	CHECK(syscall_user_dispatch_set_config(&task, sizeof(cfg), &cfg) == 0);
	CHECK(task.syscall_dispatch.offset == off && task.syscall_dispatch.len == len);
	CHECK(task.syscall_dispatch.on_upcall);
	clear_syscall_work_syscall_user_dispatch(&task);	/* restored task without handler */
	CHECK(syscall_user_dispatch_set_config(&task, sizeof(cfg), &cfg) == -EINVAL);

	/* 11. round trips of the other modes */
	reset();
	CHECK(set_syscall_user_dispatch_handler(HANDLER, 0) == 0);
	CHECK(arm(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON) == 0);
	CHECK(syscall_user_dispatch_get_config(&task, sizeof(cfg), &cfg) == 0);
	CHECK(cfg.mode == PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON && cfg.offset == 0x1000 && cfg.len == 0x1000);
	CHECK(syscall_user_dispatch_set_config(&task, sizeof(cfg), &cfg) == 0);
	reset();
	CHECK(set_syscall_user_dispatch(PR_SYS_DISPATCH_INCLUSIVE_ON, 0, 0x5000, &selector) == 0);
	CHECK(syscall_user_dispatch_get_config(&task, sizeof(cfg), &cfg) == 0);
	CHECK(cfg.mode == PR_SYS_DISPATCH_INCLUSIVE_ON && cfg.offset == 0 && cfg.len == 0x5000);
	CHECK(syscall_user_dispatch_set_config(&task, sizeof(cfg), &cfg) == 0);
	regs.ip = 0x4000;			/* inside the inclusive range: dispatched */
	CHECK(syscall_user_dispatch(&regs) == true && mock_sig_count == 1);
	reset();
	CHECK(arm(PR_SYS_DISPATCH_ON) == 0);
	CHECK(syscall_user_dispatch_get_config(&task, sizeof(cfg), &cfg) == 0);
	CHECK(cfg.mode == PR_SYS_DISPATCH_ON && cfg.offset == 0x1000 && cfg.len == 0x1000);
	CHECK(syscall_user_dispatch_set_config(&task, sizeof(cfg), &cfg) == 0);

	printf("%d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
