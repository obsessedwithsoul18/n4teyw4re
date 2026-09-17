/*
 * driver_mapper.cpp
 *
 * Extracts kdmapper and both driver .sys files from embedded PE resources,
 * writes them to %TEMP%, and runs kdmapper against the chosen driver.
 *
 * AV evasion strategy:
 *   - kdmapper bytes are XOR-obfuscated in the resource section so the
 *     embedded blob never matches the known kdmapper PE signature.
 *   - At runtime the bytes are decrypted in memory, written to a temp path
 *     with a non-.exe extension (.tmp), and run via CreateProcess.
 *   - The .sys driver is written as-is (drivers are not flagged by name).
 *   - Both temp files are deleted immediately after use.
 *   - Zone.Identifier ADS is stripped from the written files.
 *
 * The XOR key used when embedding is stored in resource_ids.h and must
 * match what CMake uses when building resources.rc.in.
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

// XOR-decrypt a buffer in place using a simple rolling 4-byte key.
// The same key must be used when XOR-encrypting the resource at build time.
// Key bytes are derived from IDR_XOR_KEY defined in resource_ids.h.
static void xor_decrypt( std::vector<std::uint8_t>& buf )
{
    const std::uint8_t key[4] = {
        static_cast<std::uint8_t>(  IDR_XOR_KEY        & 0xFF ),
        static_cast<std::uint8_t>( (IDR_XOR_KEY >>  8) & 0xFF ),
        static_cast<std::uint8_t>( (IDR_XOR_KEY >> 16) & 0xFF ),
        static_cast<std::uint8_t>( (IDR_XOR_KEY >> 24) & 0xFF ),
    };
    for ( std::size_t i = 0; i < buf.size(); ++i )
        buf[i] ^= key[i & 3];
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
    // Strip the Zone.Identifier ADS — no SmartScreen prompt for temp files
    ::DeleteFileW( ( path + L":Zone.Identifier" ).c_str() );
    return true;
}

static void delete_silent( const std::wstring& path )
{
    ::DeleteFileW( path.c_str() );
}

// Run a process synchronously, hidden. Returns its exit code via |ec|.
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
    // 1. Load resources (kdmapper is XOR-encrypted in the resource section)
    std::vector<std::uint8_t> kdmapper_data, driver_data;

    if ( !load_resource( IDR_KDMAPPER, kdmapper_data ) || kdmapper_data.empty() )
        return "Failed to load embedded kdmapper resource.";

    // Decrypt kdmapper in memory — the on-disk resource bytes are XOR'd
    // so they don't match Defender's static PE signature for kdmapper.
    xor_decrypt( kdmapper_data );

    const WORD drv_id = km_mode ? IDR_DRIVER_KM : IDR_DRIVER_UM;
    if ( !load_resource( drv_id, driver_data ) || driver_data.empty() )
        return "Failed to load embedded driver resource.";

    // 2. Write both to temp files
    //    - kdmapper uses a .tmp extension (non-.exe avoids SmartScreen heuristic)
    //    - driver uses .sys (drivers are not flagged by extension)
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

    // 4. Clean up both temp files immediately
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
