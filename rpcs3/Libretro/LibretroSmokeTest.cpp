#include <libretro.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace
{
unsigned g_video_calls = 0;
unsigned g_progress_bar_frames = 0;
unsigned g_audio_calls = 0;
unsigned g_input_polls = 0;
std::size_t g_audio_frames = 0;
std::size_t g_nonzero_audio_samples = 0;
unsigned g_test_frame = 0;
bool g_received_message = false;
bool g_received_input_descriptors = false;
std::string g_last_message;
unsigned g_message_updates = 0;
const char* g_system_directory = ".";
const char* g_save_directory = ".";
const char* g_dev_hdd0_location = "System";

bool check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		return false;
	}

	return true;
}

void RETRO_CALLCONV log_message(enum retro_log_level level, const char* format, ...)
{
	std::fprintf(stdout, "core-log[%d]: ", static_cast<int>(level));
	va_list args;
	va_start(args, format);
	std::vfprintf(stdout, format, args);
	va_end(args);
	std::fflush(stdout);
}

bool RETRO_CALLCONV environment(unsigned command, void* data)
{
	switch (command)
	{
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		return data && *static_cast<bool*>(data);
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		if (data)
		{
			static_cast<retro_log_callback*>(data)->log = log_message;
			return true;
		}
		return false;
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
		g_received_input_descriptors = data != nullptr;
		return g_received_input_descriptors;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return data && *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
	case RETRO_ENVIRONMENT_SET_MESSAGE:
		if (data)
		{
			const auto* message = static_cast<const retro_message*>(data);
			g_received_message = message->msg && message->frames != 0;
			if (message->msg && g_last_message != message->msg)
			{
				g_last_message = message->msg;
				++g_message_updates;
				std::fprintf(stdout, "frontend-message: %s\n", message->msg);
				std::fflush(stdout);
			}
			return true;
		}
		return false;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		if (data)
		{
			*static_cast<const char**>(data) = g_system_directory;
			return true;
		}
		return false;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		if (data)
		{
			*static_cast<const char**>(data) = g_save_directory;
			return true;
		}
		return false;
	case RETRO_ENVIRONMENT_GET_VARIABLE:
		if (data)
		{
			auto* variable = static_cast<retro_variable*>(data);
			if (variable->key && std::strcmp(variable->key, "rpcs3_dev_hdd0_location") == 0)
			{
				variable->value = g_dev_hdd0_location;
				return true;
			}
		}
		return false;
	default:
		return false;
	}
}

void RETRO_CALLCONV video_refresh(const void* data, unsigned width, unsigned height, std::size_t pitch)
{
	if (data && width == 1280 && height == 720 && pitch == width * sizeof(std::uint32_t))
	{
		++g_video_calls;
		const auto* pixels = static_cast<const std::uint32_t*>(data);
		for (unsigned y = height * 3 / 5; y < height * 3 / 5 + 86 && g_progress_bar_frames == 0; y += 4)
		{
			for (unsigned x = width / 10; x < width - width / 10 && g_progress_bar_frames == 0; x += 4)
			{
				if (pixels[static_cast<std::size_t>(y) * width + x] == 0x101820 ||
					pixels[static_cast<std::size_t>(y) * width + x] == 0x4CA6E8)
				{
					++g_progress_bar_frames;
				}
			}
		}
	}
}

void RETRO_CALLCONV audio_sample(std::int16_t left, std::int16_t right)
{
	if (left == 0 && right == 0)
	{
		++g_audio_frames;
	}
}

std::size_t RETRO_CALLCONV audio_sample_batch(const std::int16_t* data, std::size_t frames)
{
	if (data)
	{
		++g_audio_calls;
		g_audio_frames += frames;
		g_nonzero_audio_samples += static_cast<std::size_t>(std::count_if(
			data, data + frames * 2, [](std::int16_t sample) { return sample != 0; }));
	}
	return frames;
}

void RETRO_CALLCONV input_poll()
{
	++g_input_polls;
}

std::int16_t RETRO_CALLCONV input_state(unsigned port, unsigned device, unsigned, unsigned id)
{
	if (port == 0 && device == RETRO_DEVICE_JOYPAD && id == RETRO_DEVICE_ID_JOYPAD_B &&
		g_test_frame >= 300 && g_test_frame < 360)
	{
		return 1;
	}

	return 0;
}
} // namespace

int main(int argc, char** argv)
{
	bool passed = true;
	const bool integration_test = argc >= 2;
	if (argc >= 3)
	{
		g_system_directory = argv[2];
	}
	if (argc >= 4)
	{
		g_save_directory = argv[3];
	}
	if (argc >= 5)
	{
		g_dev_hdd0_location = argv[4];
	}

	passed &= check(retro_api_version() == RETRO_API_VERSION, "unexpected libretro API version");

	retro_system_info system_info{};
	retro_get_system_info(&system_info);
	passed &= check(system_info.library_name && std::strcmp(system_info.library_name, "RPCS3") == 0,
		"unexpected library name");
	passed &= check(system_info.need_fullpath, "the core must require full content paths");

	retro_set_environment(environment);
	retro_set_video_refresh(video_refresh);
	retro_set_audio_sample(audio_sample);
	retro_set_audio_sample_batch(audio_sample_batch);
	retro_set_input_poll(input_poll);
	retro_set_input_state(input_state);
	retro_init();

	passed &= check(g_received_input_descriptors, "input descriptors were not registered");

	if (integration_test)
	{
		const retro_game_info game{argv[1], nullptr, 0, nullptr};
		passed &= check(retro_load_game(&game), "real PS3 content did not boot");
		if (!passed)
		{
			std::fflush(stdout);
			std::fflush(stderr);
			return 1;
		}

		const unsigned integration_frames = argc >= 6 ? static_cast<unsigned>(std::strtoul(argv[5], nullptr, 10)) : 1800;
		for (g_test_frame = 0; passed && g_test_frame < integration_frames; ++g_test_frame)
		{
			retro_run();
			std::this_thread::sleep_for(std::chrono::microseconds(16'667));
		}

		passed &= check(g_video_calls != 0, "real video callback was not reached");
		passed &= check(g_audio_calls == integration_frames, "real audio callback count mismatch");
		passed &= check(g_audio_frames == static_cast<std::size_t>(integration_frames) * 800,
			"real audio frame count mismatch");
		// Short path-selection smoke runs can finish while PPU/SPU compilation is
		// still active, before the title opens its audio stream. Keep the full
		// integration test strict, but do not turn a bounded mount test into an
		// audio-playback test.
		if (integration_frames >= 1800)
		{
			passed &= check(g_nonzero_audio_samples != 0, "RPCS3 did not deliver non-silent game audio");
		}
		passed &= check(g_input_polls == integration_frames, "real input poll count mismatch");
		passed &= check(g_message_updates != 0, "progress/start messages were not delivered to the frontend");
		passed &= check(g_progress_bar_frames != 0, "the progress bar never reached the video callback");

		retro_unload_game();
		retro_deinit();

		if (!passed)
		{
			return 1;
		}

		std::fprintf(stdout,
			"PASS: integration delivered %zu audio frames (%zu non-zero samples), %u video frames and %u input polls.\n",
			g_audio_frames, g_nonzero_audio_samples, g_video_calls, g_input_polls);
		return 0;
	}

	for (unsigned frame = 0; frame < 3; ++frame)
	{
		retro_run();
	}

	passed &= check(g_video_calls == 3, "video callback count mismatch");
	passed &= check(g_audio_calls == 3, "audio batch callback count mismatch");
	passed &= check(g_audio_frames == 2400, "audio frame count mismatch");
	passed &= check(g_input_polls == 3, "input poll count mismatch");
	passed &= check(retro_serialize_size() == 0, "savestates must remain disabled in the preview");
	retro_deinit();

	if (!passed)
	{
		return 1;
	}

	std::fprintf(stdout, "PASS: RPCS3 libretro ABI and callbacks are operational.\n");
	return 0;
}
