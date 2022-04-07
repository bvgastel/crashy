#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unwind.h>

#ifdef __APPLE__
#define _XOPEN_SOURCE	// macOS needs this (however: The deprecated ucontext routines require _XOPEN_SOURCE to be defined)
#endif
#include <ucontext.h>

#include <stdio.h>

#if __FreeBSD__
// FreeBSD does not unwind (with _Unwind_*) after signal to regular stack (only signal handler stack)
// for regular stack traces we need Unwind, because C++ exception handling in std::set_terminate will crash on GCC 9.4
#define MANUAL
#include <sys/types.h>
#include <sys/sysctl.h>
#include <errno.h>
#endif

#ifdef MANUAL

// layout specific for x86 and amd64
struct stack_frame {
	struct stack_frame* next;
	void* ret;
};

int StackTraceAlt(void* ip, void* _frame, bool (*print)(void* pc, void* arg), void* arg, int max_size) {
	if (!_frame || max_size <= 0)
			return max_size;
	struct stack_frame* frame = static_cast<struct stack_frame*>(_frame);
	if (ip) {
#ifdef __cplusplus
		ip = reinterpret_cast<void*>(uintptr_t(ip)-1);
#else
		ip = ip-1;
#endif
		if (print(ip, arg))
			return max_size-1;
	}
	// stack should grow down, but it is not always the case (with signal handlers, alternative stacks, C++ uncaught exceptions)
	return StackTraceAlt(frame->ret, frame->next, print, arg, max_size-1);
}

int StackTraceAlt(bool (*report)(void *pc, void* arg), void* reportArg, int max_size) {
	struct stack_frame* frame = nullptr;
	// __builtin_return_address and __builtin_frame_address can be useful
#if defined(__amd64__)
	__asm__("mov %%rbp, %0" : "=r" (frame));
#elif defined(__i386__)
	__asm__("mov %%ebp, %0" : "=r" (frame));
#else
#error "manual stack unwinding not supported"
#endif
	// stack should grow down, but it is not always the case (with signal handlers, alternative stacks, C++ uncaught exceptions)
	return StackTraceAlt(frame->ret, frame->next, report, reportArg, max_size);
}

#endif

struct unwind_helper {
	int left;
	bool (*report)(void*pc, void* arg);
	void* reportArg;
};
static _Unwind_Reason_Code backtrace_helper(struct _Unwind_Context *ctx, void *a) {
	struct unwind_helper* arg = static_cast<struct unwind_helper*>(a);
	if (arg->left-- <= 0)
		return _URC_END_OF_STACK;
#ifdef HAVE_GETIPINFO
	int ip_before_insn = 0;
	unsigned long ip = _Unwind_GetIPInfo(ctx, &ip_before_insn);
	if (!ip)
		return _URC_END_OF_STACK;
	if (!ip_before_insn)
		--ip;
#else
	unsigned long ip = _Unwind_GetIP(ctx);
	if (!ip)
		return _URC_END_OF_STACK;
	--ip;
#endif
	if (arg->report(reinterpret_cast<void*>(ip), arg->reportArg))
		return _URC_END_OF_STACK;
	return _URC_NO_REASON;
}

int StackTrace(bool (*report)(void *pc, void* arg), void* reportArg, int max_size) {
	struct unwind_helper arg = {
		.left = max_size,
		.report = report,
		.reportArg = reportArg
	};
	_Unwind_Backtrace(backtrace_helper, &arg);
	return arg.left;
}

void StackTraceSignal(bool (*report)(void* pc, void* arg), void* reportArg, void* _ucxt [[maybe_unused]], int max_size) {
#ifdef MANUAL
	ucontext_t* ucxt = static_cast<ucontext_t*>(_ucxt);

#if defined(__linux__) && defined(__arm__)
	void* ip = (void*)ucxt->uc_mcontext.arm_ip;
	void* bp = (void*)ucxt->uc_mcontext.arm_fp;
#elif defined(__linux__) && defined(__amd64__)
	void* ip = (void*)ucxt->uc_mcontext.gregs[REG_RIP];
	void* bp = (void*)ucxt->uc_mcontext.gregs[REG_RBP];
#elif defined(__linux__) && defined(__i386__)
	void* ip = (void*)ucxt->uc_mcontext.gregs[REG_EIP];
	void* bp = (void*)ucxt->uc_mcontext.gregs[REG_EBP];
#elif defined(__APPLE__) && defined(__amd64__)
	void* ip = (void*)ucxt->uc_mcontext->__ss.__rip;
	void* bp = (void*)ucxt->uc_mcontext->__ss.__rbp;
#elif defined(__FreeBSD__) && defined(__amd64__)
	void* ip = reinterpret_cast<void*>(ucxt->uc_mcontext.mc_rip);
	void* bp = reinterpret_cast<void*>(ucxt->uc_mcontext.mc_rbp);
#elif defined(__FreeBSD__) && defined(__i386__)
	void* ip = reinterpret_cast<void*>(ucxt->uc_mcontext.mc_eip);
	void* bp = reinterpret_cast<void*>(ucxt->uc_mcontext.mc_ebp);
#else
#error "Manual stack trace for your architecture is not supported"
#endif

	StackTraceAlt(ip, bp, report, reportArg, max_size);

#else

	struct unwind_helper arg = {
		.left = max_size,
		.report = report,
		.reportArg = reportArg,
	};
	_Unwind_Backtrace(backtrace_helper, &arg);
#endif
}
