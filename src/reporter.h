#include <sys/types.h>
#include <unistd.h>

#include "crash.h"

// returns a file descriptor to write in binary form a crash report
// and returns a process id of the crash reporter that will finish if it has sent out the crash report
std::tuple<int,pid_t,CrashOptions> StartReporter(CrashOptions&& options);
