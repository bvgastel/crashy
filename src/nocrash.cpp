
#include "crash.h"

void GenerateDumpOnCrash(CrashOptions&& options [[maybe_unused]]) {
}
extern "C" int PrintCurrentCallStack(int max_size [[maybe_unused]]) {
	return -1;
}

extern "C" [[noreturn]] void CrashAssert(const char* func, const char* file, int line, const char* condition, const char* explanation) {
	fprintf(stderr, "Assertion violation in %s [%s:%i]: %s%s%s.\n", func, file, line, condition, explanation ? " due to " : "", explanation ? explanation : "");
	abort();
}
