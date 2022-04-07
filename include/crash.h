/**
Copyright 2020-2020 Bernard van Gastel, bvgastel@bitpowder.com.
This file is part of Bit Powder Libraries.

Bit Powder Libraries is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Bit Powder Libraries is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Bit Powder Libraries.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include <time.h>

#include <string>
#include <functional>
#include <optional>
#include <sstream>
#include <iomanip>

class CrashOptions {
	public:
  std::string currentExecutable;
	// so there is no default SSL functionality
	// to upload it to a HTTPS server, you have to do it yourself
	// there is a default way relying on curl installed on your system
	enum SendFormat : uint8_t {NONE=0, PLAIN_TEXT=1, JSON_SENTRY=2};
	SendFormat sendFormat = SendFormat::NONE;
	// can be called with old report formats
	// return value indicated success (so report is removed from persistent storage)
	std::function<void (SendFormat format)> prepare;
	std::function<bool (SendFormat format, const std::string& data)> sender;

	// returns name of current context/thread/executor
	std::function<const char*()> getContext;

	// breadcrumbs (log level, time, message)
	// in this function, we are using const char* so allocations can be avoided (which is disallowed if crash)
	using Breadcrumb = std::optional<std::tuple<const char*, time_t, const char*, size_t>>;
	// getBreadcrumbs can be called multiple times, until nothing is returned
	std::function<Breadcrumb()> getBreadcrumbs;

	// used to specialise thrown exception, so useful strings can be given back
	std::function<std::string(std::exception_ptr)> convertExceptionPtr;

	// directory to store crash reports in a persistent way before the file uploader is called
	// if unset, reports are directly send (and if `sender` returns false, they are lost)
	//std::string persistentCrashReportsDirectory;
	//
	std::string release = ""; // suggestion: [git revision]
	std::string dist = ""; // distribution, suggestion [gitlab pipeline iid (per project)]
	std::string environment = "local";

	std::string command;
	void setCommandLineOptions(int argc, char** argv) {
		std::stringstream commandLine;
		for (int i = 0; i < argc; ++i) {
			if (i)
				commandLine << " ";
			commandLine << std::quoted(argv[i]);
		}
		command = commandLine.str();
    if (argc)
      currentExecutable = argv[0];
	}
	std::string path;
  bool reportUsername = false;
};

void GenerateDumpOnCrash(CrashOptions&& options = {});
const char* SetCurrentExecutable(const char* executable);
const char* GetCurrentExecutable();
extern "C" int PrintCurrentCallStack(int max_size);
#ifndef NDEBUG
#define	EXPECT(e)	((e) ? (void)0 : CrashAssert(__func__, __FILE__, __LINE__, #e))
#define	EXPECT_TEXT(e, text)	((e) ? (void)0 : CrashAssert(__func__, __FILE__, __LINE__, #e, text))
#else
#define	EXPECT(e)	((void)0)
#define	EXPECT_TEXT(e, text)	((void)0)
#endif
#define	ENSURE(e)	((e) ? (void)0 : CrashAssert(__func__, __FILE__, __LINE__, #e))
#define	ENSURE_TEXT(e, text)	((e) ? (void)0 : CrashAssert(__func__, __FILE__, __LINE__, #e, text))
extern "C" [[noreturn]] void CrashAssert(const char* func, const char* file, int line, const char* condition, const char* explanation = nullptr);
