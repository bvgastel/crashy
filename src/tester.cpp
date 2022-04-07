#include "crash.h"

#include <errno.h>
#include <string.h>

#include <stdexcept>

#include <iostream>
#include <iomanip>
#include <sstream>

int x = 0;

void crash(void) {
	switch (x) {
		case 1:
			reinterpret_cast<char*>(0)[0x42] = '\42';
			break;
		case 2:
			throw uint32_t(42);
		case 3:
      ENSURE(false);
	}
	if (errno || !errno)
		throw std::runtime_error("foobar");
}

void bar(void) {
	PrintCurrentCallStack(30);
	crash();
}

void foo(void) {
	bar();
}

int main(int argc, char** argv) {
	/*
	CrashOptions options;
	options.sendFormat = CrashOptions::JSON_SENTRY;
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
	/*/
	CrashOptions options;
  options.setCommandLineOptions(argc, argv);
	options.sendFormat = CrashOptions::JSON_SENTRY;
	options.getContext = []{ return "my-context"; };
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
	//*/
	options.convertExceptionPtr = [](std::exception_ptr e) -> std::string {
		try {
			std::rethrow_exception(e);
		} catch (uint32_t n) {
			return std::string("number: ") + std::to_string(n);
		}
	};
	GenerateDumpOnCrash(std::move(options));
	x = atoi(argv[1]);
	if (x != 0)
		foo();
	return x;
}
