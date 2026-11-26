#include <stdio.h>
#include <linux/prctl.h>
#include <linux/syscall_user_dispatch.h>
struct task_struct task, *mock_current = &task; bool mock_ia32;
unsigned long mock_fault_lo, mock_fault_hi; struct kernel_siginfo mock_last_sig; int mock_sig_count, mock_exit_sig;
static char sel = 1;
int main(void) {
	int a = set_syscall_user_dispatch_handler(0x401000, 0);
	int b = set_syscall_user_dispatch(PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON, 0, 0, &sel);
	int c = set_syscall_user_dispatch(PR_SYS_DISPATCH_ON, 0, 0, &sel);
	struct pt_regs r = { .ip = 0x5000, .cs = 0x33, .orig_ax = 39 }; task.regs = &r;
	bool d = syscall_user_dispatch(&r);
	printf("non-x86: handler=%d upcall-arm=%d plain-arm=%d trap->sigsys=%d\n", a, b, c, d && mock_sig_count == 1);
	return !(a == -EINVAL && b == -EINVAL && c == 0 && d && mock_sig_count == 1);
}
