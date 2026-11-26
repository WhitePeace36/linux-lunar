/* Minimal stand-ins for the kernel APIs kernel/entry/syscall_user_dispatch.c uses. */
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CONFIG_SYSCALL_USER_DISPATCH 1

#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define __is_defined(x) ___is_defined(x)
#define ___is_defined(val) ____is_defined(__ARG_PLACEHOLDER_##val)
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define IS_ENABLED(option) __is_defined(option)

#define __user
#define __read_mostly
#ifndef __always_inline
#define __always_inline inline
#endif
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define fallthrough __attribute__((__fallthrough__))
typedef uint64_t u64;

#include <linux/types.h>
#include <linux/syscall_user_dispatch_types.h>

struct pt_regs {
	unsigned long ip, sp, cx, r11, flags, ax, orig_ax, cs, ss;
};

#define SYSCALL_WORK_SYSCALL_USER_DISPATCH 1UL
struct task_struct {
	struct syscall_user_dispatch syscall_dispatch;
	unsigned long syscall_work;
	struct pt_regs *regs;
};
extern struct task_struct *mock_current;
#define current mock_current
#define set_task_syscall_work(t, fl) ((t)->syscall_work |= SYSCALL_WORK_##fl)
#define clear_task_syscall_work(t, fl) ((t)->syscall_work &= ~SYSCALL_WORK_##fl)
#define test_task_syscall_work(t, fl) (!!((t)->syscall_work & SYSCALL_WORK_##fl))

#define X86_EFLAGS_TF 0x100UL
#define X86_EFLAGS_RF 0x10000UL
#define __USER_CS 0x33
#define __USER_DS 0x2b
#define user_64bit_mode(r) ((r)->cs == __USER_CS)
extern bool mock_ia32;
#define in_ia32_syscall() (mock_ia32)

#define TASK_SIZE_MAX ((1UL << 47) - 4096)
#define untagged_addr(x) (x)
#define access_ok(p, n) ((unsigned long)(p) + (n) <= TASK_SIZE_MAX)

/* user memory: real host memory, except an address range that "faults" */
extern unsigned long mock_fault_lo, mock_fault_hi;
static inline bool mock_faults(const void *p)
{
	unsigned long a = (unsigned long)p;
	return a >= mock_fault_lo && a < mock_fault_hi;
}
#define __get_user(x, p) (mock_faults(p) ? -EFAULT : ((x) = *(p), 0))
#define get_user(x, p) __get_user(x, p)
#define put_user(x, p) (mock_faults(p) ? -EFAULT : (*(p) = (x), 0))
static inline unsigned long copy_to_user(void *d, const void *s, size_t n) { memcpy(d, s, n); return 0; }
static inline unsigned long copy_from_user(void *d, const void *s, size_t n) { memcpy(d, s, n); return 0; }

#define SIGSEGV 11
#define SIGSYS 31
#define SYS_USER_DISPATCH 2
struct kernel_siginfo {
	int si_signo, si_code, si_errno, si_syscall;
	unsigned int si_arch;
	void *si_call_addr;
};
extern struct kernel_siginfo mock_last_sig;
extern int mock_sig_count, mock_exit_sig;
static inline void clear_siginfo(struct kernel_siginfo *i) { memset(i, 0, sizeof(*i)); }
static inline int force_sig_info(struct kernel_siginfo *i) { mock_last_sig = *i; mock_sig_count++; return 0; }
static inline void force_exit_sig(int sig) { mock_exit_sig = sig; }
#define KSTK_EIP(t) ((t)->regs->ip)
#define instruction_pointer(r) ((r)->ip)

#define syscall_get_arch(t) 0xc000003eU
#define syscall_get_nr(t, r) ((int)(r)->orig_ax)
#define syscall_rollback(t, r) ((r)->ax = (r)->orig_ax)
extern bool mock_vdso_sigreturn;
extern int mock_vdso_checks;
static inline bool arch_syscall_is_vdso_sigreturn(struct pt_regs *r) { mock_vdso_checks++; return mock_vdso_sigreturn; }
#define scoped_user_read_access(p, elbl) for (int __once = 1; __once; __once = 0)
#define unsafe_get_user(x, p, elbl) do { if (mock_faults(p)) goto elbl; (x) = *(p); } while (0)

struct ptrace_sud_config { __u64 mode, selector, offset, len; };
