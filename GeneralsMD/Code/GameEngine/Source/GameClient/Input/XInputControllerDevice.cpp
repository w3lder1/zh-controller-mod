/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// XInputControllerDevice.cpp /////////////////////////////////////////////////////////////////////
// ControllerMod @feature XInput backend for Xbox controllers on Windows.
//
// XInput is loaded at runtime with LoadLibrary instead of linking an SDK import library. That
// keeps the build free of new SDK dependencies (VC6 has no xinput.h) and lets the game start
// normally on a PC without any XInput DLL.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameController.h"
#include "GameClient/ControllerMath.h"

#ifdef _WIN32

namespace
{

// Layout-compatible copies of the XInput structures (see XINPUT_GAMEPAD / XINPUT_STATE).
struct XInputGamepadRaw
{
	WORD  wButtons;
	BYTE  bLeftTrigger;
	BYTE  bRightTrigger;
	SHORT sThumbLX;
	SHORT sThumbLY;
	SHORT sThumbRX;
	SHORT sThumbRY;
};

struct XInputStateRaw
{
	DWORD dwPacketNumber;
	XInputGamepadRaw Gamepad;
};

typedef DWORD (WINAPI *XInputGetStateFunc)(DWORD userIndex, XInputStateRaw *state);

enum
{
	XI_DPAD_UP        = 0x0001,
	XI_DPAD_DOWN      = 0x0002,
	XI_DPAD_LEFT      = 0x0004,
	XI_DPAD_RIGHT     = 0x0008,
	XI_START          = 0x0010,
	XI_BACK           = 0x0020,
	XI_LEFT_THUMB     = 0x0040,
	XI_RIGHT_THUMB    = 0x0080,
	XI_LEFT_SHOULDER  = 0x0100,
	XI_RIGHT_SHOULDER = 0x0200,
	XI_A              = 0x1000,
	XI_B              = 0x2000,
	XI_X              = 0x4000,
	XI_Y              = 0x8000,

	XI_MAX_SLOTS = 4,
	XI_ERROR_SUCCESS = 0,

	// Polling empty slots is slow on some systems, so only rescan occasionally.
	RESCAN_INTERVAL_MS = 1000
};

class XInputControllerDevice : public ControllerDevice
{
public:
	XInputControllerDevice() : m_module(nullptr), m_getState(nullptr), m_slot(-1), m_lastScanMs(0) {}

	virtual ~XInputControllerDevice()
	{
		if (m_module)
			FreeLibrary(m_module);
	}

	virtual Bool init()
	{
		// Newest first. xinput1_4 ships with Windows 8+, xinput9_1_0 with Vista+.
		static const char *const dllNames[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
		for (Int i = 0; i < (Int)(sizeof(dllNames) / sizeof(dllNames[0])) && !m_module; ++i)
		{
			m_module = LoadLibraryA(dllNames[i]);
			if (m_module)
			{
				m_getState = (XInputGetStateFunc)GetProcAddress(m_module, "XInputGetState");
				if (!m_getState)
				{
					FreeLibrary(m_module);
					m_module = nullptr;
				}
				else
				{
					m_dllName = dllNames[i];
				}
			}
		}
		return m_getState != nullptr;
	}

	virtual Bool poll(ControllerState *state)
	{
		state->clear();
		if (!m_getState)
			return FALSE;

		XInputStateRaw raw;

		if (m_slot >= 0)
		{
			memset(&raw, 0, sizeof(raw));
			if (m_getState((DWORD)m_slot, &raw) == XI_ERROR_SUCCESS)
			{
				convert(raw, state);
				return TRUE;
			}
			// The pad in use is gone: report that for one poll, so the game sees a disconnect even
			// when another pad is bound on the next poll. The next poll scans at once.
			m_slot = -1;
			m_lastScanMs = 0;
			return FALSE;
		}

		// Bind the first connected controller, but do not hammer empty slots every frame.
		const UnsignedInt now = timeGetTime();
		if (m_lastScanMs != 0 && now - m_lastScanMs < RESCAN_INTERVAL_MS)
			return FALSE;
		m_lastScanMs = now;

		for (Int slot = 0; slot < XI_MAX_SLOTS; ++slot)
		{
			memset(&raw, 0, sizeof(raw));
			if (m_getState((DWORD)slot, &raw) == XI_ERROR_SUCCESS)
			{
				m_slot = slot;
				convert(raw, state);
				return TRUE;
			}
		}
		return FALSE;
	}

	virtual const char *getBackendName() const { return m_dllName.isEmpty() ? "XInput (unavailable)" : m_dllName.str(); }
	virtual Int getActiveSlot() const { return m_slot; }

private:
	static void convert(const XInputStateRaw &raw, ControllerState *state)
	{
		const XInputGamepadRaw &pad = raw.Gamepad;
		state->connected = TRUE;
		state->leftX = ControllerMath::normalizeAxis(pad.sThumbLX);
		state->leftY = ControllerMath::normalizeAxis(pad.sThumbLY);
		state->rightX = ControllerMath::normalizeAxis(pad.sThumbRX);
		state->rightY = ControllerMath::normalizeAxis(pad.sThumbRY);
		state->leftTrigger = ControllerMath::normalizeTrigger(pad.bLeftTrigger);
		state->rightTrigger = ControllerMath::normalizeTrigger(pad.bRightTrigger);

		UnsignedInt b = 0;
		const WORD w = pad.wButtons;
		if (w & XI_A)              b |= ControllerState::BUTTON_A;
		if (w & XI_B)              b |= ControllerState::BUTTON_B;
		if (w & XI_X)              b |= ControllerState::BUTTON_X;
		if (w & XI_Y)              b |= ControllerState::BUTTON_Y;
		if (w & XI_LEFT_SHOULDER)  b |= ControllerState::BUTTON_LB;
		if (w & XI_RIGHT_SHOULDER) b |= ControllerState::BUTTON_RB;
		if (w & XI_BACK)           b |= ControllerState::BUTTON_VIEW;
		if (w & XI_START)          b |= ControllerState::BUTTON_MENU;
		if (w & XI_LEFT_THUMB)     b |= ControllerState::BUTTON_LS;
		if (w & XI_RIGHT_THUMB)    b |= ControllerState::BUTTON_RS;
		if (w & XI_DPAD_UP)        b |= ControllerState::BUTTON_DPAD_UP;
		if (w & XI_DPAD_DOWN)      b |= ControllerState::BUTTON_DPAD_DOWN;
		if (w & XI_DPAD_LEFT)      b |= ControllerState::BUTTON_DPAD_LEFT;
		if (w & XI_DPAD_RIGHT)     b |= ControllerState::BUTTON_DPAD_RIGHT;
		state->buttons = b;
	}

	HMODULE m_module;
	XInputGetStateFunc m_getState;
	AsciiString m_dllName;
	Int m_slot;
	UnsignedInt m_lastScanMs;
};

} // namespace

ControllerDevice *createXInputControllerDevice()
{
	return NEW XInputControllerDevice;
}

#else // _WIN32

ControllerDevice *createXInputControllerDevice()
{
	return nullptr;
}

#endif // _WIN32
