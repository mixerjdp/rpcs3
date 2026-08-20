#include "Input/pad_thread.h"

#include "LibretroInput.h"

#include "Emu/Io/Null/NullPadHandler.h"
#include "Emu/Io/PadHandler.h"
#include "Emu/Io/pad_config.h"
#include "Utilities/Thread.h"

#include <chrono>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

// These objects normally live in the Qt frontend. The libretro frontend owns
// its controller state directly, but emucore still references the shared input
// configuration objects.
cfg_input_configurations g_cfg_input_configs;
std::string g_input_config_override;

extern void pad_state_notify_state_change(usz index, u32 state);

namespace pad
{
atomic_t<pad_thread*> g_pad_thread = nullptr;
shared_mutex g_pad_mutex;
std::string g_title_id;
atomic_t<bool> g_enabled{true};
atomic_t<bool> g_reset{false};
atomic_t<bool> g_started{false};
atomic_t<bool> g_home_menu_requested{false};
} // namespace pad

namespace
{
struct pending_rumble
{
	std::atomic<std::uint16_t> strong{0};
	std::atomic<std::uint16_t> weak{0};
	std::atomic<bool> changed{false};
};

std::array<pending_rumble, CELL_PAD_MAX_PORT_NUM> g_pending_rumble{};

void initialize_standard_pad(const std::shared_ptr<Pad>& pad, bool connected)
{
	if (!pad)
	{
		return;
	}

	constexpr u32 capabilities = CELL_PAD_CAPABILITY_PS3_CONFORMITY |
		CELL_PAD_CAPABILITY_PRESS_MODE | CELL_PAD_CAPABILITY_HP_ANALOG_STICK |
		CELL_PAD_CAPABILITY_ACTUATOR | CELL_PAD_CAPABILITY_SENSOR_MODE;

	pad->Init(
		connected ? CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES : CELL_PAD_STATUS_DISCONNECTED,
		capabilities,
		CELL_PAD_DEV_TYPE_STANDARD,
		CELL_PAD_PCLASS_TYPE_STANDARD,
		0,
		0,
		0,
		50);

	const auto add_button = [&pad](u32 offset, u32 code)
	{
		pad->m_buttons.emplace_back(offset, std::vector<std::set<u32>>{}, code);
	};

	add_button(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_UP);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_DOWN);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_LEFT);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_RIGHT);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CROSS);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_SQUARE);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CIRCLE);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_TRIANGLE);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L1);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L2);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_L3);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R1);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R2);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_R3);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_START);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_SELECT);
	add_button(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_PS);

	pad->m_sticks[0] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X, {}, {});
	pad->m_sticks[1] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y, {}, {});
	pad->m_sticks[2] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, {}, {});
	pad->m_sticks[3] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y, {}, {});

	pad->m_sensors[0] = AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_X, 0, 0, 0, DEFAULT_MOTION_X);
	pad->m_sensors[1] = AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_Y, 0, 0, 0, DEFAULT_MOTION_Y);
	pad->m_sensors[2] = AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_Z, 0, 0, 0, DEFAULT_MOTION_Z);
	pad->m_sensors[3] = AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_G, 0, 0, 0, DEFAULT_MOTION_G);
	pad->m_vibrate_motors[0] = VibrateMotor(true);
	pad->m_vibrate_motors[1] = VibrateMotor(false);

	pad->m_buttons_external.resize(pad->m_buttons.size());
	for (usz index = 0; index < pad->m_buttons.size(); ++index)
	{
		pad->m_buttons_external[index].m_offset = pad->m_buttons[index].m_offset;
		pad->m_buttons_external[index].m_outKeyCode = pad->m_buttons[index].m_outKeyCode;
	}

	for (usz index = 0; index < pad->m_sticks.size(); ++index)
	{
		pad->m_sticks_external[index].m_offset = pad->m_sticks[index].m_offset;
		pad->m_sticks_external[index].m_value = 128;
	}

	pad->m_buffer_cleared = true;
}

std::uint16_t axis_to_cell_pad(std::int16_t value)
{
	const std::int32_t normalized = static_cast<std::int32_t>(value) + 32768;
	return static_cast<std::uint16_t>((normalized * 255 + 32767) / 65535);
}

bool button_pressed(const rpcs3::libretro::pad_state& state, u32 offset, u32 code)
{
	if (offset == CELL_PAD_BTN_OFFSET_DIGITAL1)
	{
		switch (code)
		{
		case CELL_PAD_CTRL_UP: return state.up;
		case CELL_PAD_CTRL_DOWN: return state.down;
		case CELL_PAD_CTRL_LEFT: return state.left;
		case CELL_PAD_CTRL_RIGHT: return state.right;
		case CELL_PAD_CTRL_L3: return state.l3;
		case CELL_PAD_CTRL_R3: return state.r3;
		case CELL_PAD_CTRL_START: return state.start;
		case CELL_PAD_CTRL_SELECT: return state.select;
		case CELL_PAD_CTRL_PS: return false;
		default: return false;
		}
	}

	if (offset == CELL_PAD_BTN_OFFSET_DIGITAL2)
	{
		switch (code)
		{
		case CELL_PAD_CTRL_CROSS: return state.cross;
		case CELL_PAD_CTRL_SQUARE: return state.square;
		case CELL_PAD_CTRL_CIRCLE: return state.circle;
		case CELL_PAD_CTRL_TRIANGLE: return state.triangle;
		case CELL_PAD_CTRL_L1: return state.l1;
		case CELL_PAD_CTRL_L2: return state.l2;
		case CELL_PAD_CTRL_R1: return state.r1;
		case CELL_PAD_CTRL_R2: return state.r2;
		default: return false;
		}
	}

	return false;
}
} // namespace

pad_thread::pad_thread(void* curthread, void* curwindow, std::string_view title_id)
	: m_curthread(curthread)
	, m_curwindow(curwindow)
{
	pad::g_title_id = title_id;
	pad::g_pad_thread = this;
	pad::g_started = false;
}

pad_thread::~pad_thread()
{
	pad::g_started = false;
	pad::g_pad_thread = nullptr;
}

void pad_thread::Init()
{
	std::lock_guard lock(pad::g_pad_mutex);
	m_info = {1, 0, false};
	m_handlers.clear();
	m_handlers.emplace(pad_handler::null, std::make_shared<NullPadHandler>());

	for (u32 index = 0; index < CELL_PAD_MAX_PORT_NUM; ++index)
	{
		m_pads[index] = std::make_shared<Pad>(
			pad_handler::null,
			index,
			index == 0 ? CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES : CELL_PAD_STATUS_DISCONNECTED,
			CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE | CELL_PAD_CAPABILITY_ACTUATOR,
			CELL_PAD_DEV_TYPE_STANDARD);
		m_pads_connected[index] = index == 0;

		if (index == 0)
		{
			initialize_standard_pad(m_pads[index], true);
		}
	}
}

void pad_thread::operator()()
{
	Init();
	pad::g_started = true;

	while (thread_ctrl::state() != thread_state::aborting)
	{
		thread_ctrl::wait_for(10'000);
	}

	pad::g_started = false;
}

void pad_thread::SetRumble(u32 port, u8 large_motor, u8 small_motor)
{
	if (port >= m_pads.size() || !m_pads[port])
	{
		return;
	}

	m_pads[port]->m_vibrate_motors[0].value = large_motor;
	m_pads[port]->m_vibrate_motors[1].value = small_motor;
	g_pending_rumble[port].strong = static_cast<std::uint16_t>(large_motor) * 257u;
	g_pending_rumble[port].weak = static_cast<std::uint16_t>(small_motor) * 257u;
	g_pending_rumble[port].changed = true;
}

void pad_thread::SetIntercepted(bool intercepted)
{
	if (intercepted)
	{
		m_info.system_info |= CELL_PAD_INFO_INTERCEPTED;
		m_info.ignore_input = true;
	}
	else
	{
		m_info.system_info &= ~CELL_PAD_INFO_INTERCEPTED;
		m_info.ignore_input = false;
	}
}

s32 pad_thread::AddLddPad()
{
	return -1;
}

void pad_thread::UnregisterLddPad(u32)
{
}

void pad_thread::open_home_menu()
{
}

std::shared_ptr<PadHandlerBase> pad_thread::GetHandler(pad_handler)
{
	return std::make_shared<NullPadHandler>();
}

void pad_thread::InitPadConfig(cfg_pad& cfg, pad_handler, std::shared_ptr<PadHandlerBase>& handler)
{
	cfg.restore_defaults();
	if (!handler)
	{
		handler = std::make_shared<NullPadHandler>();
	}
	handler->init_config(&cfg);
}

namespace rpcs3::libretro
{
bool update_pad_state(const pad_state& state)
{
	bool connection_changed = false;
	u32 connection_status = CELL_PAD_STATUS_DISCONNECTED;

	{
		std::lock_guard lock(pad::g_pad_mutex);
		pad_thread* const thread = pad::g_pad_thread.observe();
		if (!thread)
		{
			return false;
		}

		auto& pads = thread->GetPads();
		if (!pads[0])
		{
			return false;
		}

		const auto& target = pads[0];
		const bool was_connected = target->is_connected();
		if (state.connected)
		{
			target->m_port_status |= CELL_PAD_STATUS_CONNECTED;
			connection_status = CELL_PAD_STATUS_CONNECTED;
		}
		else
		{
			target->m_port_status &= ~CELL_PAD_STATUS_CONNECTED;
		}

		connection_changed = was_connected != state.connected;
		if (connection_changed)
		{
			target->m_port_status |= CELL_PAD_STATUS_ASSIGN_CHANGES;
			target->m_buffer_cleared = true;
			thread->GetInfo().now_connect = state.connected ? 1 : 0;
		}

		for (usz index = 0; index < target->m_buttons.size(); ++index)
		{
			Button& button = target->m_buttons[index];
			const bool pressed = state.connected && button_pressed(state, button.m_offset, button.m_outKeyCode);
			button.m_pressed = pressed;
			button.m_value = pressed ? 255 : 0;

			ButtonExternal& external = target->m_buttons_external[index];
			external.m_pressed = button.m_pressed;
			external.m_value = button.m_value;
		}

		const std::array<std::uint16_t, 4> axes{
			axis_to_cell_pad(state.left_x),
			axis_to_cell_pad(state.left_y),
			axis_to_cell_pad(state.right_x),
			axis_to_cell_pad(state.right_y),
		};

		for (usz index = 0; index < axes.size(); ++index)
		{
			target->m_sticks[index].m_value = axes[index];
			target->m_sticks_external[index].m_value = axes[index];
		}
	}

	if (connection_changed)
	{
		::pad_state_notify_state_change(0, connection_status);
	}

	return true;
}

bool take_rumble_state(unsigned port, std::uint16_t& strong, std::uint16_t& weak)
{
	if (port >= g_pending_rumble.size() || !g_pending_rumble[port].changed.exchange(false))
	{
		return false;
	}

	strong = g_pending_rumble[port].strong.load();
	weak = g_pending_rumble[port].weak.load();
	return true;
}
} // namespace rpcs3::libretro
