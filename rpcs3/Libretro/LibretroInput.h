#pragma once

#include <cstdint>

namespace rpcs3::libretro
{
// RetroArch exposes controller ports independently.  The PS3 itself can
// expose seven pad ports, but the first four are the portable libretro
// baseline used by this core and cover the multiplayer layouts of the games
// we target here.
inline constexpr unsigned max_libretro_players = 4;

struct pad_state
{
	bool connected = true;
	bool up = false;
	bool down = false;
	bool left = false;
	bool right = false;
	bool cross = false;
	bool circle = false;
	bool square = false;
	bool triangle = false;
	bool l1 = false;
	bool r1 = false;
	bool l2 = false;
	bool r2 = false;
	bool l3 = false;
	bool r3 = false;
	bool select = false;
	bool start = false;
	std::int16_t left_x = 0;
	std::int16_t left_y = 0;
	std::int16_t right_x = 0;
	std::int16_t right_y = 0;
};

// Called from the frontend thread once per retro_run(). The adapter copies the
// snapshot into the requested RPCS3 cellPad port while holding g_pad_mutex.
// Returns true when the requested port exists and received the snapshot.
bool update_pad_state(unsigned port, const pad_state& state);

// RPCS3 can request rumble from its emulation threads. The libretro frontend
// consumes the pending values on the retro_run() thread, where frontend
// callbacks are safe to invoke.
bool take_rumble_state(unsigned port, std::uint16_t& strong, std::uint16_t& weak);
} // namespace rpcs3::libretro
