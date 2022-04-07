# crashy

C++ crash reporting library for UNIX systems (macOS/Linux/FreeBSD), in Sentry format or plain text. It works by forking a crash reporting process early on, so that all the loaded libraries and locations (which can be different with ASLR) are known. When a crash occurs the main process will report to the crash reporting process the memory locations of the stack trace, breadcrumbs (log messages), and context (thread/actor name). The crash reporting process will take care of resolving the memory locations of the stack trace to function names and file names. This crash reporting process will also take care of sending the crash report (e.g. to Sentry).

On Linux+FreeBSD crashy depends on the `dwarf` library to resolve memory locations to symbols. On macOS, the `/usr/bin/atos` utility is used for this (due to the dSYM way of working on macOS).

Type of crashes:
- unhandled exceptions;
- segmentation faults and bus errors (incl null pointers);
- assertions:
  - debug build only: use `EXPECT(...)`;
  - debug and release: use `ENSURE(...)`.

## Getting started

Include this repo as a git submodule in your source tree. In your `CMakeLists.txt` include this subdirectory with:
```
add_subdirectory(crashy)
```

In your library or executable link with crashy using: 
```
target_link_libraries(${PROJECT_NAME} PUBLIC crashy)
```

In your `main()` function, add this code:
```c++
#include "crashy.h"

int main(int argc, char** argv) {
  // Builder-like options
  CrashOptions options;
  // Which format does the sender callback expect
  options.sendFormat = CrashOptions::JSON_SENTRY;
  // The callback handling the crash report, executed in the crash reporting process (that is forked)
  options.sender = [](CrashOptions::SendFormat format, const std::string& payload) -> bool{
    if (format == CrashOptions::JSON_SENTRY) {
      std::string auth = "auth";
      std::string url = "url";
      std::stringstream commandBuilder;
      commandBuilder << "curl --silent --data " << std::quoted(payload) << " -H \"Content-Type: application/json\" -H " << std::quoted("X-Sentry-Auth: Sentry sentry_version=7, sentry_key=" + auth + ", sentry_client=indigo_crash/0.1") << " " << std::quoted(url);
      std::string command = commandBuilder.str();
      std::cerr << command << std::endl;
      return system(command.c_str()) == 0;
    }
    std::cerr << payload << std::endl;
    return true;
  };
  // Commandline options can be reported too
  options.setCommandLineOptions(argc, argv);
  // Callback that can be used to report a context: actor or thread name
  options.getContext = []{ return "my-context"; };
  // Callback that retrieves the latest log messages for the current context
  // In this example we just generate bunch of log messages
  options.getBreadcrumbs = [i = 0]() mutable -> std::optional<std::tuple<const char*, time_t, const char*, size_t>> {
    if (i == 0) {
    ++i;
    const char* text = "breadcrumb 0";
    return {{"error", 42, text, strlen(text)}};
    }
    if (i == 1) {
    ++i;
    const char* text = "breadcrumb 1";
    return {{"info", 37, text, strlen(text)}};
    }
    return {};
  };
  // For custom exceptions we need to be able to convert those to a string,
  // so we need to provide a callback for this.
  options.convertExceptionPtr = [](std::exception_ptr e) -> std::string {
    try {
      std::rethrow_exception(e);
    } catch (uint32_t n) {
      return std::string("number: ") + std::to_string(n);
    }
  };
  // initiate the crash reporting service (forking a child process)
  GenerateDumpOnCrash(std::move(options));
  ...
}
```

# Example program

If building this project, a `crashtester` binary is build. It demonstrates a couple of behaviours:
- `./crashtester` will trigger a null pointer dereference (because of the `atoi(argv[1])`);
- `./crashtester 0` will be a nop;
- `./crashtester 1` will access memory location 42, and trigger a segfault;
- `./crashtester 2` will throw exception `uint32_t(42)`, and show custom exception handling;
- `./crashtester 3` will show assertion handling with `ENSURE(...)`.

# Limitations

Some inline functions are not correctly reported on Linux+FreeBSD, as they are stored differently in the DWARF format. Arm32 targets are not extensively tested, and there are some indications that sometimes filenames and linenumbers are missing (arm64 appears to work fine).
