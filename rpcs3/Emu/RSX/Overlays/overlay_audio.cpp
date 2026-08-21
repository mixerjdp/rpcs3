#include "stdafx.h"
#include "overlay_audio.h"
#include "Emu/System.h"

namespace rsx
{
	namespace overlays
	{
		audio_player::audio_player(const std::string& audio_path)
		{
			init_audio(audio_path);
		}

		void audio_player::init_audio(const std::string& audio_path)
		{
			if (audio_path.empty())
			{
				return;
			}

			// Some frontends (including the libretro core) do not provide a
			// video/audio source. Boot music is optional in that case; do not
			// turn a missing frontend capability into a process-wide abort.
			if (!Emu.GetCallbacks().make_video_source)
			{
				rsx_log.warning("Skipping boot audio because no video source is available.");
				return;
			}

			m_video_source = Emu.GetCallbacks().make_video_source();
			if (!m_video_source)
			{
				rsx_log.warning("Skipping boot audio because the frontend did not create a video source.");
				return;
			}

			m_video_source->set_audio_path(audio_path);
		}

		void audio_player::set_active(bool active)
		{
			if (m_video_source)
			{
				m_video_source->set_active(active);
			}
		}
	}
}
