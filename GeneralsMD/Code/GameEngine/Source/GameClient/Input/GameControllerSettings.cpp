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

// GameControllerSettings.cpp /////////////////////////////////////////////////////////////////////
// ControllerMod @feature The controller's own settings screen and button layout.
//
// The screen is drawn by the controller (no game files are added) and opens with View in the
// menus, or with Y from the in-match help. Every change applies at once; closing saves
// ControllerMod.ini.
//
//   Up / Down            choose a setting          Left / Right   change it
//   A                    switch on/off; on a button: press the new button for that function
//   LB / RB              previous / next page      Y (twice)      this page back to its defaults
//   X                    undo every change made since the screen opened
//   B, Menu or View      save and close
//
// Button layout: A, B, X, Y, LB and RB can be swapped around (ControllerMath::assignButton, a
// swap, so every function always keeps exactly one button). The pad is read through the layout
// (remapButtons) before anything else sees it, and the controller's own texts name the physical
// button (buttonNamesFor). If the layout ever gets lost, holding both stick clicks for three
// seconds, or Ctrl+Shift+F12 on the keyboard, restores the default buttons.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameController.h"
#include "GameClient/ControllerMath.h"

#include "Common/UserPreferences.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/InGameUI.h"

namespace
{
	enum { PAGE_BUTTONS = 0, PAGE_CAMERA, PAGE_STICKS, PAGE_TIMING, PAGE_FEATURES, NUM_PAGES };
	enum { KIND_BOOL, KIND_REAL, KIND_INT, KIND_BUTTON };

	const char *const PAGE_NAMES[NUM_PAGES] = { "Buttons", "Camera", "Sticks and triggers", "Timing", "Features" };
	const char *const PHYSICAL_NAMES[ControllerMath::NUM_REMAPPABLE_BUTTONS] = { "A", "B", "X", "Y", "LB", "RB" };

	const UnsignedInt CAPTURE_MS = 5000;           ///< waiting for the new button gives up after this
	const UnsignedInt DEFAULTS_CONFIRM_MS = 2500;  ///< the second Y must come within this
	const UnsignedInt MESSAGE_MS = 3500;
	const UnsignedInt EMERGENCY_HOLD_MS = 3000;    ///< both stick clicks held this long: default buttons

	struct SettingItem
	{
		Int page;
		Int kind;
		const char *label;
		const char *help;
		Bool ControllerSettings::*boolField;
		Real ControllerSettings::*realField;
		Int ControllerSettings::*intField;
		Real minValue, maxValue, step;
		const char *format;     ///< printf format of the value (KIND_REAL: %f, KIND_INT: %d)
		Int function;           ///< KIND_BUTTON: which function (0 A ... 5 RB)
	};

	// The functions of the six buttons that can be moved, named by what they do.
	const SettingItem ITEMS[] =
	{
		{ PAGE_BUTTONS, KIND_BUTTON, "Select, use, confirm", "Selects, uses the highlighted slice, confirms a target or building. Default: A", 0, 0, 0, 0, 0, 0, 0, 0 },
		{ PAGE_BUTTONS, KIND_BUTTON, "Back, cancel", "Cancels, closes, deselects. Default: B", 0, 0, 0, 0, 0, 0, 0, 1 },
		{ PAGE_BUTTONS, KIND_BUTTON, "Order, remove from queue", "Gives the order under the reticle (twice: guard, hold: line). Default: X", 0, 0, 0, 0, 0, 0, 0, 2 },
		{ PAGE_BUTTONS, KIND_BUTTON, "Command wheel", "Opens the command wheel of the selection. Default: Y", 0, 0, 0, 0, 0, 0, 0, 3 },
		{ PAGE_BUTTONS, KIND_BUTTON, "Whole army, previous", "Selects the whole army; previous page or tab; remove from queue on a wheel. Default: LB", 0, 0, 0, 0, 0, 0, 0, 4 },
		{ PAGE_BUTTONS, KIND_BUTTON, "Army on screen, next", "Selects the army on screen; next page or tab. Default: RB", 0, 0, 0, 0, 0, 0, 0, 5 },

		{ PAGE_CAMERA, KIND_REAL, "Pan speed", "How fast the left stick moves the camera (screens per second)", 0, &ControllerSettings::panRate, 0, 0.2f, 2.0f, 0.05f, "%.2f", -1 },
		{ PAGE_CAMERA, KIND_REAL, "Fast pan (LT)", "Pan speed multiplier with LT fully pulled", 0, &ControllerSettings::boostMultiplier, 0, 1.0f, 4.0f, 0.25f, "x %.2f", -1 },
		{ PAGE_CAMERA, KIND_REAL, "Slow aim (LT)", "Speed with LT held while aiming a command or placing a building", 0, &ControllerSettings::precisionMultiplier, 0, 0.1f, 1.0f, 0.05f, "x %.2f", -1 },
		{ PAGE_CAMERA, KIND_REAL, "Rotate speed", "Right stick left/right, degrees per second", 0, &ControllerSettings::yawRateDegrees, 0, 30.0f, 360.0f, 15.0f, "%.0f", -1 },
		{ PAGE_CAMERA, KIND_REAL, "Zoom speed", "Right stick up/down", 0, &ControllerSettings::zoomRate, 0, 0.1f, 2.0f, 0.1f, "%.1f", -1 },
		{ PAGE_CAMERA, KIND_BOOL, "Invert rotate", "Swap the right stick's left and right", &ControllerSettings::invertRotate, 0, 0, 0, 0, 0, 0, -1 },
		{ PAGE_CAMERA, KIND_BOOL, "Invert zoom", "Swap the right stick's up and down", &ControllerSettings::invertZoom, 0, 0, 0, 0, 0, 0, -1 },
		{ PAGE_CAMERA, KIND_REAL, "Pan smoothing", "Softens the start of a pan (milliseconds, 0 = off)", 0, &ControllerSettings::smoothingMs, 0, 0.0f, 150.0f, 5.0f, "%.0f ms", -1 },
		{ PAGE_CAMERA, KIND_BOOL, "Reticle at the map edge", "At the edge of the map the reticle moves on when the camera cannot", &ControllerSettings::edgeOffset, 0, 0, 0, 0, 0, 0, -1 },

		{ PAGE_STICKS, KIND_REAL, "Left stick dead zone", "How far the left stick must move before it counts", 0, &ControllerSettings::innerDeadzoneLeft, 0, 0.0f, 0.4f, 0.01f, "%.2f", -1 },
		{ PAGE_STICKS, KIND_REAL, "Right stick dead zone", "How far the right stick must move before it counts", 0, &ControllerSettings::innerDeadzoneRight, 0, 0.0f, 0.4f, 0.01f, "%.2f", -1 },
		{ PAGE_STICKS, KIND_REAL, "Stick response curve", "1.0 = straight; higher = finer control near the centre", 0, &ControllerSettings::responseExponent, 0, 0.8f, 3.0f, 0.1f, "%.1f", -1 },
		{ PAGE_STICKS, KIND_REAL, "Trigger dead zone", "How far a trigger must be pulled before it counts", 0, &ControllerSettings::triggerDeadzone, 0, 0.0f, 0.4f, 0.02f, "%.2f", -1 },
		{ PAGE_STICKS, KIND_REAL, "Target assist", "How close to a unit the reticle snaps to it (pixels, 0 = off)", 0, &ControllerSettings::assistRadius, 0, 0.0f, 40.0f, 2.0f, "%.0f", -1 },

		{ PAGE_TIMING, KIND_INT, "Double tap time", "A A and X X must come within this", 0, 0, &ControllerSettings::doubleTapMs, 150.0f, 500.0f, 10.0f, "%d ms", -1 },
		{ PAGE_TIMING, KIND_INT, "Hold A to paint", "Holding A this long starts the paint selection", 0, 0, &ControllerSettings::holdSelectMs, 100.0f, 600.0f, 25.0f, "%d ms", -1 },
		{ PAGE_TIMING, KIND_INT, "Hold X for a line", "Holding X this long starts drawing a line", 0, 0, &ControllerSettings::lineMoveHoldMs, 100.0f, 1000.0f, 25.0f, "%d ms", -1 },
		{ PAGE_TIMING, KIND_INT, "Repeat delay", "Held D-pad or bumper: first repeat after this", 0, 0, &ControllerSettings::repeatDelayMs, 200.0f, 800.0f, 25.0f, "%d ms", -1 },
		{ PAGE_TIMING, KIND_INT, "Repeat speed", "... then one step every this long", 0, 0, &ControllerSettings::repeatRateMs, 50.0f, 300.0f, 10.0f, "%d ms", -1 },

		{ PAGE_FEATURES, KIND_BOOL, "Radial wheels", "Off: the flat command panels instead of wheels", &ControllerSettings::wheelMenus, 0, 0, 0, 0, 0, 0, -1 },
		{ PAGE_FEATURES, KIND_BOOL, "Line move (hold X)", "Hold X and pan to spread the selection along a line", &ControllerSettings::lineMove, 0, 0, 0, 0, 0, 0, -1 },
		{ PAGE_FEATURES, KIND_BOOL, "Double tap X: guard", "A quick second X guards the spot instead of moving there", &ControllerSettings::doubleTapXGuard, 0, 0, 0, 0, 0, 0, -1 },
		{ PAGE_FEATURES, KIND_BOOL, "A on a building: its wheel", "Tapping A on one of your buildings opens its command wheel", &ControllerSettings::openWheelOnBuilding, 0, 0, 0, 0, 0, 0, -1 },
		{ PAGE_FEATURES, KIND_BOOL, "Classic reticle", "On: the original crosshair and corner brackets instead of the animated one", &ControllerSettings::classicReticle, 0, 0, 0, 0, 0, 0, -1 },
		{ PAGE_FEATURES, KIND_BOOL, "Faction reticle", "The idle reticle takes your side's colour and tick style (USA, China, GLA)", &ControllerSettings::factionReticle, 0, 0, 0, 0, 0, 0, -1 },
		{ PAGE_FEATURES, KIND_BOOL, "Reduced motion", "The reticle changes colour and shape without moving or pulsing", &ControllerSettings::reducedMotion, 0, 0, 0, 0, 0, 0, -1 },
		{ PAGE_FEATURES, KIND_BOOL, "Mouse cursor on the reticle", "The hidden mouse cursor follows the reticle (hints and circles show there)", &ControllerSettings::lockMouseToReticle, 0, 0, 0, 0, 0, 0, -1 },
		{ PAGE_FEATURES, KIND_BOOL, "Diagnostics text", "The technical lines on the left of the screen", &ControllerSettings::showDiagnostics, 0, 0, 0, 0, 0, 0, -1 }
	};
	const Int NUM_ITEMS = (Int)(sizeof(ITEMS) / sizeof(ITEMS[0]));

	UnicodeString toUnicodeText(const char *text)
	{
		UnicodeString u;
		u.translate(AsciiString(text));
		return u;
	}

	void pageItems(Int page, std::vector<Int> *out)
	{
		out->clear();
		for (Int i = 0; i < NUM_ITEMS; ++i)
		{
			if (ITEMS[i].page == page)
				out->push_back(i);
		}
	}

	Bool isWordChar(WideChar c)
	{
		return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
	}
}

//-------------------------------------------------------------------------------------------------
// Button layout
//-------------------------------------------------------------------------------------------------

const char *GameController::physicalButtonName(Int physical)
{
	return (physical >= 0 && physical < ControllerMath::NUM_REMAPPABLE_BUTTONS) ? PHYSICAL_NAMES[physical] : "?";
}

Int GameController::physicalButtonFromName(const char *name)
{
	for (Int i = 0; i < ControllerMath::NUM_REMAPPABLE_BUTTONS; ++i)
	{
		if (stricmp(name, PHYSICAL_NAMES[i]) == 0)
			return i;
	}
	return -1;
}

/// The controller's texts name buttons by what they do by default ("A: select"). With another
/// layout, each whole-word button name is swapped for the physical button that now does it.
UnicodeString GameController::buttonNamesFor(const UnicodeString &text) const
{
	if (ControllerMath::isDefaultButtonMap(m_settings.buttonMap))
		return text;

	UnicodeString out;
	const WideChar *s = text.str();
	const Int length = text.getLength();
	Int i = 0;
	while (i < length)
	{
		if (!isWordChar(s[i]))
		{
			out.concat(s[i]);
			++i;
			continue;
		}
		Int end = i;
		while (end < length && isWordChar(s[end]))
			++end;

		Int function = -1;
		for (Int f = 0; f < ControllerMath::NUM_REMAPPABLE_BUTTONS && function < 0; ++f)
		{
			const char *name = PHYSICAL_NAMES[f];
			const Int n = (Int)strlen(name);
			if (n != end - i)
				continue;
			Bool same = TRUE;
			for (Int k = 0; k < n && same; ++k)
				same = s[i + k] == (WideChar)name[k];
			if (same)
				function = f;
		}

		if (function >= 0)
		{
			const char *physical = PHYSICAL_NAMES[m_settings.buttonMap[function]];
			for (const char *p = physical; *p; ++p)
				out.concat((WideChar)*p);
		}
		else
		{
			for (Int k = i; k < end; ++k)
				out.concat(s[k]);
		}
		i = end;
	}
	return out;
}

/// The layout changed while buttons are held: what they now mean must not count as a new press.
void GameController::afterButtonMapChange()
{
	m_prevButtons = ControllerMath::remapButtons(m_rawButtons, m_settings.buttonMap) | (m_prevButtons & ~ControllerMath::REMAPPABLE_BUTTON_BITS);
}

void GameController::showSettingsMessage(const char *text)
{
	m_settingsMessage = toUnicodeText(text);
	m_settingsMessageUntilMs = timeGetTime() + MESSAGE_MS;
}

/// Both stick clicks held for a few seconds, or Ctrl+Shift+F12: the default buttons, whatever
/// the layout is (neither needs a button that can be moved).
void GameController::updateEmergencyReset(UnsignedInt nowMs, Bool appActive)
{
	const UnsignedInt both = ControllerState::BUTTON_LS | ControllerState::BUTTON_RS;
	// Once per hold: holding on does not reset, save and announce again every frame.
	Bool reset = ControllerMath::updateHoldLatch(&m_resetHold, (m_rawButtons & both) == both, nowMs, EMERGENCY_HOLD_MS);

	const Bool keys = appActive && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
		(GetAsyncKeyState(VK_F12) & 0x8000);
	if (keys && !m_resetKeysDown)
		reset = TRUE;
	m_resetKeysDown = keys;

	if (!reset)
		return;
	ControllerMath::setDefaultButtonMap(m_settings.buttonMap);
	afterButtonMapChange();
	saveSettings();
	showSettingsMessage("Default buttons restored (A, B, X, Y, LB, RB)");
	if (TheInGameUI && isBattlefieldContext())
		TheInGameUI->messageNoFormat(toUnicodeText("Controller: default buttons restored"));
}

//-------------------------------------------------------------------------------------------------
// Saving
//-------------------------------------------------------------------------------------------------

void GameController::loadButtonMap(const UserPreferences &prefs)
{
	static const char *const keys[ControllerMath::NUM_REMAPPABLE_BUTTONS] = { "ButtonA", "ButtonB", "ButtonX", "ButtonY", "ButtonLB", "ButtonRB" };
	for (Int f = 0; f < ControllerMath::NUM_REMAPPABLE_BUTTONS; ++f)
	{
		const AsciiString name = prefs.getAsciiString(keys[f], AsciiString(PHYSICAL_NAMES[f]));
		m_settings.buttonMap[f] = physicalButtonFromName(name.str());
	}
	// A layout that does not use every button exactly once is not used.
	if (!ControllerMath::isValidButtonMap(m_settings.buttonMap))
		ControllerMath::setDefaultButtonMap(m_settings.buttonMap);
}

void GameController::saveButtonMap(UserPreferences *prefs) const
{
	static const char *const keys[ControllerMath::NUM_REMAPPABLE_BUTTONS] = { "ButtonA", "ButtonB", "ButtonX", "ButtonY", "ButtonLB", "ButtonRB" };
	for (Int f = 0; f < ControllerMath::NUM_REMAPPABLE_BUTTONS; ++f)
		prefs->setAsciiString(keys[f], AsciiString(physicalButtonName(m_settings.buttonMap[f])));
}

//-------------------------------------------------------------------------------------------------
// The screen
//-------------------------------------------------------------------------------------------------

void GameController::openSettingsScreen()
{
	m_settingsOpen = TRUE;
	m_settingsBackup = m_settings;
	m_settingsCapture = -1;
	m_settingsDefaultsAskMs = 0;
	m_menuRepeatDir = -1;
	m_helpVisible = FALSE;
	closePanel();
	cancelGesture();
	clearMotion();
}

void GameController::closeSettingsScreen()
{
	m_settingsOpen = FALSE;
	m_settingsCapture = -1;
	m_settings.validate();
	saveSettings();
	showSettingsMessage("Controller settings saved");
	beginInputContext();
}

/// Change one setting by steps (Left/Right) or switch it (A).
void GameController::changeSetting(Int item, Int steps)
{
	const SettingItem &it = ITEMS[item];
	if (it.kind == KIND_BOOL)
	{
		m_settings.*(it.boolField) = !(m_settings.*(it.boolField));
	}
	else if (it.kind == KIND_REAL)
	{
		Real v = m_settings.*(it.realField) + steps * it.step;
		// Snap to the step grid, so values edited by hand line up again.
		v = it.minValue + (Real)floor((v - it.minValue) / it.step + 0.5f) * it.step;
		m_settings.*(it.realField) = ControllerMath::clampf(it.minValue, v, it.maxValue);
	}
	else if (it.kind == KIND_INT)
	{
		const Int stepSize = (Int)it.step;
		Int v = m_settings.*(it.intField) + steps * stepSize;
		v = (Int)it.minValue + ((v - (Int)it.minValue + stepSize / 2) / stepSize) * stepSize;
		if (v < (Int)it.minValue) v = (Int)it.minValue;
		if (v > (Int)it.maxValue) v = (Int)it.maxValue;
		m_settings.*(it.intField) = v;
	}
	else if (it.kind == KIND_BUTTON)
	{
		const Int n = ControllerMath::NUM_REMAPPABLE_BUTTONS;
		const Int physical = (m_settings.buttonMap[it.function] + steps % n + n) % n;
		assignButtonFor(it.function, physical);
		return;
	}
	m_settings.validate();
}

/// Put a function on a physical button, telling the player what moved.
void GameController::assignButtonFor(Int function, Int physical)
{
	const Int moved = ControllerMath::assignButton(m_settings.buttonMap, function, physical);
	afterButtonMapChange();
	char text[160];
	if (moved >= 0)
	{
		snprintf(text, sizeof(text), "%s is now on %s; %s moved to %s", ITEMS[function].label,
			physicalButtonName(m_settings.buttonMap[function]), ITEMS[moved].label, physicalButtonName(m_settings.buttonMap[moved]));
	}
	else
	{
		snprintf(text, sizeof(text), "%s is on %s", ITEMS[function].label, physicalButtonName(m_settings.buttonMap[function]));
	}
	showSettingsMessage(text);
}

void GameController::updateSettingsScreen(UnsignedInt pressed, UnsignedInt held, const ControllerState &state, UnsignedInt nowMs)
{
	std::vector<Int> rows;
	pageItems(m_settingsPage, &rows);
	if (m_settingsRow < 0 || m_settingsRow >= (Int)rows.size())
		m_settingsRow = 0;

	// --- Waiting for the new button ---------------------------------------------------------------
	if (m_settingsCapture >= 0)
	{
		const UnsignedInt rawPressed = m_rawButtons & ~m_rawPrevButtons & ControllerMath::REMAPPABLE_BUTTON_BITS;
		if (rawPressed)
		{
			Int physical = 0;
			while (!(rawPressed & (1u << physical)))
				++physical;
			const Int function = m_settingsCapture;
			m_settingsCapture = -1;
			assignButtonFor(function, physical);
			return;
		}
		const UnsignedInt cancelButtons = ControllerState::BUTTON_DPAD_UP | ControllerState::BUTTON_DPAD_DOWN |
			ControllerState::BUTTON_DPAD_LEFT | ControllerState::BUTTON_DPAD_RIGHT | ControllerState::BUTTON_MENU | ControllerState::BUTTON_VIEW;
		if ((pressed & cancelButtons) || nowMs >= m_settingsCaptureUntilMs)
		{
			m_settingsCapture = -1;
			showSettingsMessage("Button unchanged");
		}
		return;
	}

	// --- Close -----------------------------------------------------------------------------------
	if (pressed & (ControllerState::BUTTON_B | ControllerState::BUTTON_MENU | ControllerState::BUTTON_VIEW))
	{
		closeSettingsScreen();
		return;
	}

	// --- Pages -----------------------------------------------------------------------------------
	if (pressed & (ControllerState::BUTTON_LB | ControllerState::BUTTON_RB))
	{
		const Int step = (pressed & ControllerState::BUTTON_RB) ? 1 : -1;
		m_settingsPage = (m_settingsPage + step + NUM_PAGES) % NUM_PAGES;
		m_settingsRow = 0;
		m_settingsDefaultsAskMs = 0;
		return;
	}

	// --- Move and change -------------------------------------------------------------------------
	const Int dir = menuDirection(held, state, nowMs);   // 0 up, 1 right, 2 down, 3 left
	if (dir == 0 && m_settingsRow > 0)
		--m_settingsRow;
	else if (dir == 2 && m_settingsRow < (Int)rows.size() - 1)
		++m_settingsRow;
	else if ((dir == 1 || dir == 3) && !rows.empty())
		changeSetting(rows[m_settingsRow], dir == 1 ? 1 : -1);

	if ((pressed & ControllerState::BUTTON_A) && !rows.empty())
	{
		const SettingItem &it = ITEMS[rows[m_settingsRow]];
		if (it.kind == KIND_BUTTON)
		{
			m_settingsCapture = it.function;
			m_settingsCaptureUntilMs = nowMs + CAPTURE_MS;
		}
		else if (it.kind == KIND_BOOL)
		{
			changeSetting(rows[m_settingsRow], 1);
		}
	}

	// --- Defaults for this page (Y twice) --------------------------------------------------------
	if (pressed & ControllerState::BUTTON_Y)
	{
		if (m_settingsDefaultsAskMs == 0 || nowMs - m_settingsDefaultsAskMs > DEFAULTS_CONFIRM_MS)
		{
			m_settingsDefaultsAskMs = nowMs;
			showSettingsMessage("Press Y again to set this page back to its defaults");
		}
		else
		{
			m_settingsDefaultsAskMs = 0;
			ControllerSettings defaults;
			defaults.setDefaults();
			if (m_settingsPage == PAGE_BUTTONS)
			{
				ControllerMath::setDefaultButtonMap(m_settings.buttonMap);
				afterButtonMapChange();
			}
			for (size_t r = 0; r < rows.size(); ++r)
			{
				const SettingItem &it = ITEMS[rows[r]];
				if (it.kind == KIND_BOOL)
					m_settings.*(it.boolField) = defaults.*(it.boolField);
				else if (it.kind == KIND_REAL)
					m_settings.*(it.realField) = defaults.*(it.realField);
				else if (it.kind == KIND_INT)
					m_settings.*(it.intField) = defaults.*(it.intField);
			}
			if (m_settingsPage == PAGE_STICKS)
				m_settings.assistRetention = defaults.assistRetention;
			m_settings.validate();
			showSettingsMessage("This page is back to its defaults");
		}
	}

	// --- Undo everything since the screen opened ------------------------------------------------
	if (pressed & ControllerState::BUTTON_X)
	{
		m_settings = m_settingsBackup;
		afterButtonMapChange();
		showSettingsMessage("All changes undone");
	}
}

//-------------------------------------------------------------------------------------------------
// Drawing
//-------------------------------------------------------------------------------------------------

DisplayString *GameController::settingsLine(Int index)
{
	if (index < 0 || index >= NUM_SETTINGS_LINES)
		return nullptr;
	if (!m_settingsLines[index])
		m_settingsLines[index] = makeString();
	return m_settingsLines[index];
}

void GameController::formatSettingValue(Int item, char *buf, Int size) const
{
	const SettingItem &it = ITEMS[item];
	if (it.kind == KIND_BOOL)
		snprintf(buf, size, "%s", (m_settings.*(it.boolField)) ? "On" : "Off");
	else if (it.kind == KIND_REAL)
		snprintf(buf, size, it.format, m_settings.*(it.realField));
	else if (it.kind == KIND_INT)
		snprintf(buf, size, it.format, m_settings.*(it.intField));
	else if (m_settingsCapture == it.function)
		snprintf(buf, size, "press a button...");
	else
		snprintf(buf, size, "%s", physicalButtonName(m_settings.buttonMap[it.function]));
}

void GameController::drawSettingsScreen()
{
	if (!TheDisplay)
		return;
	std::vector<Int> rows;
	pageItems(m_settingsPage, &rows);

	const Int lineHeight = textLineHeight() + 6;
	const Int pad = lineHeight / 2;
	const Int boxW = REAL_TO_INT(TheDisplay->getWidth() * 0.62f);
	const Int boxH = lineHeight * ((Int)rows.size() + 6) + pad * 2;
	const Int boxX = (TheDisplay->getWidth() - boxW) / 2;
	const Int boxY = REAL_TO_INT(TheDisplay->getHeight() * 0.14f);
	const Int valueX = boxX + REAL_TO_INT(boxW * 0.62f);

	TheDisplay->drawFillRect(boxX, boxY, boxW, boxH, GameMakeColor(0, 0, 0, 215));
	TheDisplay->drawOpenRect(boxX, boxY, boxW, boxH, 1.0f, GameMakeColor(255, 196, 40, 200));

	Int line = 0;
	Int y = boxY + pad;
	char buf[200];

	// Title and pages.
	snprintf(buf, sizeof(buf), "CONTROLLER SETTINGS   %s  (%d/%d)   LB/RB: page", PAGE_NAMES[m_settingsPage], m_settingsPage + 1, (Int)NUM_PAGES);
	drawText(settingsLine(line++), toUnicodeText(buf), boxX + pad, y, GameMakeColor(255, 220, 120, 255));
	y += lineHeight * 3 / 2;

	// The rows. On the Buttons page the names are the real buttons, so they are not translated.
	for (Int r = 0; r < (Int)rows.size(); ++r)
	{
		const Bool selected = r == m_settingsRow;
		if (selected)
		{
			TheDisplay->drawFillRect(boxX + 4, y - 2, boxW - 8, lineHeight, GameMakeColor(150, 90, 10, 170));
			TheDisplay->drawOpenRect(boxX + 4, y - 2, boxW - 8, lineHeight, 2.0f, GameMakeColor(255, 196, 40, 255));
		}
		const UnsignedInt color = selected ? GameMakeColor(255, 255, 255, 255) : GameMakeColor(210, 210, 210, 255);
		drawText(settingsLine(line++), toUnicodeText(ITEMS[rows[r]].label), boxX + pad, y, color);
		formatSettingValue(rows[r], buf, sizeof(buf));
		m_textRemapOff = TRUE;
		if (selected && ITEMS[rows[r]].kind != KIND_BUTTON)
		{
			char withArrows[220];
			snprintf(withArrows, sizeof(withArrows), "<  %s  >", buf);
			drawText(settingsLine(line++), toUnicodeText(withArrows), valueX, y, GameMakeColor(255, 220, 120, 255));
		}
		else
		{
			drawText(settingsLine(line++), toUnicodeText(buf), valueX, y, selected ? GameMakeColor(255, 220, 120, 255) : color);
		}
		m_textRemapOff = FALSE;
		y += lineHeight;
	}

	// What the selected setting does.
	y += lineHeight / 2;
	if (m_settingsRow >= 0 && m_settingsRow < (Int)rows.size())
	{
		m_textRemapOff = m_settingsPage == PAGE_BUTTONS;
		drawText(settingsLine(line++), toUnicodeText(ITEMS[rows[m_settingsRow]].help), boxX + pad, y, GameMakeColor(180, 220, 255, 255));
		m_textRemapOff = FALSE;
	}
	y += lineHeight;

	// A message (what moved, saved, undone), or the recovery hint on the Buttons page.
	if (m_settingsMessageUntilMs > timeGetTime() && !m_settingsMessage.isEmpty())
	{
		m_textRemapOff = TRUE;
		drawText(settingsLine(line++), m_settingsMessage, boxX + pad, y, GameMakeColor(120, 255, 120, 255));
		m_textRemapOff = FALSE;
	}
	else if (m_settingsPage == PAGE_BUTTONS)
	{
		drawText(settingsLine(line++), toUnicodeText("Buttons lost? Hold both stick clicks for 3 s, or press Ctrl+Shift+F12"), boxX + pad, y, GameMakeColor(170, 170, 170, 255));
	}
	y += lineHeight;

	// What the buttons do here.
	const char *legend;
	if (m_settingsCapture >= 0)
		legend = "Press the button you want (A, B, X, Y, LB or RB)   D-pad: cancel";
	else if (m_settingsRow >= 0 && m_settingsRow < (Int)rows.size() && ITEMS[rows[m_settingsRow]].kind == KIND_BUTTON)
		legend = "Up/Down: choose   A: press the new button   Left/Right: cycle   Y Y: defaults   X: undo all   B: save and close";
	else
		legend = "Up/Down: choose   Left/Right: change   A: on/off   Y Y: page defaults   X: undo all   B: save and close";
	drawText(settingsLine(line++), toUnicodeText(legend), boxX + pad, y, GameMakeColor(255, 220, 120, 255));
}

/// A short message outside the screen (after closing it, or after the emergency reset).
void GameController::drawSettingsMessage()
{
	if (m_settingsOpen || m_settingsMessageUntilMs <= timeGetTime() || m_settingsMessage.isEmpty() || !TheDisplay)
		return;
	DisplayString *str = settingsLine(NUM_SETTINGS_LINES - 1);
	if (!str)
		return;
	m_textRemapOff = TRUE;
	drawText(str, m_settingsMessage, REAL_TO_INT(TheDisplay->getWidth() * 0.35f), REAL_TO_INT(TheDisplay->getHeight() * 0.08f), GameMakeColor(120, 255, 120, 255));
	m_textRemapOff = FALSE;
}
