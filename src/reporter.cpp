#include <limits.h>
#include <libgen.h>
#include <unistd.h>
#include <string.h>
#include <sys/utsname.h>

#include <memory>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <random>
#include <charconv>
#include <signal.h>

#include "reporter.h"
#include "tosourcecode.h"
#include "simple-raw.h"
#include "util.h"
#include "term-defines.h"

#include <sys/types.h>
#include <pwd.h>

#define out stderr
#define loggerTerminal isatty(STDERR_FILENO)

void ReadCrash(int in, CrashOptions&& options [[maybe_unused]]) {
	bool good = true;
	
	uint32_t startTag = ReadBinary(in, uint32_t(), good);
	if (startTag != CrashTag::START)
		return;

	std::time_t t = std::time(nullptr);
	char timebuffer[100];
	if (!std::strftime(timebuffer, sizeof(timebuffer), " [%F %T %z]", std::localtime(&t))) {
		timebuffer[0] = '\0';
	}
	fprintf(out, loggerTerminal ? TERMINAL_COLOR_RED "\n\n" BAR TERMINAL_RESET " CRASH " TERMINAL_COLOR_RED BAR TERMINAL_DIM "%s" TERMINAL_RESET "\n" : "\n\n" BAR " CRASH " BAR "%s\n", timebuffer);

	const char* spacing = "       ";
	std::optional<std::pair<int,void*>> signal;
	std::optional<std::pair<std::string,std::string>> uncaughtException;
	std::optional<std::tuple<std::string,std::string,uint32_t,std::string,std::string>> assertViolation; // func, file, line, condition, explanation
	std::string context;
	std::vector<std::tuple<std::string, std::string, std::string, uint32_t, uint32_t>> frames;
	std::vector<std::tuple<std::string, time_t, std::string>> breadcrumbs;
	while (good) {
		uint32_t tag = ReadBinary(in, uint32_t(), good);
		if (tag == CrashTag::FINISH) {
			break;
		}
		if (tag == CrashTag::SIGNAL) {
			int sig = int(ReadBinary(in, 0U, good));
			void* p = reinterpret_cast<void*>(uintptr_t(ReadBinary(in, uint64_t(0), good)));
			if (!good)
				break;
			fprintf(out, loggerTerminal ? 
					"%s " TERMINAL_DIM "(%i) on address " TERMINAL_RESET "%p" TERMINAL_DIM "." TERMINAL_RESET "\n" :
					"%s (%i) on address %p.\n",
					strsignal(sig), sig, p);
			signal = {sig, p};
		} else if (tag == CrashTag::UNCAUGHT_EXCEPTION) {
			std::string cause = ReadBinary(in, std::string(), good);
			std::string exceptionType = ReadBinary(in, std::string(), good);
			if (!good)
				break;
			std::unique_ptr<char, Free> retainer;
			std::string typeDescription = exceptionType.size() > 0 ? Demangle(exceptionType.c_str(), retainer, true) : "unknown";
			uncaughtException = {cause, typeDescription};
			fprintf(out, loggerTerminal ?
					"%s " TERMINAL_DIM "exception: " TERMINAL_RESET "%s" TERMINAL_DIM "." TERMINAL_RESET "\n" :
					"%s exception: %s.\n",
					typeDescription.c_str(), cause.c_str());
		} else if (tag == CrashTag::ASSERT) {
			std::string func = ReadBinary(in, std::string(), good);
			std::string file = ReadBinary(in, std::string(), good);
			uint32_t line = ReadBinary(in, 0U, good);
			std::string condition = ReadBinary(in, std::string(), good);
			std::string explanation = ReadBinary(in, std::string(), good);
			if (!good)
				break;
			std::unique_ptr<char, Free> retainer;
			std::string typeDescription = func.size() > 0 ? Demangle(func.c_str(), retainer, true) : "unknown";
			assertViolation = {func, file, line, condition, explanation};
			fprintf(out, loggerTerminal ?
					TERMINAL_DIM "Assertion violation in " TERMINAL_FULL "%s" TERMINAL_DIM " [%s:%i]: " TERMINAL_RESET "%s.\n" TERMINAL_DIM "This is due to: " TERMINAL_RESET "%s" TERMINAL_DIM "." TERMINAL_RESET "\n" :
					"Assertion violation in %s [%s:%i]: %s.\nThis is due to: %s\n",
					func.c_str(), file.c_str(), line, condition.c_str(), explanation.c_str());
		} else if (tag == CrashTag::LIBRARY) {
			std::string symbolName = ReadBinary(in, std::string(), good);
			std::string filename = ReadBinary(in, std::string(), good);
			uint32_t offset_in_file = ReadBinary(in, 0U, good);
			void* pc = reinterpret_cast<void*>(uintptr_t(ReadBinary(in, uint64_t(0), good)));
			if (!good)
				break;
      auto [functionName, library, sourceFile, lineNumber, columnOffset] = RetrieveAndPrintSymbol(symbolName.empty() ? nullptr : symbolName.c_str(), 0, filename.c_str(), offset_in_file, pc, options.currentExecutable.c_str());
			frames.emplace_back(functionName, library, sourceFile, lineNumber, columnOffset);
		} else if (tag == CrashTag::PC) {
			void* pc = reinterpret_cast<void*>(uintptr_t(ReadBinary(in, uint64_t(0), good)));
			if (!good)
				break;
			auto [functionName, sourceFile, lineNumber, columnOffset] = RetrieveAndPrintPC(pc, options.currentExecutable.c_str());
			frames.emplace_back(functionName, options.currentExecutable.c_str(), sourceFile, lineNumber, columnOffset);
		} else if (tag == CrashTag::CONTEXT) {
			context = ReadBinary(in, std::string(), good);
			if (!good)
				break;
			fprintf(out, loggerTerminal ?
          TERMINAL_CONTEXT TERMINAL_FULL "%s" TERMINAL_RESET "\n" TERMINAL_COMMANDLINE TERMINAL_FULL " %s\n    " TERMINAL_DIM "in" TERMINAL_RESET " %s\n    " TERMINAL_DIM "of" TERMINAL_RESET " %s/%s [%s]\n" :
					"<~> %s\n||= %s\n    in %s\n" TERMINAL_DIM "of" TERMINAL_RESET " %s/%s [%s]\n",
          context.c_str(), options.command.c_str(), options.path.c_str(),
          options.environment.c_str(), options.dist.c_str(), options.release.c_str());
		} else if (tag == CrashTag::BREADCRUMB) {
			std::string level = ReadBinary(in, std::string(), good);
			time_t time = time_t(ReadBinary(in, uint64_t(0), good));
			std::string description = ReadBinary(in, std::string(), good);
			if (!good)
				break;
			breadcrumbs.emplace_back(level, time, description);
			if (!std::strftime(timebuffer, sizeof(timebuffer), "%F %T", std::localtime(&time))) {
				timebuffer[0] = '\0';
			}
			fprintf(out, loggerTerminal ?
					TERMINAL_LOG "%s%s [%s] " TERMINAL_RESET "%s" "\n" TERMINAL_RESET :
					"<+> %s%s [%s] %s\n", timebuffer, &spacing[std::min(size_t(7), level.size())], level.c_str(), description.c_str());
		}
	}

	if (!good)
		return;

	std::stringstream report;
	if (options.sendFormat == CrashOptions::PLAIN_TEXT) {
		report << "=== CRASH === " << timebuffer << "\n";
		if (signal) {
			auto [sig, p] = *signal;
			report << strsignal(sig) << " (" << sig << ") on address " << p << ".\n";
		} else if (uncaughtException) {
			auto [cause, typeDescription] = *uncaughtException;
			report << typeDescription << " exception: " << cause << ".\n";
		} else if (assertViolation) {
			auto [func, file, lineno, condition, explanation] = *assertViolation;
			report << "Assertion violation in " << func << " [" << file << ":" << lineno << "]: " << condition << ".\n";
			if (!explanation.empty())
				report << "This is due to " << explanation << ".\n";
		}
		if (!frames.empty()) {
			for (size_t i = 0; i < frames.size(); i++) {
				auto [functionName, library, sourceFile, lineNumber, columnOffset] = frames[i];
				if (!sourceFile.empty()) {
					report << "  at " << functionName << " [" << sourceFile << ":" << lineNumber << "]\n";
				} else if (!functionName.empty()) {
					report << "  at " << functionName << "\n";
				} else {
					report << "  at (unknown)\n";
				}
			}
		}
		report << std::endl;
		report << "Command: " << options.command << std::endl;
		report << "   Path: " << options.path << std::endl;
		report << std::endl;
		for (auto& [level, time, string] : breadcrumbs) {
			if (!std::strftime(timebuffer, sizeof(timebuffer), "%F %T", std::localtime(&time))) {
				timebuffer[0] = '\0';
			}
			report << timebuffer << &spacing[std::min(size_t(7), level.size())] << " [" << level << "] " << string << std::endl;
		}
	} 
	if (options.sendFormat == CrashOptions::JSON_SENTRY) {
		static std::random_device rd("/dev/urandom"); // win32 ignores argument for random_device
		uint32_t id[4] = {0};
		for (auto& i : id)
			i = rd();
		report << "{";
		std::ios state(NULL);
		state.copyfmt(report);
		report << "\"event_id\": \"" << std::setfill('0') << std::hex << std::setw(8) << id[0] << std::setw(8) << id[1] << std::setw(8) << id[2] << std::setw(8) << id[3] << "\"";
		report.copyfmt(state);
		report << ",\"contexts\": {";
		struct utsname version;
		uname(&version);
		{
			report << "\"os\": {";
			{
				report << "\"name\": " << std::quoted(version.sysname);
				report << ",\"version\": " << std::quoted(std::string(version.release) + " " + version.machine);
			}
			report << "}";
			report << ",\"device\": {";
			{
				report << "\"name\": " << std::quoted(version.nodename);
				auto model = GetMachineModel();
				if (!model.empty())
					report << ",\"model\": " << std::quoted(model);
				report << ",\"arch\": " << std::quoted(version.machine);
			}
			report << "}";
		}
		report << "}";
		report << ",\"tags\": {\"path\": " << std::quoted(options.path) << ", \"commandline\": " << std::quoted(options.command) << "}";
		report << ",\"timestamp\": " << t;
		report << ",\"platform\": \"c\"";
		report << ",\"logger\": \"indigo_crash\"";
		if (!options.release.empty())
			report << ",\"release\": " << std::quoted(options.release);
		if (!options.dist.empty())
			report << ",\"dist\": " << std::quoted(options.dist);
		report << ",\"environment\": " << std::quoted(options.environment);
		report << ",\"level\": \"fatal\"";
		report << ",\"server_name\": " << std::quoted(version.nodename);
		report << ",\"exception\": {\"values\":[{";
		if (signal) {
			auto [sig, p] = *signal;
			char ptr[17];
			auto [pString, ec] = std::to_chars(ptr, ptr + sizeof(ptr), uintptr_t(p), 16);
			if (ec != std::errc())
				pString = ptr;
			if (sig == SIGSEGV || sig == SIGBUS) {
				report << "\"mechanism\": { \"type\": \"signalhandler\", \"handled\": false, \"data\": { \"relevant_address\": \"0x" << std::string(ptr, size_t(pString - ptr)) << "\"}, \"meta\": { \"signal\": { \"number\": " << sig << "} } }";
			} else {
				report << "\"mechanism\": { \"type\": \"signalhandler\", \"handled\": false, \"meta\": { \"signal\": { \"number\": " << sig << "} } }";
			}
			report << ",\"type\": " << std::quoted(std::string(strsignal(sig)));
			report << ",\"value\": " << std::quoted(std::string(strsignal(sig)) + " (" + std::to_string(sig) + ") on address 0x" + std::string(ptr, size_t(pString - ptr)) + ".");
		} else if (uncaughtException) {
			auto [cause, typeDescription] = *uncaughtException;
			report << "\"mechanism\": { \"type\": \"UncaughtExceptionHandler\", \"handled\": false }";
			report << ",\"type\": " << std::quoted(typeDescription);
			report << ",\"value\": " << std::quoted(typeDescription + " exception: " + cause + ".");
		} else if (assertViolation) {
			auto [func, file, lineno, condition, explanation] = *assertViolation;
			report << "\"mechanism\": { \"type\": \"AssertionViolation\", \"handled\": false }";
			report << ",\"type\": \"assert\"";
			report << ",\"value\": " << std::quoted(std::string("assertion ") + condition + " in " + func + " [" + file + ":" + std::to_string(lineno) + "] violated, due to " + explanation + ".");
		}
		if (!context.empty()) {
			report << ",\"thread_id\":" << std::quoted(context);
		}
		if (!frames.empty()) {
			report << ",\"stacktrace\":{\"frames\":[";
			const char* sep = "";
			for (auto i = frames.size(); i-- > 0; ) {
				auto [functionName, library, sourceFile, lineNumber, columnOffset] = frames[i];
				if (!sourceFile.empty()) {
					report << sep << "{\"function\": " << std::quoted(functionName) << ", \"package\": " << std::quoted(library) << ",\"filename\": " << std::quoted(sourceFile) << ", \"lineno\": " << lineNumber << "}";
					sep = ",";
				} else if (!functionName.empty()) {
					report << sep << "{\"function\": " << std::quoted(!functionName.empty() ? functionName : "(unknown)") << "}";
					sep = ",";
				}
			}
			report << "]}"; // end stacktrace object
		}
		{
			auto uid = getuid();
			report << ",\"user\": {";
			report << "\"id\": " << uid;

      // sometimes this code crashes the crash reporter, due to getpwuid_r() triggering a dl_open that generates a segfault.
      if (options.reportUsername) {
        char buffer[256];
        struct passwd _pw;
        struct passwd *pw;
        if (getpwuid_r(uid, &_pw, buffer, sizeof(buffer), &pw) == 0) {
          report << ",\"username\": \"" << pw->pw_name << "\"";
        }
      }
			report << "}"; // end user
		}
		report << "}]}"; // end exception

		report << ",\"breadcrumbs\":{\"values\":[";
		const char* sep = "";
		for (auto& [level, time, string] : breadcrumbs) {
			report << sep;
			report << "{\"message\":" << std::quoted(string);
			report << ",\"timestamp\":" << time;
			if (!level.empty())
				report << ",\"level\":" << std::quoted(level);
			report << "}";
			sep = ",";
		}
		report << "]}"; // end breadcrumbs

		report << "}"; // end main object
	}

	// after sending crash report, close
	if (options.sender) {
		if (!options.sender(options.sendFormat, report.str()))
			std::cerr << "Failed to send crash report." << std::endl;
	} else {
		std::cerr << report.str() << std::endl;
	}
}

#include <unistd.h>

std::tuple<int,pid_t,CrashOptions> StartReporter(CrashOptions&& options) {
	int pipefd[2];
	if (pipe(pipefd))
		return {-1, 0, std::move(options)};
	pid_t reporterPid = fork();
	if (reporterPid == 0) {
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(pipefd[1]);
		if (options.prepare)
			options.prepare(options.sendFormat);
		ReadCrash(pipefd[0], std::move(options));
		::_exit(0);
	}
	close(pipefd[0]);
	return {pipefd[1], reporterPid, std::move(options)};
}
