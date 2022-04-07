#include <memory>
#include <cstdlib>

enum CrashTag : uint8_t {
	START=1,
	SIGNAL,
	UNCAUGHT_EXCEPTION,
	ASSERT,
	LIBRARY,
	PC,
	BREADCRUMB,
	CONTEXT,
	FINISH,
};
struct Free {
	void operator()(char* ptr) {
		free(ptr);
	}
};
const char* BaseName(const char *str);
const char* AfterFirstPath(const char *str);
const char* Demangle(const char* name, std::unique_ptr<char,Free>& retainer, bool force = false);

// on FreeBSD returns the processor type; on Darwin returns the Mac model name
std::string GetMachineModel();

// returns demangled symbol name, library/executable name, source file and line number
std::tuple<std::string, std::string, std::string, uint32_t, uint32_t> RetrieveSourceCodeInfo(const char* symbolName, const char* filename, uint32_t offset_in_file, void* pc, const char* currentExecutable);
std::tuple<std::string, std::string, uint32_t, uint32_t> RetrieveSourceCodeInfo(void* pc, const char* currentExecutable);


std::tuple<std::string, std::string, std::string, uint32_t, uint32_t> RetrieveAndPrintSymbol(const char* symbolName, uint32_t offset_in_func, const char* filename, uint32_t offset_in_file, void* pc, const char* currentExecutable);
std::tuple<std::string, std::string, uint32_t, uint32_t> RetrieveAndPrintPC(void* pc, const char* currentExecutable);


void PrintSymbol(const char* symbolName, uint32_t offset_in_func, const char* filename, uint32_t offset_in_file, void* pc);
void PrintSymbolRaw(const char* symbolName, uint32_t offset_in_func, const char* filename, uint32_t offset_in_file, void* pc);

void PrintPC(void* pc);
void PrintPCRaw(void* pc);
