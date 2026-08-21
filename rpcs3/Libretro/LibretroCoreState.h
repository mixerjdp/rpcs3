#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <libretro.h>

namespace rpcs3::libretro
{
class emulator_bridge;
enum class audio_pull_state;

class core_state final
{
public:
	static constexpr unsigned video_width = 1280;
	static constexpr unsigned video_height = 720;
	static constexpr double frames_per_second = 60.0;
	static constexpr double audio_sample_rate = 48000.0;
	static constexpr std::size_t audio_frames_per_run = 800;

	core_state();
	~core_state();

	void set_environment(retro_environment_t callback);
	void set_video_refresh(retro_video_refresh_t callback);
	void set_audio_sample(retro_audio_sample_t callback);
	void set_audio_sample_batch(retro_audio_sample_batch_t callback);
	void set_input_poll(retro_input_poll_t callback);
	void set_input_state(retro_input_state_t callback);

	void init();
	void deinit();
	void reset();
	void run();

	bool load_game(const retro_game_info* game);
	void unload_game();
	void set_controller_port_device(unsigned port, unsigned device);

	void log(enum retro_log_level level, const char* message) const;
	void show_message(const char* message, unsigned frames = 300) const;

private:
	void register_frontend_capabilities();
	void update_input();
	void update_rumble();
	void submit_audio();
	void update_progress_overlay(bool frame_received);
	std::string get_system_directory() const;
	std::string get_save_directory() const;

	retro_environment_t m_environment = nullptr;
	retro_video_refresh_t m_video_refresh = nullptr;
	retro_audio_sample_t m_audio_sample = nullptr;
	retro_audio_sample_batch_t m_audio_sample_batch = nullptr;
	retro_input_poll_t m_input_poll = nullptr;
	retro_input_state_t m_input_state = nullptr;
	retro_log_printf_t m_log = nullptr;
	retro_rumble_interface m_rumble{};

	std::vector<std::uint32_t> m_framebuffer;
	std::unique_ptr<emulator_bridge> m_emulator;
	std::array<std::int16_t, audio_frames_per_run * 2> m_audio_buffer{};
	unsigned m_video_width = video_width;
	unsigned m_video_height = video_height;
	unsigned m_controller_device = RETRO_DEVICE_JOYPAD;
	std::uint64_t m_frame_counter = 0;
	bool m_initialized = false;
	bool m_content_loaded = false;
	bool m_input_activity_logged = false;
	bool m_audio_stream_logged = false;
	bool m_audio_activity_logged = false;
	bool m_audio_diagnostic_logged = false;
	bool m_progress_overlay_active = false;
	std::string m_progress_message;
	std::vector<std::uint32_t> m_progress_base_framebuffer;
};

core_state& get_core_state();
} // namespace rpcs3::libretro
