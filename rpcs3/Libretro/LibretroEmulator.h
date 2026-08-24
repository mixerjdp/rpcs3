#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace rpcs3::libretro
{
struct video_frame
{
	std::vector<std::uint32_t> pixels;
	unsigned width = 0;
	unsigned height = 0;
};

enum class audio_pull_state
{
	closed,
	paused,
	missing_callback,
	empty,
	data,
};

class emulator_bridge final
{
public:
	emulator_bridge();
	~emulator_bridge();

	emulator_bridge(const emulator_bridge&) = delete;
	emulator_bridge& operator=(const emulator_bridge&) = delete;

	bool initialize(const std::string& system_directory, const std::string& save_directory, std::string& error);
	void set_resolution_scale(unsigned percent);
	void set_dev_hdd0_location(bool use_system_directory);
	void set_fast_forward(bool enabled);
	bool boot(const std::string& content_path, std::string& error);
	void stop();
	bool pause();
	void resume();
	bool step_frame(unsigned timeout_ms = 250);
	bool restart(std::string& error);
	bool take_frame(video_frame& frame);
	audio_pull_state take_audio(std::span<std::int16_t> interleaved_stereo);
	bool is_running() const;

private:
	class impl;
	std::unique_ptr<impl> m_impl;
};
} // namespace rpcs3::libretro
