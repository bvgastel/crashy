// c++ -fPIC -pie -g1 -Wl,--export-dynamic -fdebug-prefix-map=(dirname (pwd))/= -fno-optimize-sibling-calls -fno-omit-frame-pointer -fno-inline -std=c++17 -o tester crash.cpp reporter.cpp simple-raw.cpp tester.cpp tosourcecode.cpp unwinder.cpp util.cpp -I../../include -ldwarf -lelf; and ./tester 2
// c++ -std=c++17 -g -rdynamic *.cpp -o unwinder -I. -I/usr/include/libdwarf -I../../include -ldwarf -ldl

// compile with: cc -g -Wl,--export-dynamic unwinder.c -o unwinder -I/usr/include/libdwarf -ldwarf -ldl
// or with cc -static -g unwinder.c -o unwinder -I/usr/include/libdwarf -ldwarf -ldl -lelf
//
// Linux (arm):
// cc -g -shared -o libcrashlib.so crashlib.c
// c++ -g -funwind-tables -rdynamic -o unwinder unwinder.c -lcrashlib -L. -I/usr/include/libdwarf -ldwarf -ldl; LD_LIBRARY_PATH=. ./unwinder
//

// Description:
// Simple handler to generate stack traces for the current thread, with support for demangling, 
//
// Usage:
// - add the flag -fno-omit-frame-pointer for improve stack traces
// - on some platforms the following flags can help: -Wl,--export-dynamic / -rdynamic (needed on FreeBSD if SOURCE_CODE is off and compiling without -pie)
// - the flag -g is needed for debugging symbols so source code information is present (clang has the option -gline-tables-only, to only include line numbers as debug info)
// - on macOS -fstandalone-debug can help with third-party libraries
// - on ARM add -funwind-tables or -fasynchronous-unwind-tables
//
// Platforms (tested):
// - FreeBSD (amd64)
// - Linux (amd64, arm, arm64)
// - macOS (amd64 without source code filenames and line numbers)
//
// Note:
// - from the signal handler, only safe functions can be called: https://wiki.sei.cmu.edu/confluence/display/c/SIGMAX_STACK_TRACE-C.+Call+only+asynchronous-safe+functions+within+signal+handlers
// - on x86_64 we use the manual stack frame walking (because _Unwind_Backtrace does not work in signal handlers on FreeBSD, and a double free corruption occurs in libdwarf on Linux)
// - dladdr is needed to circumvent PIE/PIC and deal with shared libraries
// - Wanting something similar for Windows? Look at:
//   - https://www.codeproject.com/Articles/207464/Exception-Handling-in-Visual-Cplusplus
//   - https://github.com/JochenKalmbach/StackWalker/blob/master/Main/StackWalker/StackWalker.cpp
//   - https://gist.github.com/jvranish/4441299
// - core dump explanation for FreeBSD: https://backtrace.io/blog/backtrace/whats-a-coredump/
//
// TODO:
// - assert support
// - C++ exception support:
//   - maybe ability to storing the stack trace with an exception (see boost::stacktrace)
//   - global uncaught exception should display stack trace
// - also handle SIGFPE, SIGILL and SIGABRT
// - macOS source code and line number support (possibly with the atos utility, only works if compilation to object file is seperate from linking)
//   popen("/usr/bin/atos -o [file] -l [dyldInfo.dli_fbase] [pc]");
//     --or--
//   char buffer[512];
//   snprintf(buffer, 511, "/usr/bin/atos -o '%s' -l %p %p", dyldInfo.dli_fname, dyldInfo.dli_fbase, pc);
//   system(buffer);
// - adjust flow:
//   - fork() at start an executable to function as crash report outputter / symbol resolver / report uploader
//   - on crash, write symbols to pipe, wait on finish of child

#if !defined(_GNU_SOURCE) && defined(__linux__)
#define _GNU_SOURCE	// linux needs this for Dl_info
#endif

#include "crashy.h"

#include <dlfcn.h>

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <libgen.h>
#include <signal.h>
#if defined(__APPLE__)
#define _XOPEN_SOURCE // needed for macOS ucontext
#endif
#include <ucontext.h>
#include <sys/wait.h>

#ifdef __cplusplus
#include <exception>
#include <memory>
#include <atomic>
#include <cxxabi.h>
#endif

#include "tosourcecode.h"
#include "unwinder.h"
#include "simple-raw.h"
#include "reporter.h"
#include "util.h"

#define MAX_STACK_TRACE 32

CrashOptions crashOptions;

using PrintSymbolFunc = void (*)(const char* symbolName, uint32_t offset_in_func, const char*filename, uint32_t offset_in_file, void* pc);
using PrintPCFunc = void (*)(void* pc);

struct ToReporterArgs {
	const char** filter = nullptr;
	bool skipUntilMatch = true;
	PrintSymbolFunc printSymbol;
	PrintPCFunc printPC;
	const char* currentExecutable = nullptr;
	bool display(const char* name) {
		if (filter) {
			if (name) {
				for (const char** current = filter; *current; current = &current[1]) {
					if (strcmp(name, *current) == 0) {
						skipUntilMatch = false;
						return false;
					}
				}
			}
			if (skipUntilMatch)
				return false;
		}
		return true;
	}
	bool display() {
		return filter && !skipUntilMatch;
	}
};

int crashReporterLink = -1;
pid_t crashReporterProcess = 0;

void WriteString(const char* str) {
	if (!str)
		str = "";
	auto length = strlen(str);
	if (length >= 8192)
		length = 0;
	WriteBinary(crashReporterLink, str, uint32_t(length));
}

void PrintSymbolToReporter(const char* symbolName, uint32_t offset_in_func [[maybe_unused]], const char*filename, uint32_t offset_in_file, void* pc) {
	WriteBinary(crashReporterLink, uint32_t(CrashTag::LIBRARY));
	WriteString(symbolName);
	WriteString(filename);
	WriteBinary(crashReporterLink, uint32_t(offset_in_file));
	WriteBinary(crashReporterLink, uint64_t(pc));
}

void PrintPCToReporter(void* pc) {
	WriteBinary(crashReporterLink, uint32_t(CrashTag::PC));
	WriteBinary(crashReporterLink, uint64_t(pc));
}

bool Process(void* pc, void* _args) {
	ToReporterArgs* args = static_cast<ToReporterArgs*>(_args);
  if (!args)
    return false;
	// on FreeBSD/Linux compile with  -Wl,--export-dynamic
	Dl_info dyldInfo;
	if (dladdr(pc, &dyldInfo)) {
		if (!args->display(dyldInfo.dli_sname))
			return false;
		uintptr_t offset_in_file = uintptr_t(pc) - uintptr_t(dyldInfo.dli_fbase);
		uint32_t offset_in_func = uint32_t(uint64_t(pc) - uint64_t(dyldInfo.dli_saddr));
		args->printSymbol(dyldInfo.dli_sname, offset_in_func, dyldInfo.dli_fname, uint32_t(offset_in_file), pc);
		return dyldInfo.dli_sname && (
				strncmp("main", dyldInfo.dli_sname, 5) == 0
				||  strncmp("GlobalDispatcherRun", dyldInfo.dli_sname, 20) == 0);
	}
	if (!args->display())
		return false;
	args->printPC(pc);
	return false;
}

extern "C" int PrintCurrentCallStack(int max_size) {
	const char* ThrowHandlers[] = {"PrintCurrentCallStack", NULL};
	ToReporterArgs args {
		.filter = ThrowHandlers,
		.printSymbol = PrintSymbol,
		.printPC = PrintPC,
	};
	return StackTrace(Process, &args, max_size);
}

[[noreturn]] void FinishReport() {
	if (crashReporterLink < 0)
		::_Exit(EXIT_FAILURE);
	if (crashOptions.getContext) {
		WriteBinary(crashReporterLink, uint32_t(CrashTag::CONTEXT));
		WriteString(crashOptions.getContext());
	}
	if (crashOptions.getBreadcrumbs) {
		while (auto c = crashOptions.getBreadcrumbs()) {
			WriteBinary(crashReporterLink, uint32_t(CrashTag::BREADCRUMB));
			WriteString(std::get<0>(*c));
			WriteBinary(crashReporterLink, uint64_t(std::get<1>(*c)));
			WriteBinary(crashReporterLink, std::get<2>(*c), std::min(uint32_t(1024UL), uint32_t(std::get<3>(*c))));
		}
	}
	WriteBinary(crashReporterLink, uint32_t(CrashTag::FINISH));
	close(crashReporterLink);
	int status = 0;
	while (waitpid(crashReporterProcess, &status, 0) < 0 && errno == EINTR);
	if (WIFEXITED(status)) {
		if (WEXITSTATUS(status)) {
			fprintf(stderr, "◢◤◢◤◢◤ CRASH REPORTER stopped with status %i ◢◤◢◤◢◤\n", WEXITSTATUS(status));
		}
	} else {
		fprintf(stderr, "◢◤◢◤◢◤ CRASH REPORTER stopped abnormally ◢◤◢◤◢◤\n");
	}
	::abort(); // so debuggers can attach
}


static void DisableCrashReporting() {
	// FIXME: maybe install a safeguard to send CrashTag::ABORTED and do a wait (in case the extra context/breadcrumbs functions hit a memory corruption)
	signal(SIGSEGV, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
	signal(SIGABRT, SIG_DFL);
}

const char* UncaughtExceptionThrowHandlers[] = {"__cxa_rethrow", "__cxa_throw", "_ZSt9terminatev", "_thr_kill", "abort", NULL};

std::tuple<std::string,std::string> GetExceptionDescription() {
	std::string descriptionString;
	const char* description = nullptr;
	std::type_info* exceptionType = __cxxabiv1::__cxa_current_exception_type();
	if (crashOptions.convertExceptionPtr) {
		try {
			descriptionString = crashOptions.convertExceptionPtr(std::current_exception());
			if (!descriptionString.empty())
				description = descriptionString.c_str();
		} catch (...) {
		}
	}
	if (!description) {
		try {
			throw;
		} catch (const std::exception& e) {
			description = e.what();
		} catch (...) {
			description = "";
		}
	}
	return {exceptionType ? exceptionType->name() : "", description};
}

void SendUncaughtExceptionToReporter() {
	auto [exceptionType, description] = GetExceptionDescription();
	WriteBinary(crashReporterLink, uint32_t(CrashTag::START));
	WriteBinary(crashReporterLink, uint32_t(CrashTag::UNCAUGHT_EXCEPTION));
	WriteString(description.c_str());
	WriteString(exceptionType.c_str());
}

extern "C" [[noreturn]] void SendToReporter(int sig, siginfo_t *si, void *_ucxt) {
	void* p = sig == SIGSEGV || sig == SIGBUS ? si->si_addr : 0;
	DisableCrashReporting();

#if defined(__linux__)
	const char* ThrowHandlers[] = {"SendToReporter", NULL};
	ToReporterArgs args {
		.filter = ThrowHandlers,
#elif defined(__APPLE__)
	const char* ThrowHandlers[] = {"_sigtramp", NULL};
	ToReporterArgs args {
		.filter = ThrowHandlers,
#else
	ToReporterArgs args {
#endif
		.printSymbol = PrintSymbolToReporter,
		.printPC = PrintPCToReporter,
	};
	if (crashReporterLink < 0) {
		fprintf(stderr, "=== CRASH ===\n" "%s (%i) on address %p.\n", strsignal(sig), sig, p);
		args.printSymbol = PrintSymbolRaw;
		args.printPC = PrintPCRaw;
	} else {
		WriteBinary(crashReporterLink, uint32_t(CrashTag::START));
		WriteBinary(crashReporterLink, uint32_t(CrashTag::SIGNAL));
		WriteBinary(crashReporterLink, uint32_t(sig));
		WriteBinary(crashReporterLink, uint64_t(p));
	}

	StackTraceSignal(Process, &args, _ucxt, MAX_STACK_TRACE);

	FinishReport();
}

std::atomic<bool> reportingAssertionBusy {false};
extern "C" [[noreturn]] void CrashAssert(const char* func, const char* file, int line, const char* condition, const char *explanation) {
	if (reportingAssertionBusy.exchange(true)) {
		while (1)
			sleep(1);
	}
	DisableCrashReporting();

	const char* ThrowHandlers[] = {"CrashAssert", NULL};
	ToReporterArgs args {
		.filter = ThrowHandlers,
		.printSymbol = PrintSymbolToReporter,
		.printPC = PrintPCToReporter,
	};
	if (crashReporterLink < 0) {
		fprintf(stderr, "=== CRASH ===\n" "Assertion violation in %s [%s:%i]: %s.\n", func, file, line, condition);
		args.printSymbol = PrintSymbolRaw;
		args.printPC = PrintPCRaw;
	} else {
		WriteBinary(crashReporterLink, uint32_t(CrashTag::START));
		WriteBinary(crashReporterLink, uint32_t(CrashTag::ASSERT));
		WriteString(func);
		WriteString(file);
		WriteBinary(crashReporterLink, uint32_t(line));
		WriteString(condition);
		WriteString(explanation);
	}
	StackTrace(Process, &args, MAX_STACK_TRACE);
	FinishReport();
}

void GenerateDumpOnUncaughtException() {
	DisableCrashReporting();

	if (crashReporterLink < 0) {
		auto [exceptionType, description] = GetExceptionDescription();
		fprintf(stderr, "=== CRASH ===\nUncaught exception of type %s: %s\n", exceptionType.c_str(), description.c_str());
		ToReporterArgs args {
			.filter = UncaughtExceptionThrowHandlers,
			.printSymbol = PrintSymbol,
			.printPC = PrintPC,
		};
		StackTrace(Process, &args, MAX_STACK_TRACE);
		return;
	}

	SendUncaughtExceptionToReporter();
	ToReporterArgs args {
		.filter = UncaughtExceptionThrowHandlers,
		.printSymbol = PrintSymbolToReporter,
		.printPC = PrintPCToReporter,
	};
	StackTrace(Process, &args, MAX_STACK_TRACE);
	FinishReport();
}

void GenerateDumpOnCrash(CrashOptions&& options) {
  options.currentExecutable = SetCurrentExecutable(options.currentExecutable.c_str());

	std::tie(crashReporterLink, crashReporterProcess, options) = StartReporter(std::move(options));
	crashOptions = std::move(options);

#ifdef __cplusplus
	std::set_terminate(GenerateDumpOnUncaughtException);
#endif
	// alternate stack is needed, in case of stack overflow
	stack_t ss;
	ss.ss_size = size_t(SIGSTKSZ);
	ss.ss_sp = malloc(ss.ss_size);

	ss.ss_flags = 0;
	if (sigaltstack(&ss, NULL) == -1) {
		perror("sigaltstack");
		exit(EXIT_FAILURE);
	}

	struct sigaction sa;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = SendToReporter;
	if (sigaction(SIGSEGV, &sa, NULL) == -1)
		perror("sigaction");
	if (sigaction(SIGBUS, &sa, NULL) == -1)
		perror("sigaction");
	if (sigaction(SIGABRT, &sa, NULL) == -1)
		perror("sigaction");
}



