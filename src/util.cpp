#include "util.h"
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <string>
#include <stdio.h>

#ifdef __FreeBSD__
#include <errno.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#include <sstream>
#endif

#include <cxxabi.h>
#include "tosourcecode.h"
#include "term-defines.h"
#include "crash.h"

const char* BaseName(const char *str) {
	const char *p = strrchr(str, '/');
	return p ? p + 1 : str;
}

std::string RawDirName(const char *str) {
	const char *p = strrchr(str, '/');
	if (p)
		return {str, p + 1};
	return {};
}

const char* AfterFirstPath(const char *str) {
	const char *p = strchr(str, '/');
	return p ? p + 1 : str;
}

const char* Demangle(const char* name, std::unique_ptr<char,Free>& retainer, bool force) {
	// C++ names are prefixed by _Z (which is reserved in C)
	if (!force && (!name || name[0] != '_' || (name[0] != '\0' && name[1] != 'Z')))
		return name;
	int status = 0;
	retainer.reset(abi::__cxa_demangle(name, NULL, NULL, &status));
	return status ? name : retainer.get();
}

void ReplaceAll(std::string& str, const std::string& from, const std::string& to) {
	if(from.empty())
		return;
	size_t start_pos = 0;
	while((start_pos = str.find(from, start_pos)) != std::string::npos) {
		str.replace(start_pos, from.length(), to);
		start_pos += to.length(); // In case 'to' contains 'from', like replacing 'x' with 'yx'
	}
}

// on FreeBSD returns the processor type; on Darwin returns the Mac model name
std::string GetMachineModel() {
#if defined(__FreeBSD__) || defined(__APPLE__)
	char buffer[256];
	size_t count = sizeof(buffer);
#if defined(__APPLE__)
	//if (sysctlbyname("machdep.cpu.brand_string", buffer, &count, NULL, 0) != 0 && errno != ENOMEM)
	if (sysctlbyname("hw.model", buffer, &count, NULL, 0) != 0 && errno != ENOMEM)
		return {};
#else
	static const int name[] = {
		CTL_HW, HW_MODEL, -1,
	};
	if (sysctl(name, 2, buffer, &count, NULL, 0) != 0 && errno != ENOMEM)
		return {};
#endif
	for (size_t i = 0; i < count; ++i) {
		if (std::isalnum(buffer[i]))
			continue;
		if (std::isblank(buffer[i]))
			continue;
		if (std::ispunct(buffer[i]))
			continue;
		buffer[i] = ' ';
	}
	std::string retval {buffer, buffer+count};
	ReplaceAll(retval, "(R)", "");
	ReplaceAll(retval, "(TM)", "");
	ReplaceAll(retval, "CPU", "");
	ReplaceAll(retval, "  ", " ");
	return retval;
#else
	return {};
#endif
}

bool loggerTerminal = isatty(STDERR_FILENO);
#define out stderr

#ifdef __APPLE__
std::string Exec(const char* command) {
    FILE* f = popen(command, "r");
    if (!f)
        return {};
    char buffer[16384];
		char *current = buffer;
    while (!feof(f) && current < buffer+sizeof(buffer)) {
        size_t c = fread(current, 1, size_t(buffer+sizeof(buffer) - current), f);
        if (c > 0)
					current += c;
    }
    pclose(f);
    return {buffer, current};
}
#endif

int GetCurrentProcess(char* result, size_t& count) {
  size_t s = count;
#if defined(__FreeBSD__)
 static const int name[] = {
   CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1,
 };
 if (sysctl(name, 4, result, &count, NULL, 0) != 0 && errno != ENOMEM)
   return -1;
#elif defined(__APPLE__)
 if (count > size_t(std::numeric_limits<uint32_t>::max()))
   return -1;
 uint32_t c = uint32_t(count);
 if (_NSGetExecutablePath(result, &c))
   return -1;
 count = c;
#else
 // NetBSD uses /proc/curproc/exe
 ssize_t retval = readlink( "/proc/self/exe", result, PATH_MAX);
 if (retval == -1)
   return -1;
 count = size_t(retval);
#endif
  char* resolvedLibrary = realpath(result, nullptr);
  count = std::min(s, strlen(resolvedLibrary));
  memcpy(result, resolvedLibrary, count);
  free(resolvedLibrary);
  return 0;
}

template <size_t N>
const char* GetCurrentProcess(char (&result)[N]) {
 size_t count = N-1; //sizeof(result)-1;
 if (GetCurrentProcess(result, count))
   return nullptr;
 result[count] = '\0';
  return result;
}

std::string crashExecutable;
const char* SetCurrentExecutable(const char* executable) {
#if defined(__FreeBSD__) || defined(__APPLE__)
  if (strlen(executable) >= 2 && (executable[0] != '.' || executable[1] != '/') && executable[0] != '/') {
    // need this to resolve 'my-global-command' to a path
    char result[PATH_MAX+1] = {0};
    crashExecutable = GetCurrentProcess(result);
  } else {
    char* exe = realpath(executable, nullptr);
    if (exe) {
      crashExecutable = exe;
      free(exe);
    } else {
      perror("SetCurrentExecutable: realpath error");
      crashExecutable = executable;
    }
  }
#else
  crashExecutable = executable;
#endif
  return crashExecutable.c_str();
}
const char* GetCurrentExecutable() {
  return crashExecutable.c_str();
}


std::tuple<std::string, std::string, std::string, uint32_t, uint32_t> RetrieveSourceCodeInfo(const char* _symbolName, const char* filename, uint32_t offset_in_file, void* pc [[maybe_unused]], const char* currentExecutable [[maybe_unused]]) {
	char* symbolName = _symbolName ? strdup(_symbolName) : nullptr;
	std::unique_ptr<char, Free> retainer {symbolName};
#ifdef __APPLE__
	std::unique_ptr<char, Free> retainer2;
	auto demangled = Demangle(symbolName, retainer2);
	std::string symbolNameDemangled = demangled ? demangled : "-";
	char buffer[512];
	snprintf(buffer, sizeof(buffer)-1, "/usr/bin/atos -o '%s' -l %08lx %p", 
		filename,
		uintptr_t(pc) - offset_in_file,
		pc);
	buffer[511] = '\0';
	std::string output = Exec(buffer);
	// I know, this is ugly parsing. It is small though
	std::stringstream in {std::move(output)};
	in >> output >> output >> output >> output;
	if (!in.good())
		return {symbolNameDemangled, {}, {}, 0, 0};
	in.str(output.substr(1, output.length()-2));
	std::getline(in, output, ':');
	uint32_t lineNumber = 0;
	in >> lineNumber;
	if (in.eof())
		return {symbolNameDemangled, filename, output, lineNumber, 0};
#else

  const char* currentExecutableFullPath = currentExecutable;
#if defined(__linux__)
  char result[PATH_MAX+1] = {0};
  // Linux uses the start command as currentExecutable (as the start command is returned by dladdr)
  // however, this is normally not a path (just the basename)
  // so we should fetch the actual path
  if (strlen(filename) >= 2 && (filename[0] != '.' || filename[1] != '/') && filename[0] != '/') {
    currentExecutableFullPath = GetCurrentProcess(result);
  }
#endif

  //fprintf(out, "\nRetrieveSourceCodeInfo(%p/%x): currentExecutable %s filename %s\n", pc, offset_in_file, currentExecutable, filename);
  std::string symbolNameDemangled;
  uint32_t lineNumber = 0;
  uint32_t columnNumber = 0;
  if (currentExecutable && strcmp(filename, currentExecutable) == 0) {
    // fall back uses offset_in_file, and should also look at the full path
    filename = currentExecutableFullPath;
    // if export-dynamic option is given, but the symbol is from the main executable, lookup in the main executable
    char* sourceFile = NULL;
    Lookup(filename, uintptr_t(pc), &sourceFile, &lineNumber, &columnNumber, symbolName ? NULL : &symbolName, NULL);
    std::unique_ptr<char, Free> retainer2;
    auto demangled = Demangle(symbolName, retainer2);
    if (demangled)
      symbolNameDemangled = demangled;
    if (sourceFile) {
      std::string sourceFileDisplay = AfterFirstPath(AfterFirstPath(sourceFile));
      free(sourceFile);
      //fprintf(out, "RetrieveSourceCodeInfo(): based on pc of currentExecutable %s\n", filename);
      return {symbolNameDemangled, currentExecutable, sourceFileDisplay, lineNumber, columnNumber};
    }
  }
  char* sourceFile = nullptr;
  Lookup(filename, offset_in_file, &sourceFile, &lineNumber, &columnNumber, symbolName ? NULL : &symbolName, NULL);
  if (symbolNameDemangled.empty()) {
    std::unique_ptr<char, Free> retainer2;
    auto demangled = Demangle(symbolName, retainer2);
    if (demangled)
      symbolNameDemangled = demangled;
  }
  if (sourceFile) {
    std::string sourceFileDisplay = AfterFirstPath(sourceFile);
    free(sourceFile);
    //fprintf(out, "RetrieveSourceCodeInfo(): based on library %s with currentExecutable %s\n", filename, currentExecutable);
    return {symbolNameDemangled, filename, sourceFileDisplay, lineNumber, columnNumber};
  }
#endif
	return {symbolNameDemangled, {}, {}, 0, 0};
}

// lookup source code and symbol name in a full static binary with full debug symbols
std::tuple<std::string, std::string, uint32_t, uint32_t> RetrieveSourceCodeInfo(void* pc [[maybe_unused]], const char* currentExecutable [[maybe_unused]]) {
#ifndef __APPLE__
	std::unique_ptr<char,Free> retainer;
	char* sourceFile = NULL;
	char* functionName = NULL;
	uint32_t lineNumber = 0;
	uint32_t columnNumber = 0;
	uint32_t offset = 0;

#if defined(__linux__)
  // Linux uses the start command as currentExecutable (as the start command is returned by dladdr)
  // however, this is normally not a path (just the basename)
  // so we should fetch the actual path
  char result[PATH_MAX+1] = {0};
  currentExecutable = GetCurrentProcess(result);
#endif
	Lookup(currentExecutable, uint64_t(uintptr_t(pc)), &sourceFile, &lineNumber, &columnNumber, &functionName, &offset);
	if (sourceFile && functionName) {
		std::string sourceFileDisplay = AfterFirstPath(AfterFirstPath(sourceFile));
		free(sourceFile);
		std::string symbolNameDemangled = Demangle(functionName, retainer);
		free(functionName);
		return {symbolNameDemangled, sourceFileDisplay, lineNumber, columnNumber};
	}
#endif
	return {{}, {}, 0, 0};
}

void PrintLine(const std::string& functionName, const std::string& module, uint64_t offset, const std::string& filename, uint32_t lineNumber, uint32_t columnOffset [[maybe_unused]]) {
	std::string sourceFileDirectory = RawDirName(filename.c_str());
	const char* sourceFileBase = BaseName(filename.c_str());
	fprintf(out,
			loggerTerminal ?
			TERMINAL_BULLET TERMINAL_FULL "%s" TERMINAL_DIM " in " TERMINAL_RESET "%s+0x%llx" TERMINAL_DIM "\n" TERMINAL_ALIGN "[%s" TERMINAL_UNDERLINE "%s" TERMINAL_UNDERLINE_RESET ":%u]" TERMINAL_RESET "\n" : 
			SYMBOL_BULLET "%s in %s+0x%llx [%s%s:%u]\n",
			functionName.c_str(), BaseName(module.c_str()), static_cast<long long unsigned int>(offset), sourceFileDirectory.c_str(), sourceFileBase, lineNumber);
}

std::tuple<std::string, std::string, std::string, uint32_t, uint32_t> RetrieveAndPrintSymbol(const char* symbolName, uint32_t offset_in_func [[maybe_unused]], const char* filename, uint32_t offset_in_file, void* pc, const char* currentExecutable) {
  auto [functionName, library, sourceFile, lineNumber, columnOffset] = RetrieveSourceCodeInfo(symbolName, filename, offset_in_file, pc, currentExecutable);

	if (!sourceFile.empty()) {
		PrintLine(functionName, library, uintptr_t(offset_in_file), sourceFile, lineNumber, columnOffset);
	} else {
		fprintf(out,
				loggerTerminal ?
				TERMINAL_BULLET TERMINAL_FULL "%s" TERMINAL_DIM " in " TERMINAL_RESET "%s" TERMINAL_DIM "+0x%x (%p)" TERMINAL_RESET "\n" : 
				SYMBOL_BULLET "%s in %s+0x%x (%p)\n",
				functionName.c_str(), BaseName(filename), offset_in_file, pc);
	}
	return {functionName, library, sourceFile, lineNumber, columnOffset};
}
void PrintSymbol(const char* symbolName, uint32_t offset_in_func, const char* filename, uint32_t offset_in_file, void* pc) {
	RetrieveAndPrintSymbol(symbolName, offset_in_func, filename, offset_in_file, pc, GetCurrentExecutable());
}

void PrintSymbolRaw(const char* symbolName, uint32_t offset_in_func [[maybe_unused]], const char*filename, uint32_t offset_in_file, void* pc) {
	std::unique_ptr<char, Free> retainer;
	fprintf(out,
			loggerTerminal ?
			TERMINAL_BULLET TERMINAL_FULL "%s" TERMINAL_DIM "+0x%x in " TERMINAL_RESET "%s" TERMINAL_DIM "+0x%x" TERMINAL_RESET "\n" : 
			SYMBOL_BULLET "%s+0x%x in %s+0x%x (%p)\n",
			Demangle(symbolName, retainer), offset_in_func, BaseName(filename), offset_in_file, pc);
}

std::tuple<std::string, std::string, uint32_t, uint32_t> RetrieveAndPrintPC(void* pc, const char* currentExecutable) {
	auto [functionName, sourceFile, lineNumber, columnOffset] = RetrieveSourceCodeInfo(pc, currentExecutable);

	if (!functionName.empty()) {
		PrintLine(functionName, currentExecutable, uintptr_t(pc), sourceFile, lineNumber, columnOffset);
	} else if (!sourceFile.empty()) {
		fprintf(out,
        loggerTerminal ?
        TERMINAL_BULLET TERMINAL_FULL "[%s:%u] %p" TERMINAL_RESET "\n" :
        SYMBOL_BULLET "[%s:%u] (%p)\n",
        sourceFile.c_str(), lineNumber, pc);
	} else {
		fprintf(out,
        loggerTerminal ?
        TERMINAL_BULLET TERMINAL_FULL "%p" TERMINAL_RESET "\n" :
        SYMBOL_BULLET "%p\n",
        pc);
	}
	return {functionName, sourceFile, lineNumber, columnOffset};
}
void PrintPC(void* pc) {
	RetrieveAndPrintPC(pc, GetCurrentExecutable());
}

void PrintPCRaw(void* pc) {
	fprintf(out, SYMBOL_BULLET "%p\n", pc);
}

