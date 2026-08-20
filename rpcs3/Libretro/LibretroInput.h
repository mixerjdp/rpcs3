#pragma once

#include <cstdint>

namespace rpcs3::libretro
{
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
// snapshot into RPCS3's cellPad representation while holding g_pad_mutex.
// Returns true when port 1 exists and received the snapshot.
bool update_pad_state(const pad_state& state);

// RPCS3 can request rumble from its emulation threads. The libretro frontend
// consumes the pending values on the retro_run() thread, where frontend
// callbacks are safe to invoke.
bool take_rumble_state(unsigned port, std::uint16_t& strong, std::uint16_t& weak);
} // namespace rpcs3::libretro
