#include "LibretroCoreState.h"
#include "LibretroEmulator.h"
#include "LibretroInput.h"

#include "Emu/system_progress.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace rpcs3::libretro
{
namespace
{
core_state g_core_state;

struct progress_snapshot
{
	std::string title;
	std::uint32_t done = 0;
	std::uint32_t total = 0;

	bool active() const
	{
		// The RPCS3 progress server can leave the last completed counters
		// published briefly while it tears down a dialog. Do not keep covering
		// the first game frame once a known unit of work reached its total.
		return !title.empty() && (total == 0 || done < total);
	}
};

progress_snapshot read_progress_snapshot()
{
	progress_snapshot progress;
	progress.title = g_progr_text.operator std::string();
	const std::uint32_t module_total = static_cast<std::uint32_t>(+g_progr_ptotal);
	const std::uint32_t module_done = static_cast<std::uint32_t>(+g_progr_pdone);
	const std::uint32_t file_total = static_cast<std::uint32_t>(+g_progr_ftotal);
	const std::uint32_t file_done = static_cast<std::uint32_t>(+g_progr_fdone);

	if (module_total != 0 || module_done != 0)
	{
		progress.done = module_done;
		progress.total = module_total;
	}
	else
	{
		progress.done = file_done;
		progress.total = file_total;
	}

	return progress;
}

void draw_progress_bar(std::vector<std::uint32_t>& framebuffer, unsigned width, unsigned height, std::uint32_t value)
{
	if (framebuffer.size() < static_cast<std::size_t>(width) * height || width < 32 || height < 32)
	{
		return;
	}

	const unsigned panel_left = width / 10;
	const unsigned panel_right = width - panel_left;
	const unsigned panel_top = height * 3 / 5;
	const unsigned panel_bottom = std::min(height - 8, panel_top + 86);
	const unsigned bar_left = panel_left + 8;
	const unsigned bar_right = panel_right - 8;
	const unsigned bar_top = panel_top + 48;
	const unsigned bar_bottom = std::min(panel_bottom - 8, bar_top + 18);

	const auto fill_rect = [&](unsigned left, unsigned top, unsigned right, unsigned bottom, std::uint32_t color)
	{
		for (unsigned y = top; y < bottom; ++y)
		{
			std::fill(framebuffer.begin() + static_cast<std::size_t>(y) * width + left,
				framebuffer.begin() + static_cast<std::size_t>(y) * width + right, color);
		}
	};

	fill_rect(panel_left, panel_top, panel_right, panel_bottom, 0x101820);
	fill_rect(bar_left, bar_top, bar_right, bar_bottom, 0x343D4A);
	const unsigned clamped = std::min(value, 100u);
	const unsigned filled_right = bar_left + static_cast<unsigned>((static_cast<std::uint64_t>(bar_right - bar_left) * clamped) / 100);
	if (filled_right > bar_left)
	{
		fill_rect(bar_left, bar_top, filled_right, bar_bottom, 0x4CA6E8);
	}
}

constexpr std::array<retro_input_descriptor, max_libretro_players * 16 + 1> input_descriptors = []
{
	std::array<retro_input_descriptor, max_libretro_players * 16 + 1> descriptors{};
	std::size_t index = 0;
	const auto add_player_descriptors = [&descriptors, &index](unsigned port)
	{
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Cross"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Circle"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Square"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Triangle"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L1"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R1"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"};
		descriptors[index++] = {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"};
	};

	for (unsigned port = 0; port < max_libretro_players; ++port)
	{
		add_player_descriptors(port);
	}

	return descriptors;
}();

constexpr std::array<retro_variable, 3> core_options{{
	{"rpcs3_resolution_scale", "Resolution Scale; 100%|150%|200%|300%"},
	{"rpcs3_dev_hdd0_location", "Dev HDD0 Location; System|Saves"},
	{nullptr, nullptr},
}};
} // namespace

core_state::core_state()
{
	m_controller_devices.fill(RETRO_DEVICE_JOYPAD);
}
core_state::~core_state() = default;

core_state& get_core_state()
{
	return g_core_state;
}

void core_state::set_environment(retro_environment_t callback)
{
	m_environment = callback;
	register_frontend_capabilities();
}

void core_state::set_video_refresh(retro_video_refresh_t callback)
{
	m_video_refresh = callback;
}

void core_state::set_audio_sample(retro_audio_sample_t callback)
{
	m_audio_sample = callback;
}

void core_state::set_audio_sample_batch(retro_audio_sample_batch_t callback)
{
	m_audio_sample_batch = callback;
}

void core_state::set_input_poll(retro_input_poll_t callback)
{
	m_input_poll = callback;
}

void core_state::set_input_state(retro_input_state_t callback)
{
	m_input_state = callback;
}

void core_state::register_frontend_capabilities()
{
	m_log = nullptr;
	m_rumble = {};

	if (!m_environment)
	{
		return;
	}

	bool supports_no_game = false;
	m_environment(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &supports_no_game);

	retro_log_callback log_callback{};
	if (m_environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_callback))
	{
		m_log = log_callback.log;
	}

	m_environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
		const_cast<retro_input_descriptor*>(input_descriptors.data()));
	m_environment(RETRO_ENVIRONMENT_SET_VARIABLES,
		const_cast<retro_variable*>(core_options.data()));
	m_environment(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &m_rumble);
}

unsigned core_state::get_resolution_scale_percent() const
{
	retro_variable variable{"rpcs3_resolution_scale", nullptr};
	if (m_environment && m_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) && variable.value)
	{
		const std::string_view setting(variable.value);
		if (setting == "150%")
		{
			return 150;
		}
		if (setting == "200%")
		{
			return 200;
		}
		if (setting == "300%")
		{
			return 300;
		}
	}

	return 100;
}

bool core_state::get_dev_hdd0_in_system() const
{
	retro_variable variable{"rpcs3_dev_hdd0_location", nullptr};
	if (m_environment && m_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) && variable.value)
	{
		return std::string_view{variable.value} != "Saves";
	}

	// System is the first/default value in the core option list and is also
	// the safest location for a fresh portable RetroArch installation.
	return true;
}

void core_state::init()
{
	if (m_initialized)
	{
		return;
	}

	register_frontend_capabilities();

	if (m_environment)
	{
		retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_XRGB8888;
		if (!m_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixel_format))
		{
			log(RETRO_LOG_ERROR, "The frontend rejected the required XRGB8888 pixel format.");
		}
	}

	m_framebuffer.assign(static_cast<std::size_t>(video_width) * video_height, 0);
	m_video_width = video_width;
	m_video_height = video_height;
	m_emulator = std::make_unique<emulator_bridge>();

	std::string error;
	const std::string system_directory = get_system_directory();
	std::string save_directory = get_save_directory();
	if (save_directory.empty())
	{
		// Older frontends may not expose RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY.
		// Keep the core usable there while still preferring the save root.
		save_directory = system_directory;
	}

	if (!m_emulator->initialize(system_directory, save_directory, error))
	{
		log(RETRO_LOG_ERROR, error.c_str());
		show_message(error.c_str(), 600);
		m_emulator.reset();
	}
	else
	{
		log(RETRO_LOG_INFO, "RPCS3 emulator bridge initialized.");
	}

	m_initialized = true;
	log(RETRO_LOG_INFO, "RPCS3 libretro video, audio and RetroPad bridge initialized.");
}

void core_state::deinit()
{
	if (!m_initialized)
	{
		return;
	}

	unload_game();
	if (m_rumble.set_rumble_state)
	{
		for (unsigned port = 0; port < max_libretro_players; ++port)
		{
			m_rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, 0);
			m_rumble.set_rumble_state(port, RETRO_RUMBLE_WEAK, 0);
		}
	}
	m_emulator.reset();
	m_framebuffer.clear();
	m_framebuffer.shrink_to_fit();
	m_initialized = false;
	log(RETRO_LOG_INFO, "RPCS3 libretro bridge deinitialized.");
	m_log = nullptr;
}

void core_state::reset()
{
	if (!m_emulator)
	{
		return;
	}

	std::string error;
	if (!m_emulator->restart(error))
	{
		log(RETRO_LOG_ERROR, error.c_str());
		show_message(error.c_str());
		return;
	}

	m_frame_counter = 0;
	log(RETRO_LOG_INFO, "RPCS3 title reset requested.");
}

void core_state::run()
{
	if (m_input_poll)
	{
		m_input_poll();
	}

	// RPCS3 does not create its pad thread until content starts. Poll the
	// frontend unconditionally, but only touch cellPad once a title is loaded.
	if (m_content_loaded)
	{
		update_input();
	}

	bool fast_forward = false;
	bool frame_stepping = false;
	if (m_environment)
	{
		bool frontend_fast_forward = false;
		if (m_environment(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &frontend_fast_forward))
		{
			fast_forward = frontend_fast_forward;
		}

		retro_throttle_state throttle_state{};
		if (m_environment(RETRO_ENVIRONMENT_GET_THROTTLE_STATE, &throttle_state))
		{
			fast_forward = fast_forward || throttle_state.mode == RETRO_THROTTLE_FAST_FORWARD;
			frame_stepping = throttle_state.mode == RETRO_THROTTLE_FRAME_STEPPING;
		}
	}

	bool frame_received = false;
	if (m_emulator)
	{
		m_emulator->set_fast_forward(fast_forward);

		if (m_content_loaded && frame_stepping)
		{
			// libretro frontends call retro_run once for each requested step. The
			// bridge resumes RPCS3, waits for one present (with a bounded timeout),
			// and pauses again so a frame-step cannot spin at thousands of FPS.
			m_emulator->step_frame();
			m_frame_step_active = true;
		}
		else if (m_frame_step_active)
		{
			// Leaving the frontend's frame-step mode returns the title to normal
			// running before the next frame is consumed.
			m_emulator->resume();
			m_frame_step_active = false;
		}

		video_frame frame;
		if (m_emulator->take_frame(frame) && !frame.pixels.empty())
		{
			frame_received = true;
			const bool geometry_changed = frame.width != m_video_width || frame.height != m_video_height;
			m_framebuffer = std::move(frame.pixels);
			m_video_width = frame.width;
			m_video_height = frame.height;

			if (geometry_changed && m_environment)
			{
				retro_game_geometry geometry{};
				geometry.base_width = m_video_width;
				geometry.base_height = m_video_height;
				geometry.max_width = 4096;
				geometry.max_height = 4096;
				geometry.aspect_ratio = 16.0f / 9.0f;
				m_environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
			}
		}
	}

	if (m_content_loaded)
	{
		update_progress_overlay(frame_received);
	}

	if (m_video_refresh && !m_framebuffer.empty())
	{
		m_video_refresh(m_framebuffer.data(), m_video_width, m_video_height,
			static_cast<std::size_t>(m_video_width) * sizeof(std::uint32_t));
	}

	submit_audio();
	update_rumble();

	++m_frame_counter;
}

bool core_state::load_game(const retro_game_info* game)
{
	if (!m_initialized || !m_emulator)
	{
		log(RETRO_LOG_ERROR, "RPCS3 is not initialized.");
		return false;
	}

	if (!game || !game->path || std::string_view{game->path}.empty())
	{
		log(RETRO_LOG_ERROR, "A full PS3 game path is required.");
		show_message("RPCS3: select a PS3 ISO or game directory.");
		return false;
	}

	std::string error;
	m_emulator->set_resolution_scale(get_resolution_scale_percent());
	m_emulator->set_dev_hdd0_location(get_dev_hdd0_in_system());
	show_message("RPCS3: preparing PPU/SPU caches...", 600);
	if (!m_emulator->boot(game->path, error))
	{
		log(RETRO_LOG_ERROR, error.c_str());
		show_message(error.c_str(), 600);
		return false;
	}

	m_content_loaded = true;
	m_frame_counter = 0;
	m_input_activity_logged.fill(false);
	m_audio_stream_logged = false;
	m_audio_activity_logged = false;
	m_audio_diagnostic_logged = false;
	m_frame_step_active = false;
	m_progress_overlay_active = false;
	m_progress_message.clear();
	m_progress_base_framebuffer.clear();
	log(RETRO_LOG_INFO, "RPCS3 accepted the PS3 content and started emulation.");
	show_message("RPCS3: emulation started; waiting for the first rendered frame.", 240);
	return true;
}

void core_state::unload_game()
{
	if (!m_content_loaded)
	{
		return;
	}

	if (m_emulator)
	{
		m_emulator->stop();
	}

	m_content_loaded = false;
	m_frame_counter = 0;
	m_frame_step_active = false;
	m_progress_overlay_active = false;
	m_progress_message.clear();
	m_progress_base_framebuffer.clear();
	if (m_rumble.set_rumble_state)
	{
		for (unsigned port = 0; port < max_libretro_players; ++port)
		{
			m_rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, 0);
			m_rumble.set_rumble_state(port, RETRO_RUMBLE_WEAK, 0);
		}
	}
	log(RETRO_LOG_INFO, "RPCS3 content unloaded.");
}

void core_state::set_controller_port_device(unsigned port, unsigned device)
{
	if (port >= max_libretro_players)
	{
		return;
	}

	const unsigned base_device = device & RETRO_DEVICE_MASK;
	if (base_device == RETRO_DEVICE_NONE || base_device == RETRO_DEVICE_JOYPAD || base_device == RETRO_DEVICE_ANALOG)
	{
		m_controller_devices[port] = base_device;
		return;
	}

	m_controller_devices[port] = RETRO_DEVICE_NONE;
	const std::string message = "Unsupported controller type on port " + std::to_string(port + 1) + "; the PS3 pad was disconnected.";
	log(RETRO_LOG_WARN, message.c_str());
}

void core_state::update_input()
{
	for (unsigned port = 0; port < max_libretro_players; ++port)
	{
		pad_state state{};
		state.connected = m_controller_devices[port] != RETRO_DEVICE_NONE;

		if (state.connected && m_input_state)
		{
			const auto pressed = [this, port](unsigned id)
			{
				return m_input_state(port, RETRO_DEVICE_JOYPAD, 0, id) != 0;
			};

			state.up = pressed(RETRO_DEVICE_ID_JOYPAD_UP);
			state.down = pressed(RETRO_DEVICE_ID_JOYPAD_DOWN);
			state.left = pressed(RETRO_DEVICE_ID_JOYPAD_LEFT);
			state.right = pressed(RETRO_DEVICE_ID_JOYPAD_RIGHT);
			state.cross = pressed(RETRO_DEVICE_ID_JOYPAD_B);
			state.circle = pressed(RETRO_DEVICE_ID_JOYPAD_A);
			state.square = pressed(RETRO_DEVICE_ID_JOYPAD_Y);
			state.triangle = pressed(RETRO_DEVICE_ID_JOYPAD_X);
			state.l1 = pressed(RETRO_DEVICE_ID_JOYPAD_L);
			state.r1 = pressed(RETRO_DEVICE_ID_JOYPAD_R);
			state.l2 = pressed(RETRO_DEVICE_ID_JOYPAD_L2);
			state.r2 = pressed(RETRO_DEVICE_ID_JOYPAD_R2);
			state.l3 = pressed(RETRO_DEVICE_ID_JOYPAD_L3);
			state.r3 = pressed(RETRO_DEVICE_ID_JOYPAD_R3);
			state.select = pressed(RETRO_DEVICE_ID_JOYPAD_SELECT);
			state.start = pressed(RETRO_DEVICE_ID_JOYPAD_START);
			state.left_x = m_input_state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
			state.left_y = m_input_state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
			state.right_x = m_input_state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
			state.right_y = m_input_state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
		}

		const bool pad_updated = update_pad_state(port, state);

		const bool button_activity = state.up || state.down || state.left || state.right ||
			state.cross || state.circle || state.square || state.triangle ||
			state.l1 || state.r1 || state.l2 || state.r2 || state.l3 || state.r3 ||
			state.select || state.start;
		const bool analog_activity = std::abs(static_cast<int>(state.left_x)) > 2048 ||
			std::abs(static_cast<int>(state.left_y)) > 2048 ||
			std::abs(static_cast<int>(state.right_x)) > 2048 ||
			std::abs(static_cast<int>(state.right_y)) > 2048;

		if (!m_input_activity_logged[port] && pad_updated && (button_activity || analog_activity))
		{
			m_input_activity_logged[port] = true;
			const std::string message = "RetroPad input activity reached RPCS3 cellPad port " + std::to_string(port + 1) + ".";
			log(RETRO_LOG_INFO, message.c_str());
		}
	}
}

void core_state::update_rumble()
{
	if (!m_rumble.set_rumble_state)
	{
		return;
	}

	for (unsigned port = 0; port < max_libretro_players; ++port)
	{
		std::uint16_t strong = 0;
		std::uint16_t weak = 0;
		if (take_rumble_state(port, strong, weak))
		{
			m_rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, strong);
			m_rumble.set_rumble_state(port, RETRO_RUMBLE_WEAK, weak);
		}
	}
}

void core_state::submit_audio()
{
	audio_pull_state pull_state = audio_pull_state::closed;
	if (m_emulator)
	{
		pull_state = m_emulator->take_audio(std::span<std::int16_t>{m_audio_buffer});
	}
	else
	{
		m_audio_buffer.fill(0);
	}

	if (pull_state == audio_pull_state::data && !m_audio_stream_logged)
	{
		m_audio_stream_logged = true;
		log(RETRO_LOG_INFO, "RPCS3 audio stream is feeding 48 kHz stereo samples to RetroArch.");
	}

	if (!m_audio_activity_logged && std::any_of(m_audio_buffer.begin(), m_audio_buffer.end(), [](std::int16_t sample)
	{
		return sample != 0;
	}))
	{
		m_audio_activity_logged = true;
		log(RETRO_LOG_INFO, "RPCS3 delivered non-silent game audio to RetroArch.");
	}

	if (!m_audio_diagnostic_logged && m_frame_counter >= 300 && pull_state != audio_pull_state::data)
	{
		m_audio_diagnostic_logged = true;
		switch (pull_state)
		{
		case audio_pull_state::closed:
			log(RETRO_LOG_WARN, "RPCS3 has not opened its libretro audio backend yet.");
			break;
		case audio_pull_state::paused:
			log(RETRO_LOG_WARN, "RPCS3 opened the libretro audio backend but has not started playback yet.");
			break;
		case audio_pull_state::missing_callback:
			log(RETRO_LOG_WARN, "RPCS3 opened the audio backend without installing its mixer callback.");
			break;
		case audio_pull_state::empty:
			log(RETRO_LOG_WARN, "RPCS3 audio playback started, but its mixer queue is still empty.");
			break;
		case audio_pull_state::data:
			break;
		}
	}

	if (m_audio_sample_batch)
	{
		std::size_t submitted = 0;
		while (submitted < audio_frames_per_run)
		{
			const std::size_t consumed = m_audio_sample_batch(
				m_audio_buffer.data() + submitted * 2,
				audio_frames_per_run - submitted);
			if (consumed == 0)
			{
				break;
			}
			submitted += std::min(consumed, audio_frames_per_run - submitted);
		}
	}
	else if (m_audio_sample)
	{
		for (std::size_t frame = 0; frame < audio_frames_per_run; ++frame)
		{
			m_audio_sample(m_audio_buffer[frame * 2], m_audio_buffer[frame * 2 + 1]);
		}
	}
}

void core_state::update_progress_overlay(bool frame_received)
{
	const progress_snapshot progress = read_progress_snapshot();
	if (!progress.active())
	{
		if (m_progress_overlay_active)
		{
			if (!frame_received && m_progress_base_framebuffer.size() == m_framebuffer.size())
			{
				m_framebuffer = m_progress_base_framebuffer;
			}
			m_progress_overlay_active = false;
			m_progress_message.clear();
			m_progress_base_framebuffer.clear();
			show_message("RPCS3: PPU/SPU preparation complete.", 90);
		}
		return;
	}

	const std::string title = progress.title.empty() ? "Preparing RPCS3 caches..." : progress.title;
	const std::uint32_t percent = progress.total != 0
		? std::min<std::uint32_t>(100, (static_cast<std::uint64_t>(progress.done) * 100) / progress.total)
		: 0;

	if (!m_progress_overlay_active || frame_received || m_progress_base_framebuffer.size() != m_framebuffer.size())
	{
		m_progress_base_framebuffer = m_framebuffer;
	}
	else
	{
		m_framebuffer = m_progress_base_framebuffer;
	}

	std::string message = "RPCS3: " + title;
	if (progress.total != 0)
	{
		message += "\nProgress: " + std::to_string(progress.done) + " of " + std::to_string(progress.total);
	}
	else
	{
		message += "\nPlease wait...";
	}

	if (!m_progress_overlay_active || m_progress_message != message || (m_frame_counter % 30) == 0)
	{
		show_message(message.c_str(), 120);
		m_progress_message = message;
	}

	draw_progress_bar(m_framebuffer, m_video_width, m_video_height, percent);
	m_progress_overlay_active = true;
}

void core_state::log(enum retro_log_level level, const char* message) const
{
	if (m_log && message)
	{
		m_log(level, "[RPCS3] %s\n", message);
	}
}

void core_state::show_message(const char* message, unsigned frames) const
{
	if (!m_environment || !message)
	{
		return;
	}

	retro_message frontend_message{message, frames};
	m_environment(RETRO_ENVIRONMENT_SET_MESSAGE, &frontend_message);
}

std::string core_state::get_system_directory() const
{
	const char* system_directory = nullptr;
	if (m_environment && m_environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory) &&
		system_directory && *system_directory)
	{
		return std::filesystem::u8path(system_directory).string();
	}

	return {};
}

std::string core_state::get_save_directory() const
{
	const char* save_directory = nullptr;
	if (m_environment && m_environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_directory) &&
		save_directory && *save_directory)
	{
		return std::filesystem::u8path(save_directory).string();
	}

	return {};
}
} // namespace rpcs3::libretro
