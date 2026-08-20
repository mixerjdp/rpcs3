#include "LibretroCoreState.h"

#include <cstring>

namespace
{
constexpr const char* core_name = "RPCS3 (Audio/Input Preview)";
constexpr const char* core_version = "0.3.0-audio-input-preview";
constexpr const char* supported_extensions = "elf|self|bin|iso|m3u|ps3";
}

extern "C"
{
RETRO_API unsigned RETRO_CALLCONV retro_api_version()
{
	return RETRO_API_VERSION;
}

RETRO_API void RETRO_CALLCONV retro_set_environment(retro_environment_t callback)
{
	rpcs3::libretro::get_core_state().set_environment(callback);
}

RETRO_API void RETRO_CALLCONV retro_set_video_refresh(retro_video_refresh_t callback)
{
	rpcs3::libretro::get_core_state().set_video_refresh(callback);
}

RETRO_API void RETRO_CALLCONV retro_set_audio_sample(retro_audio_sample_t callback)
{
	rpcs3::libretro::get_core_state().set_audio_sample(callback);
}

RETRO_API void RETRO_CALLCONV retro_set_audio_sample_batch(retro_audio_sample_batch_t callback)
{
	rpcs3::libretro::get_core_state().set_audio_sample_batch(callback);
}

RETRO_API void RETRO_CALLCONV retro_set_input_poll(retro_input_poll_t callback)
{
	rpcs3::libretro::get_core_state().set_input_poll(callback);
}

RETRO_API void RETRO_CALLCONV retro_set_input_state(retro_input_state_t callback)
{
	rpcs3::libretro::get_core_state().set_input_state(callback);
}

RETRO_API void RETRO_CALLCONV retro_init()
{
	rpcs3::libretro::get_core_state().init();
}

RETRO_API void RETRO_CALLCONV retro_deinit()
{
	rpcs3::libretro::get_core_state().deinit();
}

RETRO_API void RETRO_CALLCONV retro_get_system_info(retro_system_info* info)
{
	if (!info)
	{
		return;
	}

	std::memset(info, 0, sizeof(*info));
	info->library_name = core_name;
	info->library_version = core_version;
	info->valid_extensions = supported_extensions;
	info->need_fullpath = true;
	info->block_extract = true;
}

RETRO_API void RETRO_CALLCONV retro_get_system_av_info(retro_system_av_info* info)
{
	if (!info)
	{
		return;
	}

	std::memset(info, 0, sizeof(*info));
	info->geometry.base_width = rpcs3::libretro::core_state::video_width;
	info->geometry.base_height = rpcs3::libretro::core_state::video_height;
	info->geometry.max_width = 4096;
	info->geometry.max_height = 4096;
	info->geometry.aspect_ratio = 16.0f / 9.0f;
	info->timing.fps = rpcs3::libretro::core_state::frames_per_second;
	info->timing.sample_rate = rpcs3::libretro::core_state::audio_sample_rate;
}

RETRO_API void RETRO_CALLCONV retro_set_controller_port_device(unsigned port, unsigned device)
{
	rpcs3::libretro::get_core_state().set_controller_port_device(port, device);
}

RETRO_API void RETRO_CALLCONV retro_reset()
{
	rpcs3::libretro::get_core_state().reset();
}

RETRO_API void RETRO_CALLCONV retro_run()
{
	rpcs3::libretro::get_core_state().run();
}

RETRO_API size_t RETRO_CALLCONV retro_serialize_size()
{
	return 0;
}

RETRO_API bool RETRO_CALLCONV retro_serialize(void*, size_t)
{
	return false;
}

RETRO_API bool RETRO_CALLCONV retro_unserialize(const void*, size_t)
{
	return false;
}

RETRO_API void RETRO_CALLCONV retro_cheat_reset()
{
}

RETRO_API void RETRO_CALLCONV retro_cheat_set(unsigned, bool, const char*)
{
}

RETRO_API bool RETRO_CALLCONV retro_load_game(const retro_game_info* game)
{
	return rpcs3::libretro::get_core_state().load_game(game);
}

RETRO_API bool RETRO_CALLCONV retro_load_game_special(unsigned, const retro_game_info*, size_t)
{
	return false;
}

RETRO_API void RETRO_CALLCONV retro_unload_game()
{
	rpcs3::libretro::get_core_state().unload_game();
}

RETRO_API unsigned RETRO_CALLCONV retro_get_region()
{
	return RETRO_REGION_NTSC;
}

RETRO_API void* RETRO_CALLCONV retro_get_memory_data(unsigned)
{
	return nullptr;
}

RETRO_API size_t RETRO_CALLCONV retro_get_memory_size(unsigned)
{
	return 0;
}
} // extern "C"
