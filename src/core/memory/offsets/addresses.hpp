

#pragma once

#include <cstdint>

namespace dword_table {

	// Updated from a2x/cs2-dumper output dated 2026-09-10
	inline constexpr std::uintptr_t csgo_input{ 0x23E2610 };
	inline constexpr std::uintptr_t entity_list{ 0x2577BE0 };
	inline constexpr std::uintptr_t local_player_controller{ 0x23A78D0 };
	inline constexpr std::uintptr_t global_vars{ 0x20B57C0 };
	inline constexpr std::uintptr_t view_matrix{ 0x23D21F0 };

}
