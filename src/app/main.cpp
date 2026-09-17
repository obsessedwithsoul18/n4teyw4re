#include <stdafx.hpp>
#include <app/context.hpp>
#include <app/workers.hpp>
#include <driver/protocol.hpp>
#include <render/overlay/ui.hpp>
#include <scripting/runtime.hpp>
#include <launcher/launcher_ui.hpp>

#include <timeapi.h>
#include <chrono>
#include <thread>
#pragma comment( lib, "winmm.lib" )

namespace
{
	struct timer_period_guard
	{
		timer_period_guard( ) { timeBeginPeriod( 1 ); }
		~timer_period_guard( ) { timeEndPeriod( 1 ); }
	};

	struct single_instance_guard
	{
		single_instance_guard( )
		{
			handle = CreateMutexW( nullptr, FALSE, L"Local\\n4teyw4re.overlay.single-instance" );
			acquired = handle != nullptr && GetLastError( ) != ERROR_ALREADY_EXISTS;
		}

		~single_instance_guard( )
		{
			if ( handle != nullptr )
			{
				CloseHandle( handle );
			}
		}

		HANDLE handle{};
		bool acquired{};
	};

}

int main( )
{
	::SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );

	// ------------------------------------------------------------------
	// Launcher gate
	//
	// On first launch the command line has no --n4teyw4re-ready flag.
	// We show the mode-selection GUI, map the driver, then re-launch self
	// with --n4teyw4re-ready --km  or  --n4teyw4re-ready --um and exit.
	//
	// The re-launched process arrives here with --n4teyw4re-ready present
	// and skips straight to the app.
	// ------------------------------------------------------------------
	{
		const std::wstring cmdline( ::GetCommandLineW() );
		const bool ready = cmdline.find( L"--n4teyw4re-ready" ) != std::wstring::npos;

		if ( !ready )
		{
			launcher_ui::run(); // shows GUI, maps driver, re-launches self
			return 0;           // this instance is done
		}

		// Determine which mode was chosen by the launcher
		const bool km_mode = cmdline.find( L"--km" ) != std::wstring::npos;
		// Store it somewhere the driver wait below can use
		// (we re-use a simple local; the driver is already mapped)
		(void)km_mode; // used implicitly — driver is already up
	}

#if defined( VESTA_PERF_LOG ) && VESTA_PERF_LOG
	const DWORD ui_access_result = ERROR_SUCCESS;
#else
	if ( !ui_access::elevated( ) )
	{
		const DWORD elevation_result = ui_access::elevate( );
		return elevation_result == ERROR_SUCCESS ? 0 : 1;
	}
	const DWORD ui_access_result = ui_access::prepare( );
#endif
	const single_instance_guard instance{};
	if ( !instance.acquired )
	{
		return 0;
	}

	{
		if ( !app::context().diagnostics.open( " :> " ) )
		{
			return 1;
		}

		if ( ui_access_result != ERROR_SUCCESS )
		{
			app::context().diagnostics.warning(
				"UIAccess initialization failed with Win32 error {}; continuing without UIAccess.",
				ui_access_result );
		}
	}

	const timer_period_guard timer_period{};

	// ------------------------------------------------------------------
	// Driver connect
	//
	// The launcher already mapped the driver and waited 1.2 s for it to
	// initialise, so we do a single fast connect attempt.  If it fails
	// (e.g. the user somehow bypassed the launcher) we fall back to a
	// short retry window of 5 s so the experience stays snappy.
	// ------------------------------------------------------------------
	{
		constexpr auto fast_timeout  = std::chrono::seconds( 5 );
		constexpr auto retry_delay   = std::chrono::milliseconds( 200 );
		const auto     deadline      = std::chrono::steady_clock::now() + fast_timeout;

		app::context().diagnostics.info( "Connecting to N4TEYW4RE driver..." );

		bool driver_ready = false;
		while ( std::chrono::steady_clock::now() < deadline )
		{
			if ( app::context().driver.connect() )
			{
				const auto status = app::context().driver.get_status();
				const auto echoed = app::context().driver.echo( "N4TEYW4RE handshake" );

				if ( status
					&& status->protocol      == n4teyw4re::driver::protocol_version
					&& status->driver_version == app::context().driver.driver_version()
					&& echoed && *echoed      == "N4TEYW4RE handshake" )
				{
					driver_ready = true;
					app::context().diagnostics.info(
						"N4TEYW4RE driver verified (protocol {}, driver version {}, uptime_ms={}).",
						status->protocol, status->driver_version,
						status->uptime_100ns / 10000ull );
					app::context().driver.set_message( "N4TEYW4RE ready" );
					break;
				}

				app::context().driver.disconnect();
			}

			std::this_thread::sleep_for( retry_delay );
		}

		if ( !driver_ready )
		{
			app::context().diagnostics.warning(
				"N4TEYW4RE driver was not detected or failed the handshake." );
			::MessageBoxW(
				nullptr,
				L"N4TEYW4RE driver was not detected or failed verification.\n\n"
				L"Please use the launcher to map the driver before running.",
				L"N4TEYW4RE - Driver Required",
				MB_OK | MB_ICONERROR );
			return 1;
		}
	}

	{

		if ( !app::context().process.attach( L"cs2.exe" ) )
		{
			return 1;
		}

		if ( !app::context().input.connect( ) )
		{
			return 1;
		}
	}

	{
		if ( !app::context().modules.discover( app::context().process ) )
		{
			return 1;
		}

		if ( !app::context().addresses.initialize( ) )
		{
			return 1;
		}

		if ( !game::fields( ).initialize( ) )
		{
			return 1;
		}
	}

	{

		config::apply_default_config( );
		if ( !config::storage.read_cache( ) )
		{
			config::storage.write_cache( );
		}
		if ( !scripting::runtime().initialize( ) )
		{
			app::context().diagnostics.warning(
				"Lua runtime storage could not be initialized; scripting is disabled." );
			config::general_settings.lua_enabled = false;
		}

		const auto game_process = static_cast<HANDLE>(
			app::context().process.native_handle( ) );
		std::thread( [game_process]
		{
			if ( ::WaitForSingleObject( game_process, INFINITE ) != WAIT_OBJECT_0 )
				return;

			::ClipCursor( nullptr );
			app::context().input.set_key_gate( 0, false );
			app::context().input.set_movement_gate( {}, false );
			app::context().input.key( VK_ESCAPE, false );
			const std::array movement_releases{
				platform::windows::input_gateway::key_transition{ VK_CONTROL, false },
				platform::windows::input_gateway::key_transition{ VK_F24, false },
			};
			app::context().input.keys( movement_releases );
			app::context().input.pointer(
				0, 0,
				platform::windows::pointer_action::primary_up
					| platform::windows::pointer_action::secondary_up );

			app::context().overlay.request_shutdown( );
		} ).detach( );

		std::thread( app::workers::game ).detach( );
		std::thread( app::workers::pose_sampler ).detach( );
		std::thread( app::workers::movement ).detach( );
		std::thread( app::workers::combat ).detach( );
		std::thread( app::workers::nade_helper ).detach( );
		std::thread( app::workers::seed_trigger ).detach( );
#if defined( VESTA_ENABLE_CONSOLE ) && VESTA_ENABLE_CONSOLE
		std::thread( app::workers::watchdog ).detach( );
#endif

		if ( !app::context().overlay.launch( ) )
		{
			scripting::runtime().shutdown( );
			return 1;
		}
	}
	scripting::runtime().shutdown( );

	::ExitProcess( EXIT_SUCCESS );
}
