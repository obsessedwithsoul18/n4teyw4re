#include <stdafx.hpp>
#include <algorithm>
#include <cstring>
#include <driver/driver_client.hpp>
#include <driver/protocol.hpp>

namespace n4teyw4re::driver
{
    client::~client( )
    {
        disconnect( );
    }

    bool client::connect( ) noexcept
    {
        disconnect( );

        handle_ = ::CreateFileW(
            device_path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr );

        if ( handle_ == INVALID_HANDLE_VALUE )
            return false;

        ping_request request{};
        request.Protocol = N4TEY_PROTOCOL_VERSION;
        ping_response response{};
        DWORD returned{};

        if ( !::DeviceIoControl(
                handle_, IOCTL_N4TEY_PING,
                &request, static_cast<DWORD>( sizeof( request ) ),
                &response, static_cast<DWORD>( sizeof( response ) ),
                &returned, nullptr )
            || returned < sizeof( response )
            || response.Protocol != N4TEY_PROTOCOL_VERSION )
        {
            disconnect( );
            return false;
        }

        driver_version_ = response.DriverVersion;
        return true;
    }

    void client::disconnect( ) noexcept
    {
        driver_version_ = 0;
        if ( handle_ != INVALID_HANDLE_VALUE )
        {
            ::CloseHandle( handle_ );
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool client::connected( ) const noexcept
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    std::uint32_t client::driver_version( ) const noexcept
    {
        return driver_version_;
    }

    std::optional<status> client::get_status( ) const noexcept
    {
        if ( !connected( ) )
            return std::nullopt;

        status_response response{};
        DWORD returned{};
        if ( !::DeviceIoControl(
                handle_, IOCTL_N4TEY_GET_STATUS,
                nullptr, 0,
                &response, static_cast<DWORD>( sizeof( response ) ),
                &returned, nullptr )
            || returned < sizeof( response ) )
            return std::nullopt;

        return status{
            response.Protocol,
            response.DriverVersion,
            response.Uptime100ns,
            response.RequestCount,
            response.OpenHandles,
            response.MessageLength };
    }

    std::optional<std::string> client::echo( std::string_view text ) const
    {
        if ( !connected( ) )
            return std::nullopt;

        echo_packet packet{};
        packet.Protocol = N4TEY_PROTOCOL_VERSION;
        packet.Length = static_cast<ULONG>( std::min<std::size_t>( text.size( ), N4TEY_MESSAGE_CAPACITY ) );
        if ( packet.Length )
            std::memcpy( packet.Data, text.data( ), packet.Length );

        DWORD returned{};
        if ( !::DeviceIoControl(
                handle_, IOCTL_N4TEY_ECHO,
                &packet, static_cast<DWORD>( sizeof( packet ) ),
                &packet, static_cast<DWORD>( sizeof( packet ) ),
                &returned, nullptr )
            || returned < sizeof( ULONG ) * 2
            || packet.Protocol != N4TEY_PROTOCOL_VERSION
            || packet.Length > N4TEY_MESSAGE_CAPACITY )
            return std::nullopt;

        return std::string( reinterpret_cast<const char*>( packet.Data ), packet.Length );
    }

    bool client::set_message( std::string_view text ) const noexcept
    {
        if ( !connected( ) )
            return false;

        message_packet packet{};
        packet.Protocol = N4TEY_PROTOCOL_VERSION;
        packet.Length = static_cast<ULONG>( std::min<std::size_t>( text.size( ), N4TEY_MESSAGE_CAPACITY ) );
        if ( packet.Length )
            std::memcpy( packet.Data, text.data( ), packet.Length );

        DWORD returned{};
        return !!::DeviceIoControl(
            handle_, IOCTL_N4TEY_SET_MESSAGE,
            &packet, static_cast<DWORD>( sizeof( packet ) ),
            nullptr, 0, &returned, nullptr );
    }

    std::optional<std::string> client::get_message( ) const
    {
        if ( !connected( ) )
            return std::nullopt;

        message_packet packet{};
        DWORD returned{};
        if ( !::DeviceIoControl(
                handle_, IOCTL_N4TEY_GET_MESSAGE,
                nullptr, 0,
                &packet, static_cast<DWORD>( sizeof( packet ) ),
                &returned, nullptr )
            || returned < sizeof( ULONG ) * 2
            || packet.Protocol != N4TEY_PROTOCOL_VERSION
            || packet.Length > N4TEY_MESSAGE_CAPACITY )
            return std::nullopt;

        return std::string( reinterpret_cast<const char*>( packet.Data ), packet.Length );
    }
}
