#include "LibretroEmulator.h"

#include "Emu/System.h"
#include "Emu/IdManager.h"
#include "Emu/localized_string_id.h"
#include "Emu/system_config.h"
#include "Emu/vfs_config.h"
#include "Emu/Audio/AudioBackend.h"
#include "Emu/Audio/Null/null_enumerator.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNp.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/VK/VKGSRender.h"
#include "Input/pad_thread.h"
#include "Utilities/Thread.h"
#include "util/video_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
using namespace std::chrono_literals;

std::string libretro_localized_string(localized_string_id id, const char*)
{
	switch (id)
	{
	case localized_string_id::PROGRESS_DIALOG_PROGRESS: return "Progress:";
	case localized_string_id::PROGRESS_DIALOG_PROGRESS_ANALYZING: return "Analyzing...";
	case localized_string_id::PROGRESS_DIALOG_REMAINING: return "remaining";
	case localized_string_id::PROGRESS_DIALOG_DONE: return "done";
	case localized_string_id::PROGRESS_DIALOG_FILE: return "file";
	case localized_string_id::PROGRESS_DIALOG_MODULE: return "module";
	case localized_string_id::PROGRESS_DIALOG_OF: return "of";
	case localized_string_id::PROGRESS_DIALOG_PLEASE_WAIT: return "Please wait...";
	case localized_string_id::PROGRESS_DIALOG_STOPPING_PLEASE_WAIT: return "Stopping, please wait...";
	case localized_string_id::PROGRESS_DIALOG_SAVESTATE_PLEASE_WAIT: return "Saving state, please wait...";
	case localized_string_id::PROGRESS_DIALOG_SCANNING_PPU_EXECUTABLE: return "Scanning PPU Executable...";
	case localized_string_id::PROGRESS_DIALOG_ANALYZING_PPU_EXECUTABLE: return "Analyzing PPU Executable...";
	case localized_string_id::PROGRESS_DIALOG_SCANNING_PPU_MODULES: return "Scanning PPU Modules...";
	case localized_string_id::PROGRESS_DIALOG_LOADING_PPU_MODULES: return "Loading PPU Modules...";
	case localized_string_id::PROGRESS_DIALOG_COMPILING_PPU_MODULES: return "Compiling PPU Modules...";
	case localized_string_id::PROGRESS_DIALOG_LINKING_PPU_MODULES: return "Linking PPU Modules...";
	case localized_string_id::PROGRESS_DIALOG_APPLYING_PPU_CODE: return "Applying PPU Code...";
	case localized_string_id::PROGRESS_DIALOG_BUILDING_SPU_CACHE: return "Building SPU Cache...";
	default: return {};
	}
}

bool libretro_frame_readback_disabled()
{
	const char* value = std::getenv("RPCS3_LIBRETRO_DISABLE_FRAME_READBACK");
	if (!value)
	{
		return false;
	}

	const std::string_view setting(value);
	return setting == "1" || setting == "true" || setting == "TRUE" || setting == "yes" || setting == "on";
}

class libretro_audio_backend final : public AudioBackend
{
public:
	std::string_view GetName() const override
	{
		return std::string_view{"Libretro"};
	}

	bool Open(std::string_view, AudioFreq freq, AudioSampleSize sample_size,
		AudioChannelCnt, audio_channel_layout) override
	{
		Close();
		std::lock_guard lock(m_cb_mutex);
		m_sampling_rate = freq;
		m_sample_size = sample_size;
		m_channels = 2;
		m_layout = audio_channel_layout::stereo;
		m_open = true;
		return true;
	}

	void Close() override
	{
		std::lock_guard lock(m_cb_mutex);
		m_open = false;
		m_playing = false;
		m_write_callback = {};
	}

	f64 GetCallbackFrameLen() override
	{
		return 1.0 / 60.0;
	}

	void Play() override
	{
		std::lock_guard lock(m_cb_mutex);
		m_playing = m_open;
	}

	void Pause() override
	{
		std::lock_guard lock(m_cb_mutex);
		m_playing = false;
	}

	bool IsPlaying() override
	{
		std::lock_guard lock(m_cb_mutex);
		return m_open && m_playing;
	}

	rpcs3::libretro::audio_pull_state pull(std::span<std::int16_t> interleaved_stereo)
	{
		std::fill(interleaved_stereo.begin(), interleaved_stereo.end(), 0);
		if (interleaved_stereo.empty())
		{
			return rpcs3::libretro::audio_pull_state::empty;
		}

		std::lock_guard lock(m_cb_mutex);
		if (!m_open)
		{
			return rpcs3::libretro::audio_pull_state::closed;
		}
		if (!m_write_callback)
		{
			return rpcs3::libretro::audio_pull_state::missing_callback;
		}
		// RPCS3's audio ring buffer becomes active on the device's first write
		// callback. Permit this priming pull before Play() or producer and
		// consumer would each wait for the other to start.
		const bool playing = m_playing;

		if (m_sample_size == AudioSampleSize::S16)
		{
			const u32 requested_bytes = static_cast<u32>(interleaved_stereo.size_bytes());
			const u32 written_bytes = std::min(m_write_callback(requested_bytes, interleaved_stereo.data()), requested_bytes);
			const usz written_samples = written_bytes / sizeof(std::int16_t);
			std::fill(interleaved_stereo.begin() + written_samples, interleaved_stereo.end(), 0);
			return written_samples != 0 ? rpcs3::libretro::audio_pull_state::data :
				(playing ? rpcs3::libretro::audio_pull_state::empty : rpcs3::libretro::audio_pull_state::paused);
		}

		m_float_buffer.assign(interleaved_stereo.size(), 0.0f);
		const u32 requested_bytes = static_cast<u32>(m_float_buffer.size() * sizeof(float));
		const u32 written_bytes = std::min(m_write_callback(requested_bytes, m_float_buffer.data()), requested_bytes);
		const u32 written_samples = written_bytes / sizeof(float);
		AudioBackend::convert_to_s16(written_samples, m_float_buffer.data(), interleaved_stereo.data());
		return written_samples != 0 ? rpcs3::libretro::audio_pull_state::data :
			(playing ? rpcs3::libretro::audio_pull_state::empty : rpcs3::libretro::audio_pull_state::paused);
	}

private:
	bool m_open = false;
	std::vector<float> m_float_buffer;
};

class frame_mailbox final
{
public:
	void submit(std::vector<u8>&& source, u32 pitch, u32 width, u32 height, bool is_bgra)
	{
		if (!width || !height || pitch < width * 4 || source.size() < static_cast<usz>(pitch) * height)
		{
			return;
		}

		std::vector<std::uint32_t> converted(static_cast<usz>(width) * height);

		for (u32 y = 0; y < height; ++y)
		{
			const u8* src = source.data() + static_cast<usz>(y) * pitch;
			auto* dst = converted.data() + static_cast<usz>(y) * width;

			if (is_bgra)
			{
				std::memcpy(dst, src, static_cast<usz>(width) * 4);
				continue;
			}

			for (u32 x = 0; x < width; ++x)
			{
				const u8 red = src[x * 4 + 0];
				const u8 green = src[x * 4 + 1];
				const u8 blue = src[x * 4 + 2];
				dst[x] = (static_cast<u32>(red) << 16) |
					(static_cast<u32>(green) << 8) | blue;
			}
		}

		std::lock_guard lock(m_mutex);
		m_pending.pixels = std::move(converted);
		m_pending.width = width;
		m_pending.height = height;
		m_has_pending = true;
	}

	bool take(rpcs3::libretro::video_frame& destination)
	{
		std::lock_guard lock(m_mutex);
		if (!m_has_pending)
		{
			return false;
		}

		destination = std::move(m_pending);
		m_pending = {};
		m_has_pending = false;
		return true;
	}

	void clear()
	{
		std::lock_guard lock(m_mutex);
		m_pending = {};
		m_has_pending = false;
	}

private:
	std::mutex m_mutex;
	rpcs3::libretro::video_frame m_pending;
	bool m_has_pending = false;
};

#ifdef _WIN32
class hidden_render_window final
{
public:
	bool create()
	{
		if (m_handle)
		{
			return true;
		}

		m_instance = GetModuleHandleW(nullptr);
		WNDCLASSEXW window_class{};
		window_class.cbSize = sizeof(window_class);
		window_class.hInstance = m_instance;
		window_class.lpfnWndProc = DefWindowProcW;
		window_class.lpszClassName = class_name;

		m_class_atom = RegisterClassExW(&window_class);
		if (!m_class_atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		{
			return false;
		}

		m_handle = CreateWindowExW(
			WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
			class_name,
			L"RPCS3 libretro render surface",
			WS_POPUP,
			0,
			0,
			1280,
			720,
			nullptr,
			nullptr,
			m_instance,
			nullptr);

		return m_handle != nullptr;
	}

	~hidden_render_window()
	{
		if (m_handle)
		{
			DestroyWindow(m_handle);
		}

		if (m_class_atom)
		{
			UnregisterClassW(class_name, m_instance);
		}
	}

	HWND handle() const
	{
		return m_handle;
	}

private:
	static constexpr const wchar_t* class_name = L"RPCS3LibretroHiddenSurface";
	HINSTANCE m_instance = nullptr;
	ATOM m_class_atom = 0;
	HWND m_handle = nullptr;
};
#endif

class libretro_gs_frame final : public GSFrameBase
{
public:
#ifdef _WIN32
	libretro_gs_frame(std::shared_ptr<frame_mailbox> mailbox, std::shared_ptr<std::atomic<bool>> readback_gate, HWND window)
		: m_mailbox(std::move(mailbox))
		, m_readback_gate(std::move(readback_gate))
		, m_window(window)
	{
		m_frame_readback_enabled = !libretro_frame_readback_disabled();
	}
#else
	explicit libretro_gs_frame(std::shared_ptr<frame_mailbox> mailbox, std::shared_ptr<std::atomic<bool>> readback_gate)
		: m_mailbox(std::move(mailbox))
		, m_readback_gate(std::move(readback_gate))
	{
		m_frame_readback_enabled = !libretro_frame_readback_disabled();
	}
#endif

	void close() override { m_open = false; }
	void reset() override { m_open = true; }
	bool shown() override { return m_open; }
	void hide() override {}
	void show() override { m_open = true; }
	void toggle_fullscreen() override {}
	void delete_context(draw_context_t) override {}
	draw_context_t make_context() override { return nullptr; }
	void set_current(draw_context_t) override {}
	void flip(draw_context_t, bool) override {}
	int client_width() override { return 1280; }
	int client_height() override { return 720; }
	f64 client_display_rate() override { return 60.0; }
	bool has_alpha() override { return false; }

	display_handle_t handle() const override
	{
#ifdef _WIN32
		return m_window;
#else
		return {};
#endif
	}

	bool can_consume_frame() const override
	{
		return m_open && m_frame_readback_enabled && (!m_readback_gate || m_readback_gate->load());
	}

	void present_frame(std::vector<u8>&& data, u32 pitch, u32 width, u32 height, bool is_bgra) const override
	{
		m_mailbox->submit(std::move(data), pitch, width, height, is_bgra);
	}

	void take_screenshot(std::vector<u8>&& data, u32 width, u32 height, bool is_bgra) override
	{
		m_mailbox->submit(std::move(data), width * 4, width, height, is_bgra);
	}

	void update_title(double) override {}

private:
	std::shared_ptr<frame_mailbox> m_mailbox;
	std::shared_ptr<std::atomic<bool>> m_readback_gate;
	std::atomic<bool> m_open{true};
	bool m_frame_readback_enabled = true;
#ifdef _WIN32
	HWND m_window = nullptr;
#endif
};
} // namespace

// System.cpp deliberately delegates waits to the frontend. Libretro has no Qt
// event loop, so use the same non-GUI wait behavior as RPCS3 headless mode.
void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op)
{
	while (!wrapped_op())
	{
		if (repeat_duration_ms == 0)
		{
			std::this_thread::yield();
		}
		else if (thread_ctrl::get_current())
		{
			thread_ctrl::wait_for(static_cast<u64>(repeat_duration_ms) * 1000);
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(repeat_duration_ms));
		}
	}
}

[[noreturn]] void report_fatal_error(std::string_view text, bool, bool)
{
	OutputDebugStringA("RPCS3 libretro fatal error: ");
	OutputDebugStringA(std::string{text}.c_str());
	OutputDebugStringA("\n");
	std::abort();
}

namespace rpcs3::libretro
{
class emulator_bridge::impl final
{
public:
	bool initialize(const std::string& system_directory, const std::string& save_directory, std::string& error)
	{
		if (m_initialized)
		{
			return true;
		}

		if (system_directory.empty())
		{
			error = "RetroArch did not provide a system directory for RPCS3.";
			return false;
		}

		std::filesystem::path system_path = std::filesystem::u8path(system_directory);
		if (system_path.filename().string() != "rpcs3" && system_path.filename().string() != "RPCS3")
		{
			system_path /= "rpcs3";
		}
		const std::filesystem::path save_path = std::filesystem::u8path(save_directory.empty() ? system_directory : save_directory) / "rpcs3";
		std::error_code filesystem_error;
		std::filesystem::create_directories(system_path, filesystem_error);
		if (filesystem_error || !std::filesystem::is_directory(system_path))
		{
			error = "Could not create the RPCS3 system directory: " + system_path.string();
			return false;
		}
		filesystem_error.clear();
		std::filesystem::create_directories(save_path / "dev_hdd0", filesystem_error);
		if (filesystem_error || !std::filesystem::is_directory(save_path))
		{
			error = "Could not create the RPCS3 save directory: " + save_path.string();
			return false;
		}

#ifdef _WIN32
		// The standalone executable calls WSAStartup during process setup. A
		// libretro core does not run that entry point, and RetroArch is not
		// required to initialize Winsock for a core. Without this, a PS3 title
		// that opens a network socket can get WSAENOTINITIALISED (10093), which
		// RPCS3 treats as an unexpected host error and terminates the guest PPU
		// thread. Keep this reference for the whole loaded-core lifetime.
		if (!initialize_winsock(error))
		{
			return false;
		}

		std::wstring data_directory_w = system_path.wstring();
		// fs::get_config_dir() treats RPCS3_CONFIG_DIR like an executable path
		// and keeps everything through its final separator. Preserve the actual
		// directory by making that separator explicit.
		if (!data_directory_w.ends_with(L'\\') && !data_directory_w.ends_with(L'/'))
		{
			data_directory_w.push_back(L'\\');
		}
		if (!SetEnvironmentVariableW(L"RPCS3_CONFIG_DIR", data_directory_w.c_str()))
		{
			error = "Could not set RPCS3_CONFIG_DIR.";
			return false;
		}

		set_libretro_vfs_paths(system_path.string(), save_path.string());

		if (!m_window.create())
		{
			cleanup_winsock();
			error = "Could not create the hidden Win32 Vulkan surface.";
			return false;
		}
#else
		error = "The initial real-video bridge is currently implemented for Windows only.";
		return false;
#endif

		m_mailbox = std::make_shared<frame_mailbox>();
		m_audio_backend = std::make_shared<libretro_audio_backend>();
		m_main_thread_id = std::this_thread::get_id();
		install_callbacks();

		try
		{
			// Libretro cannot rely on RPCS3's process-start static registration:
			// the frontend may unload this DLL while worker threads still exist.
			// Register the handlers for the emulation lifetime and remove them in
			// stop() after all emulator callbacks have drained.
			thread_ctrl::initialize_exception_handler();
			Emu.SetHasGui(false);
			// RPCS3's headless flag intentionally forces the Null renderer. This is
			// a non-Qt frontend, but it still owns a real hidden render surface.
			Emu.SetHeadless(false);
			Emu.SetUsr("00000001");
			Emu.SetSupportedRenderers({video_renderer::null, video_renderer::vulkan});
			Emu.SetDefaultRenderer(video_renderer::vulkan);
			// Init requires a non-empty Vulkan adapter. The loaded RPCS3 config can
			// replace this with its persisted adapter immediately afterward.
			Emu.SetDefaultGraphicsAdapter("NVIDIA GeForce RTX 2060");
			Emu.Init();
			m_initialized = true;
			return true;
		}
		catch (const std::exception& exception)
		{
			error = std::string{"RPCS3 initialization failed: "} + exception.what();
		}
		catch (...)
		{
			error = "RPCS3 initialization failed with an unknown exception.";
		}

#ifdef _WIN32
		cleanup_winsock();
#endif

		return false;
	}

	bool boot(const std::string& content_path, std::string& error)
	{
		if (!m_initialized)
		{
			error = "RPCS3 was not initialized.";
			return false;
		}

		if (content_path.empty())
		{
			error = "A full PS3 content path is required.";
			return false;
		}

		stop();
		m_readback_gate->store(true);
		thread_ctrl::initialize_exception_handler();
		m_mailbox->clear();
		Emu.SetForceBoot(true);
		g_cfg.video.resolution_scale_percent.set(m_resolution_scale_percent);

		try
		{
			const game_boot_result result = Emu.BootGame(content_path);
			if (result != game_boot_result::no_errors)
			{
				error = "RPCS3 rejected the content (game_boot_result=" +
					std::to_string(static_cast<u32>(result)) + ").";
				return false;
			}

			// A title-specific RPCS3 configuration may be applied during BootGame.
			// Re-apply the libretro option so the frontend's explicit choice wins
			// for this core session. VKPresent synchronizes the change on the next
			// flip.
			g_cfg.video.resolution_scale_percent.set(m_resolution_scale_percent);
		}
		catch (const std::exception& exception)
		{
			error = std::string{"RPCS3 boot failed: "} + exception.what();
			return false;
		}
		catch (...)
		{
			error = "RPCS3 boot failed with an unknown exception.";
			return false;
		}

		return true;
	}

	void set_resolution_scale(unsigned percent)
	{
		m_resolution_scale_percent = std::clamp(percent, 25u, 800u);
		if (m_initialized)
		{
			g_cfg.video.resolution_scale_percent.set(m_resolution_scale_percent);
		}
	}

	void stop()
	{
		if (!m_initialized)
		{
			return;
		}

		// Prevent a new Vulkan readback from being queued while the RSX renderer
		// and its command buffers are being stopped for close-content.
		m_readback_gate->store(false);

		if (!Emu.IsStopped(true))
		{
			Emu.Kill(false);

			// Emulator::Kill completes asynchronously. Its final cleanup is posted
			// through call_from_main_thread and owns the join-thread object, so the
			// libretro thread must dispatch that callback before checking the final
			// stopped state. Executing it inline on the worker would let FreeLibrary
			// unmap this core while the worker was still returning through its code.
			while (!Emu.IsStopped(true))
			{
				pump_main_thread_callbacks();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		pump_main_thread_callbacks();
		thread_ctrl::cleanup_exception_handler();
	}

	bool restart(std::string& error)
	{
		if (!m_initialized || Emu.IsStopped())
		{
			error = "No running PS3 title is available to reset.";
			return false;
		}

		g_cfg.video.resolution_scale_percent.set(m_resolution_scale_percent);
		const game_boot_result result = Emu.Restart(false, true);
		if (result != game_boot_result::no_errors)
		{
			error = "RPCS3 reset failed (game_boot_result=" +
				std::to_string(static_cast<u32>(result)) + ").";
			return false;
		}

		g_cfg.video.resolution_scale_percent.set(m_resolution_scale_percent);

		return true;
	}

	bool take_frame(video_frame& frame)
	{
		pump_main_thread_callbacks();
		return m_mailbox && m_mailbox->take(frame);
	}

	audio_pull_state take_audio(std::span<std::int16_t> interleaved_stereo)
	{
		return m_audio_backend ? m_audio_backend->pull(interleaved_stereo) : audio_pull_state::closed;
	}

	bool is_running() const
	{
		return m_initialized && !Emu.IsStopped();
	}

	~impl()
	{
		stop();
		if (m_initialized)
		{
			Emulator::CleanUp();
		}
		clear_libretro_vfs_paths();

#ifdef _WIN32
		cleanup_winsock();
#endif

		// RPCS3 standalone keeps its process-wide exception hooks until process
		// exit. A libretro core is unloaded with FreeLibrary, so leaving callbacks
		// into this DLL registered would trap the loader in unloaded code.
		thread_ctrl::cleanup_exception_handler();
	}

private:
#ifdef _WIN32
	bool initialize_winsock(std::string& error)
	{
		if (m_winsock_initialized)
		{
			return true;
		}

		WSADATA wsa_data{};
		const int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
		if (result != 0)
		{
			error = "Could not initialize Winsock for the RPCS3 core (error=" + std::to_string(result) + ").";
			return false;
		}

		m_winsock_initialized = true;
		return true;
	}

	void cleanup_winsock()
	{
		if (m_winsock_initialized)
		{
			WSACleanup();
			m_winsock_initialized = false;
		}
	}
#endif

	struct pending_main_thread_call
	{
		std::function<void()> function;
		atomic_t<u32>* wake_up = nullptr;
	};

	void dispatch_main_thread_call(std::function<void()> function, atomic_t<u32>* wake_up)
	{
		if (std::this_thread::get_id() == m_main_thread_id)
		{
			function();
			if (wake_up)
			{
				*wake_up = true;
				wake_up->notify_one();
			}
			return;
		}

		std::lock_guard lock(m_main_thread_mutex);
		m_main_thread_calls.push_back({std::move(function), wake_up});
	}

	void pump_main_thread_callbacks()
	{
		ensure(std::this_thread::get_id() == m_main_thread_id);

		for (;;)
		{
			std::deque<pending_main_thread_call> calls;
			{
				std::lock_guard lock(m_main_thread_mutex);
				if (m_main_thread_calls.empty())
				{
					break;
				}
				calls.swap(m_main_thread_calls);
			}

			for (auto& call : calls)
			{
				call.function();
				if (call.wake_up)
				{
					*call.wake_up = true;
					call.wake_up->notify_one();
				}
			}
		}
	}

	void install_callbacks()
	{
		EmuCallbacks callbacks{};
		callbacks.call_from_main_thread = [this](std::function<void()> function, atomic_t<u32>* wake_up)
		{
			dispatch_main_thread_call(std::move(function), wake_up);
		};

		callbacks.try_to_quit = [](bool force_quit, std::function<void()> on_exit)
		{
			if (!force_quit)
			{
				return false;
			}
			if (on_exit)
			{
				on_exit();
			}
			return true;
		};

		callbacks.on_run = [](bool) {};
		callbacks.on_pause = [] {};
		callbacks.on_resume = [] {};
		callbacks.on_stop = [] {};
		callbacks.on_ready = [] {};
		callbacks.on_missing_fw = [] {};
		callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>>, int) {};
		callbacks.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};
		callbacks.enable_disc_eject = [](bool) {};
		callbacks.enable_disc_insert = [](bool) {};
		callbacks.handle_taskbar_progress = [](s32, s32) {};

		callbacks.init_kb_handler = []
		{
			ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager()));
		};
		callbacks.init_mouse_handler = []
		{
			ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager()));
		};
		callbacks.init_pad_handler = [](std::string_view title_id)
		{
			ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, title_id));
			qt_events_aware_op(0, [] { return !!pad::g_started; });
		};
		callbacks.update_emu_settings = [] {};
		callbacks.save_emu_settings = []
		{
			Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID());
		};

		callbacks.close_gs_frame = [] {};
		callbacks.get_gs_frame = [this]() -> std::unique_ptr<GSFrameBase>
		{
#ifdef _WIN32
			return std::make_unique<libretro_gs_frame>(m_mailbox, m_readback_gate, m_window.handle());
#else
			return std::make_unique<libretro_gs_frame>(m_mailbox, m_readback_gate);
#endif
		};
		callbacks.init_gs_render = [](utils::serial* archive)
		{
			if (g_cfg.video.renderer == video_renderer::vulkan)
			{
				g_fxo->init<rsx::thread, named_thread<VKGSRender>>(archive);
			}
			else
			{
				g_fxo->init<rsx::thread, named_thread<NullGSRender>>(archive);
			}
		};

		callbacks.get_audio = [this] { return m_audio_backend; };
		callbacks.get_audio_enumerator = [](u64) { return std::make_shared<null_enumerator>(); };
		callbacks.get_camera_handler = [] { return std::make_shared<null_camera_handler>(); };
		callbacks.get_music_handler = [] { return std::make_shared<null_music_handler>(); };
		callbacks.get_msg_dialog = [] { return std::shared_ptr<MsgDialogBase>{}; };
		callbacks.get_osk_dialog = [] { return std::shared_ptr<OskDialogBase>{}; };
		callbacks.get_save_dialog = [] { return std::unique_ptr<SaveDialogBase>{}; };
		callbacks.get_sendmessage_dialog = [] { return std::shared_ptr<SendMessageDialogBase>{}; };
		callbacks.get_recvmessage_dialog = [] { return std::shared_ptr<RecvMessageDialogBase>{}; };
		callbacks.get_trophy_notification_dialog = [] { return std::unique_ptr<TrophyNotificationBase>{}; };

		callbacks.get_localized_string = [](localized_string_id id, const char* args)
		{
			return libretro_localized_string(id, args);
		};
		callbacks.get_localized_u32string = [](localized_string_id, const char*) { return std::u32string{}; };
		callbacks.get_localized_setting = [](const cfg::_base*, u32) { return std::string{}; };
		callbacks.get_photo_path = [](std::string_view path) { return std::string{path}; };
		callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
		callbacks.get_image_info = [](const std::string&, std::string& subtype, s32& width, s32& height, s32& orientation)
		{
			subtype.clear();
			width = height = orientation = 0;
			return false;
		};
		callbacks.get_scaled_image = [](const std::string&, s32, s32, s32& width, s32& height, u8*, bool)
		{
			width = height = 0;
			return false;
		};
		callbacks.get_font_dirs = [] { return std::vector<std::string>{}; };
		callbacks.on_install_pkgs = [](const std::vector<std::string>&) { return false; };
		callbacks.add_breakpoint = [](u32) {};
		callbacks.display_sleep_control_supported = [] { return false; };
		callbacks.enable_display_sleep = [](bool) {};
		callbacks.check_microphone_permissions = [] {};
		callbacks.make_video_source = [] { return std::unique_ptr<video_source>{}; };
		callbacks.enable_gamemode = [](bool) {};
		callbacks.get_database_config = [](const std::string&) { return std::string{}; };

		Emu.SetCallbacks(std::move(callbacks));
	}

	bool m_initialized = false;
	unsigned m_resolution_scale_percent = 100;
	std::shared_ptr<frame_mailbox> m_mailbox;
	std::shared_ptr<libretro_audio_backend> m_audio_backend;
	std::shared_ptr<std::atomic<bool>> m_readback_gate = std::make_shared<std::atomic<bool>>(true);
	std::thread::id m_main_thread_id;
	std::mutex m_main_thread_mutex;
	std::deque<pending_main_thread_call> m_main_thread_calls;
#ifdef _WIN32
	hidden_render_window m_window;
	bool m_winsock_initialized = false;
#endif
};

emulator_bridge::emulator_bridge()
	: m_impl(std::make_unique<impl>())
{
}

emulator_bridge::~emulator_bridge() = default;

bool emulator_bridge::initialize(const std::string& system_directory, const std::string& save_directory, std::string& error)
{
	return m_impl->initialize(system_directory, save_directory, error);
}

void emulator_bridge::set_resolution_scale(unsigned percent)
{
	m_impl->set_resolution_scale(percent);
}

bool emulator_bridge::boot(const std::string& content_path, std::string& error)
{
	return m_impl->boot(content_path, error);
}

void emulator_bridge::stop()
{
	m_impl->stop();
}

bool emulator_bridge::restart(std::string& error)
{
	return m_impl->restart(error);
}

bool emulator_bridge::take_frame(video_frame& frame)
{
	return m_impl->take_frame(frame);
}

audio_pull_state emulator_bridge::take_audio(std::span<std::int16_t> interleaved_stereo)
{
	return m_impl->take_audio(interleaved_stereo);
}

bool emulator_bridge::is_running() const
{
	return m_impl->is_running();
}
} // namespace rpcs3::libretro
