#pragma once

#include <Windows.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace n4teyw4re::driver
{
    struct status
    {
        std::uint32_t protocol{};
        std::uint32_t driver_version{};
        std::uint64_t uptime_100ns{};
        std::uint64_t request_count{};
        std::uint32_t open_handles{};
        std::uint32_t message_length{};
    };

    class client
    {
    public:
        client( ) = default;
        client( const client& ) = delete;
        client& operator=( const client& ) = delete;
        ~client( );

        bool connect( ) noexcept;
        void disconnect( ) noexcept;
        [[nodiscard]] bool connected( ) const noexcept;
        [[nodiscard]] std::uint32_t driver_version( ) const noexcept;

        [[nodiscard]] std::optional<status> get_status( ) const noexcept;
        [[nodiscard]] std::optional<std::string> echo( std::string_view text ) const;
        bool set_message( std::string_view text ) const noexcept;
        [[nodiscard]] std::optional<std::string> get_message( ) const;

    private:
        HANDLE handle_{ INVALID_HANDLE_VALUE };
        std::uint32_t driver_version_{};
    };
}
