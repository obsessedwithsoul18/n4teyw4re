#pragma once

#include <Windows.h>
#include <cstdint>
#include <string_view>
#include "shared/n4tey_protocol.h"

namespace n4teyw4re::driver
{
    inline constexpr wchar_t device_path[] = L"\\\\.\\N4TEYW4RE";
    inline constexpr std::uint32_t protocol_version = N4TEY_PROTOCOL_VERSION;

    using ping_request = N4TEY_PING_REQUEST;
    using ping_response = N4TEY_PING_RESPONSE;
    using status_response = N4TEY_STATUS_RESPONSE;
    using echo_packet = N4TEY_ECHO_PACKET;
    using message_packet = N4TEY_MESSAGE_PACKET;
}
