// LibUsageExample
// Check project properties -> VC++ Directories -> Include Directories
// Check project properties -> VC++ Directories -> Library Directories
// Check project properties -> Linker -> Input -> Additional Dependencies
//

#include <Windows.h>
#include <string>
#include <vector>
#include <filesystem>

#include <kdmapper.hpp>
#include <utils.hpp>
#include <intel_driver.hpp>

LONG WINAPI SimplestCrashHandler(EXCEPTION_POINTERS* ExceptionInfo)
{
	if (ExceptionInfo && ExceptionInfo->ExceptionRecord)
		kdmLog(L"[!!] Crash at addr 0x" << ExceptionInfo->ExceptionRecord->ExceptionAddress << L" by 0x" << std::hex << ExceptionInfo->ExceptionRecord->ExceptionCode << std::endl);
	else
		kdmLog(L"[!!] Crash" << std::endl);

	if (intel_driver::hDevice)
		intel_driver::Unload();

	return EXCEPTION_EXECUTE_HANDLER;
}

int paramExists(const int argc, wchar_t** argv, const wchar_t* param) {
	size_t plen = wcslen(param);
	for (int i = 1; i < argc; i++) {
		if (wcslen(argv[i]) == plen + 1ull && _wcsicmp(&argv[i][1], param) == 0 && argv[i][0] == '/') { // with slash
			return i;
		}
		else if (wcslen(argv[i]) == plen + 2ull && _wcsicmp(&argv[i][2], param) == 0 && argv[i][0] == '-' && argv[i][1] == '-') { // with double dash
			return i;
		}
	}
	return -1;
}

void help() {
	kdmLog(L"\r\n\r\n[!] Incorrect Usage!" << std::endl);
	kdmLog(L"[+] Usage: LibUsageExample.exe driver_path" << std::endl);
}

bool callbackExample(ULONG64* param1, ULONG64* param2, ULONG64 allocationPtr, ULONG64 allocationSize) {
	UNREFERENCED_PARAMETER(param1);
	UNREFERENCED_PARAMETER(param2);
	UNREFERENCED_PARAMETER(allocationPtr);
	UNREFERENCED_PARAMETER(allocationSize);
	kdmLog("[+] Callback example called" << std::endl);

	/*
	This callback occurs before call driver entry and
	can be usefull to pass more customized params in
	the last step of the mapping procedure since you
	know now the mapping address and other things
	*/
	return true;
}



int wmain(const int argc, wchar_t** argv) {
	SetUnhandledExceptionFilter(SimplestCrashHandler);

	int drvIndex = -1;
	for (int i = 1; i < argc; i++) {
		if (std::filesystem::path(argv[i]).extension().string().compare(".sys") == 0) {
			drvIndex = i;
			break;
		}
	}

	if (drvIndex <= 0) {
		help();
		return -1;
	}

	const std::wstring driver_path = argv[drvIndex];

	if (!std::filesystem::exists(driver_path)) {
		kdmLog(L"[-] File " << driver_path << L" doesn't exist" << std::endl);
		return -1;
	}

	intel_driver::Load();

	if (intel_driver::hDevice == INVALID_HANDLE_VALUE)
		return -1;

	std::vector<uint8_t> raw_image = { 0 };
	if (!kdmUtils::ReadFileToMemory(driver_path, &raw_image)) {
		kdmLog(L"[-] Failed to read image to memory" << std::endl);
		intel_driver::Unload();
		return -1;
	}

	NTSTATUS exitCode = 0;
	if (!kdmapper::MapDriver(raw_image.data(), 0, 0, true, true, kdmapper::AllocationMode::AllocatePool, false, callbackExample, &exitCode)) {
		kdmLog(L"[-] Failed to map " << driver_path << std::endl);
		intel_driver::Unload();
		return -1;
	}

	if (!intel_driver::Unload()) {
		kdmLog(L"[-] Warning failed to fully unload vulnerable driver " << std::endl);
	}
	kdmLog(L"[+] success" << std::endl);
}