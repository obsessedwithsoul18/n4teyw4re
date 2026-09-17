#pragma once
// Force-included before every kdmapper / driver_mapper source file via /FI.
//
// Step 1: tell winerror.h (pulled by windows.h) to skip its STATUS_* block.
#define WIN32_NO_STATUS
// Step 2: include windows.h so the rest of the WinAPI surface is available.
#include <Windows.h>
// Step 3: now include ntstatus.h — with WIN32_NO_STATUS in effect it defines
//         all STATUS_* macros cleanly without any redefinition conflicts.
#pragma warning( push )
#pragma warning( disable: 4005 )
#include <ntstatus.h>
#pragma warning( pop )
