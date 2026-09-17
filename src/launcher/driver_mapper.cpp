/*
 * driver_mapper.cpp
 *
 * Extracts kdmapper and both driver .sys files from embedded PE resources,
 * writes them to %TEMP%, and runs kdmapper against the chosen driver.
 *
 * kdmapper is written with a .tmp extension rather than .exe so that
 * Windows Defender's real-time file scanner is less likely to flag it.
 * The Zone.Identifier ADS is stripped so SmartScreen doesn't prompt.
 * Both temp files are deleted immediately after use.
 */

#include <stdafx.hpp>
#include <launcher/driver_mapper.hpp>
#include <launcher/resource_ids.h>
#include <shellapi.h>
#include <vector>
#include <cstdint>

#define RT_RCDATA_W  MAKEINTRESOURCEW( 10 )

namespace
{

// ---------------------------------------------------------------------------
//  Resource helpers
// ---------------------------------------------------------------------------
static bool load_resource( WORD id, std::vector<std::uint8_t>& out )
{
    const HMODULE hmod  = ::GetModuleHandleW( nullptr );
    const HRSRC   hrsrc = ::FindResourceW( hmod, MAKEINTRESOURCEW( id ), RT_RCDATA_W );
    if ( !hrsrc ) return false;
    const HGLOBAL hg   = ::LoadResource( hmod, hrsrc );
    if ( !hg    ) return false;
    const DWORD  size  = ::SizeofResource( hmod, hrsrc );
    const void*  data  = ::LockResource( hg );
    if ( !data || !size ) return false;
    out.assign( static_cast<const std::uint8_t*>( data ),
                static_cast<const std::uint8_t*>( data ) + size );
    return true;
}

// ---------------------------------------------------------------------------
//  Temp file helpers
// ---------------------------------------------------------------------------
static std::wstring make_temp_path( const wchar_t* suffix, int salt )
{
    wchar_t dir[ MAX_PATH ]{};
    ::GetTempPathW( MAX_PATH, dir );
    wchar_t name[ 64 ]{};
    ::swprintf_s( name, 64, L"n4t_%08X_%d%s",
        static_cast<unsigned>( ::GetTickCount64() ^ ::GetCurrentProcessId() ),
        salt, suffix );
    return std::wstring( dir ) + name;
}

static bool write_file( const std::wstring& path,
                         const std::vector<std::uint8_t>& data )
{
    const HANDLE h = ::CreateFileW( path.c_str(), GENERIC_WRITE, 0,
                                     nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr );
    if ( h == INVALID_HANDLE_VALUE ) return false;
    DWORD written = 0;
    const BOOL ok = ::WriteFile( h, data.data(),
                                  static_cast<DWORD>( data.size() ),
                                  &written, nullptr );
    ::CloseHandle( h );
    if ( !ok || written != static_cast<DWORD>( data.size() ) ) return false;
    // Strip Zone.Identifier ADS so SmartScreen doesn't block execution
    ::DeleteFileW( ( path + L":Zone.Identifier" ).c_str() );
    return true;
}

static void delete_silent( const std::wstring& path )
{
    ::DeleteFileW( path.c_str() );
}

// Runs a process synchronously and hidden, returns its exit code via |ec|.
static bool run_wait( const std::wstring& exe,
                       const std::wstring& args,
                       DWORD&              ec )
{
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb          = sizeof( si );
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if ( !::CreateProcessW( nullptr, cmd.data(), nullptr, nullptr,
                             FALSE, 0, nullptr, nullptr, &si, &pi ) )
        return false;
    ::WaitForSingleObject( pi.hProcess, INFINITE );
    ::GetExitCodeProcess( pi.hProcess, &ec );
    ::CloseHandle( pi.hProcess );
    ::CloseHandle( pi.hThread );
    return true;
}

} // anonymous namespace

// ============================================================================
namespace launcher
{

bool relaunch_self( const std::wstring& extra_args )
{
    wchar_t self[ MAX_PATH ]{};
    ::GetModuleFileNameW( nullptr, self, MAX_PATH );

    const wchar_t* cmd    = ::GetCommandLineW();
    const bool     quoted = ( *cmd == L'"' );
    const wchar_t* p      = quoted ? cmd + 1 : cmd;
    while ( *p && ( quoted ? *p != L'"' : *p != L' ' ) ) ++p;
    if ( quoted && *p == L'"' ) ++p;
    while ( *p == L' ' ) ++p;

    std::wstring params( p );
    if ( !params.empty() ) params += L' ';
    params += extra_args;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof( sei );
    sei.fMask        = SEE_MASK_NOASYNC;
    sei.lpVerb       = L"runas";
    sei.lpFile       = self;
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.nShow        = SW_SHOWNORMAL;
    return ::ShellExecuteExW( &sei ) != FALSE;
}

std::string map_driver( bool km_mode )
{
    // 1. Load resources
    std::vector<std::uint8_t> kdmapper_data, driver_data;

    if ( !load_resource( IDR_KDMAPPER, kdmapper_data ) || kdmapper_data.empty() )
        return "Failed to load embedded kdmapper resource.";

    const WORD drv_id = km_mode ? IDR_DRIVER_KM : IDR_DRIVER_UM;
    if ( !load_resource( drv_id, driver_data ) || driver_data.empty() )
        return "Failed to load embedded driver resource.";

    // 2. Write to temp files
    //    .tmp extension for kdmapper — avoids Defender's .exe heuristic scan
    //    .sys extension for the driver — kernel drivers are not flagged by name
    const std::wstring kdm_tmp = make_temp_path( L"_n4t.tmp", 1 );
    const std::wstring drv_tmp = make_temp_path( L"_drv.sys", 2 );

    if ( !write_file( kdm_tmp, kdmapper_data ) )
        return "Failed to write kdmapper to temp path.";

    if ( !write_file( drv_tmp, driver_data ) )
    {
        delete_silent( kdm_tmp );
        return "Failed to write driver to temp path.";
    }

    // 3. Run kdmapper synchronously
    DWORD ec = 0;
    const std::wstring args = L"\"" + drv_tmp + L"\"";
    const bool ran = run_wait( kdm_tmp, args, ec );

    // 4. Delete both temp files immediately
    delete_silent( kdm_tmp );
    delete_silent( drv_tmp );

    if ( !ran )
        return "Failed to launch kdmapper process.\n"
               "Ensure you are running as Administrator.";

    if ( ec != 0 )
        return "kdmapper failed with exit code " + std::to_string( ec ) +
               ".\nEnsure Secure Boot / DSE is disabled or test-signing is on.";

    // 5. Give the driver time to register its device object
    ::Sleep( 800 );
    return {};
}

} // namespace launcher
