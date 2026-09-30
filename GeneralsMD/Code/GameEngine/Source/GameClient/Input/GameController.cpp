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

// GameController.cpp /////////////////////////////////////////////////////////////////////////////
// ControllerMod @feature Halo Wars style game controller: camera and reticle.
// See GameController.h and ControllerMod/DEVELOPMENT.md.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameController.h"
#include "Common/ControllerModVersion.h"
#include "GameClient/ControllerMath.h"

#include "Common/FramePacer.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingTemplate.h"
#include "Common/UserPreferences.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Mouse.h"
#include "GameClient/Shell.h"
#include "GameClient/View.h"
#include "GameClient/GameText.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/StealthUpdate.h"
#include "GameLogic/Object.h"

#ifndef CONTROLLERMOD_ENABLE_TEST_INPUT
#define CONTROLLERMOD_ENABLE_TEST_INPUT 0   // set by the CMake option CONTROLLERMOD_TEST_INPUT
#endif

GameController *TheGameController = nullptr;

extern HWND ApplicationHWnd;

namespace
{
	/// Shown in the help and diagnostics so the tester always knows which candidate is running.
	const char *const CONTROLLER_MOD_BUILD = "Zero Hour Controller Mod " ZH_CONTROLLER_MOD_VERSION;
	const char *const SETTINGS_FILE = "ControllerMod.ini";
	const Int SETTINGS_PROFILE_VERSION = 8;   // 2: selection, 3: command bar, 4: mouse lock, 5: wheels, 6: line move, 7: building wheel, 8: button layout

	const Real MAX_FRAME_DT = 0.1f;          ///< clamp long frames (loading hitches, debugger stalls)
	const Real BASIS_PROBE_PX = 16.0f;       ///< pixel step used to measure world-per-pixel at the reticle
	const Real RETICLE_MARGIN_PX = 48.0f;    ///< edge offset keeps the reticle this far inside the view
	const Real FONT_REFERENCE_HEIGHT = 1080.0f;

	Real length2D(const Coord2D &v)
	{
		return (Real)sqrt((double)(v.x * v.x + v.y * v.y));
	}

	/// Solves world = a * right + b * down for (a, b). Returns FALSE for a degenerate basis.
	Bool solveInBasis(const Coord2D &world, const Coord2D &right, const Coord2D &down, Real *a, Real *b)
	{
		const Real det = right.x * down.y - right.y * down.x;
		if (fabs(det) < 1e-12f)
			return FALSE;
		*a = (world.x * down.y - world.y * down.x) / det;
		*b = (right.x * world.y - right.y * world.x) / det;
		return TRUE;
	}

	UnicodeString toUnicode(const char *text)
	{
		UnicodeString u;
		u.translate(AsciiString(text));
		return u;
	}
}

//-------------------------------------------------------------------------------------------------
// ControllerSettings
//-------------------------------------------------------------------------------------------------
void ControllerSettings::setDefaults()
{
	profileVersion = SETTINGS_PROFILE_VERSION;
	enabled = TRUE;
	innerDeadzoneLeft = 0.18f;
	innerDeadzoneRight = 0.20f;
	outerLimit = 0.98f;
	responseExponent = 1.6f;
	triggerDeadzone = 0.10f;
	panRate = 0.65f;
	boostMultiplier = 2.0f;
	yawRateDegrees = 90.0f;
	zoomRate = 0.6f;
	invertZoom = FALSE;
	invertRotate = FALSE;
	smoothingMs = 35.0f;
	edgeOffset = TRUE;
	mouseTakeoverPixels = 6;
	showDiagnostics = FALSE;   // off for players; the settings screen (Features) turns it on

	doubleTapMs = 260;
	holdSelectMs = 200;
	brushRadiusMin = 18.0f;
	brushRadiusMax = 90.0f;
	brushGrowMs = 450;
	assistRadius = 14.0f;
	assistRetention = 22.0f;
	triggerPress = 0.55f;
	triggerRelease = 0.35f;
	doubleTapXGuard = TRUE;
	brushUnitPadding = 10.0f;

	precisionMultiplier = 0.35f;
	repeatDelayMs = 350;
	repeatRateMs = 110;
	lockMouseToReticle = TRUE;
	wheelMenus = TRUE;
	classicReticle = FALSE;
	reducedMotion = FALSE;
	factionReticle = TRUE;
	lineMove = TRUE;
	lineMoveHoldMs = 250;
	openWheelOnBuilding = TRUE;
	ControllerMath::setDefaultButtonMap(buttonMap);
}

void ControllerSettings::validate()
{
	innerDeadzoneLeft = ControllerMath::clampf(0.0f, innerDeadzoneLeft, 0.6f);
	innerDeadzoneRight = ControllerMath::clampf(0.0f, innerDeadzoneRight, 0.6f);
	outerLimit = ControllerMath::clampf(0.5f, outerLimit, 1.0f);
	if (outerLimit <= innerDeadzoneLeft + 0.05f || outerLimit <= innerDeadzoneRight + 0.05f)
	{
		innerDeadzoneLeft = 0.18f;
		innerDeadzoneRight = 0.20f;
		outerLimit = 0.98f;
	}
	responseExponent = ControllerMath::clampf(0.5f, responseExponent, 4.0f);
	triggerDeadzone = ControllerMath::clampf(0.0f, triggerDeadzone, 0.5f);
	panRate = ControllerMath::clampf(0.05f, panRate, 5.0f);
	boostMultiplier = ControllerMath::clampf(1.0f, boostMultiplier, 6.0f);
	yawRateDegrees = ControllerMath::clampf(5.0f, yawRateDegrees, 720.0f);
	zoomRate = ControllerMath::clampf(0.05f, zoomRate, 5.0f);
	smoothingMs = ControllerMath::clampf(0.0f, smoothingMs, 200.0f);
	if (mouseTakeoverPixels < 1) mouseTakeoverPixels = 1;
	if (mouseTakeoverPixels > 200) mouseTakeoverPixels = 200;

	if (doubleTapMs < 100) doubleTapMs = 100;
	if (doubleTapMs > 600) doubleTapMs = 600;
	if (holdSelectMs < 100) holdSelectMs = 100;
	if (holdSelectMs > 1000) holdSelectMs = 1000;
	brushRadiusMin = ControllerMath::clampf(4.0f, brushRadiusMin, 200.0f);
	brushRadiusMax = ControllerMath::clampf(brushRadiusMin, brushRadiusMax, 400.0f);
	if (brushGrowMs < 0) brushGrowMs = 0;
	if (brushGrowMs > 3000) brushGrowMs = 3000;
	assistRadius = ControllerMath::clampf(0.0f, assistRadius, 80.0f);
	assistRetention = ControllerMath::clampf(assistRadius, assistRetention, 120.0f);
	triggerPress = ControllerMath::clampf(0.1f, triggerPress, 0.95f);
	triggerRelease = ControllerMath::clampf(0.05f, triggerRelease, triggerPress - 0.05f);
	brushUnitPadding = ControllerMath::clampf(0.0f, brushUnitPadding, 60.0f);
	precisionMultiplier = ControllerMath::clampf(0.05f, precisionMultiplier, 1.0f);
	if (repeatDelayMs < 100) repeatDelayMs = 100;
	if (repeatDelayMs > 1000) repeatDelayMs = 1000;
	if (repeatRateMs < 40) repeatRateMs = 40;
	if (repeatRateMs > 500) repeatRateMs = 500;
	if (lineMoveHoldMs < 100) lineMoveHoldMs = 100;
	if (lineMoveHoldMs > 1500) lineMoveHoldMs = 1500;
	if (!ControllerMath::isValidButtonMap(buttonMap))
		ControllerMath::setDefaultButtonMap(buttonMap);
}

//-------------------------------------------------------------------------------------------------
// GameController
//-------------------------------------------------------------------------------------------------
GameController::GameController() :
	m_device(nullptr),
	m_prevButtons(0),
	m_wasConnected(FALSE),
	m_lastSlot(-1),
	m_needNeutral(TRUE),
	m_suspended(FALSE),
	m_active(FALSE),
	m_lastMouseEventCount(0),
	m_haveMousePos(FALSE),
	m_mouseTravel(0),
	m_cursorLocked(FALSE),
	m_cursorHidden(FALSE),
	m_hiddenCursor(Mouse::ARROW),
	m_lockPlaced(FALSE),
	m_panX(0.0f), m_panY(0.0f),
	m_rotX(0.0f), m_rotY(0.0f),
	m_boost(0.0f),
	m_rtAnalog(0.0f),
	m_wasInBattle(FALSE),
	m_worldPerScrollX(0.0f),
	m_worldPerScrollY(0.0f),
	m_scrollScaleValid(FALSE),
	m_haveLastCameraPos(FALSE),
	m_lastDt(0.0f),
	m_hoverDrawableID(0),
	m_hoverRelation(HOVER_NONE),
	m_hoverHasGround(FALSE),
	m_hoverAssisted(FALSE),
	m_gesture(GESTURE_IDLE),
	m_gestureDownMs(0),
	m_contextGeneration(1),
	m_gestureGeneration(0),
	m_tapCandidate(INVALID_ID),
	m_lastTapReleaseMs(0),
	m_lastTapObject(INVALID_ID),
	m_lastTapGeneration(0),
	m_brushRadius(0.0f),
	m_typeIndex(0),
	m_typeShown(nullptr),
	m_rtDown(FALSE),
	m_bumperPending(0),
	m_bumperDownMs(0),
	m_bumperGeneration(0),
	m_orderHint(GameMessage::MSG_INVALID),
	m_orderFeedbackUntilMs(0),
	m_lastOrderMs(0),
	m_lastOrderGeneration(0),
	m_panelOpen(FALSE),
	m_panelKind(PANEL_NONE),
	m_panelRegion(0),
	m_returnPanel(PANEL_NONE),
	m_focusCommand(nullptr),
	m_focusChangedMs(0),
	m_waypointCount(0),
	m_wheelHighlight(FALSE),
	m_xDownMs(0),
	m_xHoldSlot(-1),
	m_xHoldProducer(INVALID_ID),
	m_xHoldButton(0),
	m_xHoldDone(FALSE),
	m_promotionRefreshMs(0),
	m_pendingWheelObject(INVALID_ID),
	m_pendingWheelUntilMs(0),
	m_lineState(LINE_IDLE),
	m_lineGuard(FALSE),
	m_lineDownMs(0),
	m_lineGeneration(0),
	m_sellConfirmUntilMs(0),
	m_scienceShownByUs(FALSE),
	m_orderMode(ORDERMODE_NONE),
	m_groupClearPending(-1),
	m_baseJumpIndex(0),
	m_alertJumpIndex(0),
	m_alertNewestFrame(0),
	m_lastAlertJumpMs(0),
	m_repeatButton(0),
	m_repeatNextMs(0),
	m_wasNativeMode(FALSE),
	m_checkTargetResult(FALSE),
	m_checkTargetCommand(nullptr),
	m_helpVisible(FALSE),
	m_font(nullptr),
	m_fontSize(0),
	m_hoverString(nullptr),
	m_menuFocus(nullptr),
	m_menuFocusStyle(0),
	m_menuFocusPicture(FALSE),
	m_menuFocusDrawable(FALSE),
	m_menuFocusShown(TRUE),
	m_menuRepeatDir(-1),
	m_menuRepeatNextMs(0),
	m_menuMode(0),
	m_menuModeWindow(nullptr),
	m_menuDropdownOriginal(-1),
	m_menuCell(0),
	m_menuHasTabs(FALSE),
	m_menuSettleUntilMs(0),
	m_menuCursorKnown(FALSE),
	m_menuMouseEvents(0),
	m_menuLegend(nullptr),
	m_settingsOpen(FALSE),
	m_settingsPage(0),
	m_settingsRow(0),
	m_settingsCapture(-1),
	m_settingsCaptureUntilMs(0),
	m_settingsDefaultsAskMs(0),
	m_settingsMessageUntilMs(0),
	m_rawButtons(0),
	m_rawPrevButtons(0),
	m_resetKeysDown(FALSE),
	m_textRemapOff(FALSE),
	m_testStartMs(0),
	m_testBattleStartMs(0),
	m_testFromStart(FALSE),
	m_testStateEnabled(FALSE),
	m_testStateNextMs(0)
{
	m_state.clear();
	m_settings.setDefaults();
	m_lastMousePos.x = m_lastMousePos.y = 0;
	m_lockScreenPos.x = m_lockScreenPos.y = 0;
	m_menuCursorScreen.x = m_menuCursorScreen.y = 0;
	m_menuFocusRect.lo.x = m_menuFocusRect.lo.y = m_menuFocusRect.hi.x = m_menuFocusRect.hi.y = 0;
	ControllerMath::resetHoldLatch(&m_resetHold);
	resetReticleVisual();
	m_reticleOffset.x = m_reticleOffset.y = 0.0f;
	m_lastCameraPos.zero();
	m_hoverGround.zero();
	for (Int i = 0; i < MAX_TEXT_LINES; ++i)
		m_lines[i] = nullptr;
	for (Int i = 0; i < 3; ++i)
		m_infoLines[i] = nullptr;
	m_panelLines[0] = m_panelLines[1] = nullptr;
	for (Int i = 0; i < NUM_GROUP_LINES; ++i)
		m_groupLines[i] = nullptr;
	for (Int i = 0; i < NUM_WHEEL_LINES; ++i)
		m_wheelLines[i] = nullptr;
	for (Int i = 0; i < NUM_SETTINGS_LINES; ++i)
		m_settingsLines[i] = nullptr;
	for (Int i = 0; i < NUM_KEYBOARD_LINES; ++i)
		m_keyboardLines[i] = nullptr;
	m_keyboardOpen = FALSE;
	m_keyboardEntry = nullptr;
	m_keyboardStop = nullptr;
	m_keyboardRow = 0;
	m_keyboardCol = 0;
	m_keyboardNumeric = FALSE;
	m_keyboardCaps = FALSE;
	m_keyboardFeedbackUntilMs = 0;
	m_keyboardMatchChat = FALSE;
	m_matchChatPendingUntilMs = 0;
	m_settingsBackup.setDefaults();
	m_dualActive[0] = m_dualActive[1] = FALSE;
	m_dualSlot[0] = m_dualSlot[1] = -1;
	m_dualCurrent = -1;
	m_stickPushed[0] = m_stickPushed[1] = FALSE;
	m_needStickNeutral[0] = m_needStickNeutral[1] = TRUE;
	for (Int k = 0; k < NUM_PANEL_KINDS; ++k)
		for (Int r = 0; r < MAX_PANEL_REGIONS; ++r)
			m_panelSlot[k][r] = 0;
}

GameController::~GameController()
{
	if (TheDisplayStringManager)
	{
		for (Int i = 0; i < MAX_TEXT_LINES; ++i)
		{
			if (m_lines[i])
				TheDisplayStringManager->freeDisplayString(m_lines[i]);
			m_lines[i] = nullptr;
		}
		if (m_hoverString)
			TheDisplayStringManager->freeDisplayString(m_hoverString);
		m_hoverString = nullptr;
		if (m_menuLegend)
			TheDisplayStringManager->freeDisplayString(m_menuLegend);
		m_menuLegend = nullptr;
		for (Int i = 0; i < NUM_SETTINGS_LINES; ++i)
		{
			if (m_settingsLines[i])
				TheDisplayStringManager->freeDisplayString(m_settingsLines[i]);
			m_settingsLines[i] = nullptr;
		}
		for (Int i = 0; i < 3; ++i)
		{
			if (m_infoLines[i])
				TheDisplayStringManager->freeDisplayString(m_infoLines[i]);
			m_infoLines[i] = nullptr;
		}
		for (Int i = 0; i < 2; ++i)
		{
			if (m_panelLines[i])
				TheDisplayStringManager->freeDisplayString(m_panelLines[i]);
			m_panelLines[i] = nullptr;
		}
		for (Int i = 0; i < NUM_GROUP_LINES; ++i)
		{
			if (m_groupLines[i])
				TheDisplayStringManager->freeDisplayString(m_groupLines[i]);
			m_groupLines[i] = nullptr;
		}
		for (Int i = 0; i < NUM_WHEEL_LINES; ++i)
		{
			if (m_wheelLines[i])
				TheDisplayStringManager->freeDisplayString(m_wheelLines[i]);
			m_wheelLines[i] = nullptr;
		}
		for (Int i = 0; i < NUM_KEYBOARD_LINES; ++i)
		{
			if (m_keyboardLines[i])
				TheDisplayStringManager->freeDisplayString(m_keyboardLines[i]);
			m_keyboardLines[i] = nullptr;
		}
	}

	delete m_device;
	m_device = nullptr;

	if (TheGameController == this)
		TheGameController = nullptr;
}

void GameController::init()
{
	loadSettings();
	loadTestInput();
#if CONTROLLERMOD_ENABLE_TEST_INPUT
	beginTestState();
#endif

	m_device = createXInputControllerDevice();
	if (m_device && !m_device->init())
	{
		DEBUG_LOG(("GameController::init - XInput not available, controller support disabled"));
	}
	reset();
}

void GameController::loadSettings()
{
	m_settings.setDefaults();

	UserPreferences prefs;
	prefs.load(SETTINGS_FILE);

	const ControllerSettings d = m_settings;
	m_settings.profileVersion     = prefs.getInt("ProfileVersion", d.profileVersion);
	m_settings.enabled            = prefs.getBool("Enabled", d.enabled);
	m_settings.innerDeadzoneLeft  = prefs.getReal("InnerDeadzoneLeft", d.innerDeadzoneLeft);
	m_settings.innerDeadzoneRight = prefs.getReal("InnerDeadzoneRight", d.innerDeadzoneRight);
	m_settings.outerLimit         = prefs.getReal("OuterLimit", d.outerLimit);
	m_settings.responseExponent   = prefs.getReal("ResponseExponent", d.responseExponent);
	m_settings.triggerDeadzone    = prefs.getReal("TriggerDeadzone", d.triggerDeadzone);
	m_settings.panRate            = prefs.getReal("PanRate", d.panRate);
	m_settings.boostMultiplier    = prefs.getReal("BoostMultiplier", d.boostMultiplier);
	m_settings.yawRateDegrees     = prefs.getReal("YawRate", d.yawRateDegrees);
	m_settings.zoomRate           = prefs.getReal("ZoomRate", d.zoomRate);
	m_settings.invertZoom         = prefs.getBool("InvertZoom", d.invertZoom);
	m_settings.invertRotate       = prefs.getBool("InvertRotate", d.invertRotate);
	m_settings.smoothingMs        = prefs.getReal("SmoothingMs", d.smoothingMs);
	m_settings.edgeOffset         = prefs.getBool("EdgeOffset", d.edgeOffset);
	m_settings.mouseTakeoverPixels = prefs.getInt("MouseTakeoverPixels", d.mouseTakeoverPixels);
	m_settings.showDiagnostics    = prefs.getBool("ShowDiagnostics", d.showDiagnostics);
	m_settings.doubleTapMs        = prefs.getInt("DoubleTapMs", d.doubleTapMs);
	m_settings.holdSelectMs       = prefs.getInt("HoldSelectMs", d.holdSelectMs);
	m_settings.brushRadiusMin     = prefs.getReal("BrushRadiusMin", d.brushRadiusMin);
	m_settings.brushRadiusMax     = prefs.getReal("BrushRadiusMax", d.brushRadiusMax);
	m_settings.brushGrowMs        = prefs.getInt("BrushGrowMs", d.brushGrowMs);
	m_settings.assistRadius       = prefs.getReal("AssistRadius", d.assistRadius);
	m_settings.assistRetention    = prefs.getReal("AssistRetention", d.assistRetention);
	m_settings.triggerPress       = prefs.getReal("TriggerPress", d.triggerPress);
	m_settings.triggerRelease     = prefs.getReal("TriggerRelease", d.triggerRelease);
	m_settings.doubleTapXGuard    = prefs.getBool("DoubleTapXGuard", d.doubleTapXGuard);
	m_settings.brushUnitPadding   = prefs.getReal("BrushUnitPadding", d.brushUnitPadding);
	m_settings.precisionMultiplier = prefs.getReal("PrecisionMultiplier", d.precisionMultiplier);
	m_settings.repeatDelayMs      = prefs.getInt("PanelRepeatDelayMs", d.repeatDelayMs);
	m_settings.repeatRateMs       = prefs.getInt("PanelRepeatRateMs", d.repeatRateMs);
	m_settings.lockMouseToReticle = prefs.getBool("LockMouseToReticle", d.lockMouseToReticle);
	m_settings.wheelMenus         = prefs.getBool("WheelMenus", d.wheelMenus);
	m_settings.classicReticle     = prefs.getBool("ClassicReticle", d.classicReticle);
	m_settings.reducedMotion      = prefs.getBool("ReducedMotion", d.reducedMotion);
	m_settings.factionReticle     = prefs.getBool("FactionReticle", d.factionReticle);
	m_settings.lineMove           = prefs.getBool("LineMove", d.lineMove);
	m_settings.lineMoveHoldMs     = prefs.getInt("LineMoveHoldMs", d.lineMoveHoldMs);
	m_settings.openWheelOnBuilding = prefs.getBool("OpenWheelOnBuilding", d.openWheelOnBuilding);
	loadButtonMap(prefs);
	m_settings.validate();

	// Write back so every setting is visible and editable in the file.
	saveSettings();
}

/// Writes every setting to ControllerMod.ini. Keys the controller does not know are kept.
void GameController::saveSettings()
{
	UserPreferences prefs;
	prefs.load(SETTINGS_FILE);
	prefs.setInt("ProfileVersion", SETTINGS_PROFILE_VERSION);
	prefs.setBool("Enabled", m_settings.enabled);
	prefs.setReal("InnerDeadzoneLeft", m_settings.innerDeadzoneLeft);
	prefs.setReal("InnerDeadzoneRight", m_settings.innerDeadzoneRight);
	prefs.setReal("OuterLimit", m_settings.outerLimit);
	prefs.setReal("ResponseExponent", m_settings.responseExponent);
	prefs.setReal("TriggerDeadzone", m_settings.triggerDeadzone);
	prefs.setReal("PanRate", m_settings.panRate);
	prefs.setReal("BoostMultiplier", m_settings.boostMultiplier);
	prefs.setReal("YawRate", m_settings.yawRateDegrees);
	prefs.setReal("ZoomRate", m_settings.zoomRate);
	prefs.setBool("InvertZoom", m_settings.invertZoom);
	prefs.setBool("InvertRotate", m_settings.invertRotate);
	prefs.setReal("SmoothingMs", m_settings.smoothingMs);
	prefs.setBool("EdgeOffset", m_settings.edgeOffset);
	prefs.setInt("MouseTakeoverPixels", m_settings.mouseTakeoverPixels);
	prefs.setBool("ShowDiagnostics", m_settings.showDiagnostics);
	prefs.setInt("DoubleTapMs", m_settings.doubleTapMs);
	prefs.setInt("HoldSelectMs", m_settings.holdSelectMs);
	prefs.setReal("BrushRadiusMin", m_settings.brushRadiusMin);
	prefs.setReal("BrushRadiusMax", m_settings.brushRadiusMax);
	prefs.setInt("BrushGrowMs", m_settings.brushGrowMs);
	prefs.setReal("AssistRadius", m_settings.assistRadius);
	prefs.setReal("AssistRetention", m_settings.assistRetention);
	prefs.setReal("TriggerPress", m_settings.triggerPress);
	prefs.setReal("TriggerRelease", m_settings.triggerRelease);
	prefs.setBool("DoubleTapXGuard", m_settings.doubleTapXGuard);
	prefs.setReal("BrushUnitPadding", m_settings.brushUnitPadding);
	prefs.setReal("PrecisionMultiplier", m_settings.precisionMultiplier);
	prefs.setInt("PanelRepeatDelayMs", m_settings.repeatDelayMs);
	prefs.setInt("PanelRepeatRateMs", m_settings.repeatRateMs);
	prefs.setBool("LockMouseToReticle", m_settings.lockMouseToReticle);
	prefs.setBool("WheelMenus", m_settings.wheelMenus);
	prefs.setBool("ClassicReticle", m_settings.classicReticle);
	prefs.setBool("ReducedMotion", m_settings.reducedMotion);
	prefs.setBool("FactionReticle", m_settings.factionReticle);
	prefs.setBool("LineMove", m_settings.lineMove);
	prefs.setInt("LineMoveHoldMs", m_settings.lineMoveHoldMs);
	prefs.setBool("OpenWheelOnBuilding", m_settings.openWheelOnBuilding);
	saveButtonMap(&prefs);
	prefs.write();
}

void GameController::reset()
{
	clearMotion();
	m_needNeutral = TRUE;
	m_reticleOffset.x = m_reticleOffset.y = 0.0f;
	m_scrollScaleValid = FALSE;
	m_needStickNeutral[0] = m_needStickNeutral[1] = TRUE;
	m_hoverDrawableID = 0;
	m_hoverRelation = HOVER_NONE;
	m_hoverName.clear();
	m_hoverHasGround = FALSE;
	m_helpVisible = FALSE;
	m_wasInBattle = FALSE;
	m_hoverAssisted = FALSE;
	resetReticleVisual();
	cancelGesture();
	m_lastTapObject = INVALID_ID;
	m_cohort.clear();
	m_appliedSelection.clear();
	m_typeIndex = 0;
	m_typeShown = nullptr;
	m_typeLabel.clear();
	m_rtDown = FALSE;
	m_bumperPending = 0;
	m_orderHint = GameMessage::MSG_INVALID;
	m_orderFeedbackUntilMs = 0;
	m_lastOrderMs = 0;
	m_panelOpen = FALSE;
	m_panelKind = PANEL_NONE;
	m_returnPanel = PANEL_NONE;
	m_repeatButton = 0;
	m_wasNativeMode = FALSE;
	m_checkTargetResult = FALSE;
	m_focusCommand = nullptr;
	m_focusChangedMs = 0;
	m_sellConfirmUntilMs = 0;
	m_scienceShownByUs = FALSE;   // the game's own windows are reset with the new game
	m_orderMode = ORDERMODE_NONE; // likewise the native force-attack and waypoint flags
	m_waypointCount = 0;
	m_groupClearPending = -1;
	resetWheel();
	m_lineState = LINE_IDLE;
	m_linePoints.clear();
	m_pendingWheelObject = INVALID_ID;
	m_baseJumpIndex = 0;
	m_alertJumpIndex = 0;
	m_alertNewestFrame = 0;
	m_lastAlertJumpMs = 0;
}

void GameController::clearMotion()
{
	m_panX = m_panY = 0.0f;
	m_rotX = m_rotY = 0.0f;
	m_boost = 0.0f;
	m_rtAnalog = 0.0f;
	m_haveLastCameraPos = FALSE;
}

//-------------------------------------------------------------------------------------------------
/** Called whenever input ownership changes (battlefield entered or left, help opened or closed,
	game menu requested). Motion stops, and the sticks must be released before they can move the
	camera again, so a stick held across the change does not act in the new context. Buttons are
	not gated, so Menu, View and B always work and can never leave the player stuck. Each stick is
	released on its own, and the triggers are not gated (a right stick still pointing at a
	wheel, or LT held to place slowly, used to freeze the camera and the building ghost). */
//-------------------------------------------------------------------------------------------------
void GameController::beginInputContext()
{
	clearMotion();
	m_needStickNeutral[0] = m_needStickNeutral[1] = TRUE;
	// A gesture started in the old context must not finish in the new one.
	++m_contextGeneration;
	cancelGesture();
}

//-------------------------------------------------------------------------------------------------
/** The controller stops being the input (disconnected, replaced by another pad, the mouse took
	over, the window lost focus): every half-finished controller action is dropped, so nothing the
	pad started can finish later without a fresh press. A new context invalidates
	gestures, taps, lines and double-tap orders; the delayed ones are cleared here as well. Stable
	state (the selection, open menus and wheels) is left alone. */
//-------------------------------------------------------------------------------------------------
void GameController::resetTransientInput()
{
	beginInputContext();
	m_bumperPending = 0;
	m_xHoldSlot = -1;
	m_pendingWheelObject = INVALID_ID;
	m_repeatButton = 0;
}

Real GameController::uiScale() const
{
	return TheDisplay ? TheDisplay->getHeight() / FONT_REFERENCE_HEIGHT : 1.0f;
}

/// One stick (0 left, 1 right) is inside its deadzone.
Bool GameController::isStickNeutral(const ControllerState &s, Int side) const
{
	Real x, y;
	if (side == 0)
		ControllerMath::processStick(s.leftX, s.leftY, m_settings.innerDeadzoneLeft, m_settings.outerLimit, 1.0f, &x, &y);
	else
		ControllerMath::processStick(s.rightX, s.rightY, m_settings.innerDeadzoneRight, m_settings.outerLimit, 1.0f, &x, &y);
	return x == 0.0f && y == 0.0f;
}

/// While the pad waits to come back to rest (after focus, connect or load) it is not the input in
/// use, so it cannot hold the mouse cursor on the reticle.
Bool GameController::isActiveInput() const
{
	return m_settings.enabled && m_state.connected && m_active && !m_suspended && !m_needNeutral;
}

Bool GameController::isNeutral(const ControllerState &s) const
{
	if (s.buttons != 0)
		return FALSE;
	Real x, y;
	ControllerMath::processStick(s.leftX, s.leftY, m_settings.innerDeadzoneLeft, m_settings.outerLimit, 1.0f, &x, &y);
	if (x != 0.0f || y != 0.0f)
		return FALSE;
	ControllerMath::processStick(s.rightX, s.rightY, m_settings.innerDeadzoneRight, m_settings.outerLimit, 1.0f, &x, &y);
	if (x != 0.0f || y != 0.0f)
		return FALSE;
	return s.leftTrigger <= 0.35f && s.rightTrigger <= 0.35f;
}

Bool GameController::isBattlefieldContext() const
{
	if (!TheGameLogic || !TheInGameUI || !TheTacticalView)
		return FALSE;
	if (!TheGameLogic->isInGame() || TheGameLogic->isInShellGame() || TheGameLogic->isLoadingMap())
		return FALSE;
	if (TheShell && TheShell->isShellActive())
		return FALSE;
	if (TheInGameUI->isQuitMenuVisible())
		return FALSE;
	// A multiplayer match's chat box and "waiting for players" screen are used like menus.
	if (isMatchChatOpen() || isDisconnectScreenOpen())
		return FALSE;
	return TRUE;
}

void GameController::onConnected()
{
	m_needNeutral = TRUE;
	m_active = TRUE;
	m_scrollScaleValid = FALSE;
	if (TheInGameUI && isBattlefieldContext())
		TheInGameUI->messageNoFormat(toUnicode("Controller connected"));
}

void GameController::onDisconnected()
{
	resetTransientInput();
	closePanel();
	endOrderMode(FALSE);
	m_prevButtons = 0;
	m_active = FALSE;
	m_needNeutral = TRUE;
	m_helpVisible = FALSE;
	if (TheInGameUI && isBattlefieldContext())
		TheInGameUI->messageNoFormat(toUnicode("Controller disconnected - camera stopped"));
}

//-------------------------------------------------------------------------------------------------
/** Returns TRUE when the player deliberately used the mouse since the last frame: any button or
	wheel event, or cursor travel beyond the takeover threshold. Tiny sensor noise does not count.
	Called every frame so the baselines never go stale.

	Buttons and wheel use Mouse's ever-increasing event counter, because MouseIO's event fields are
	overwritten by later events in the same frame and are not cleared on idle frames. */
//-------------------------------------------------------------------------------------------------
Bool GameController::mouseActedThisFrame()
{
	if (!TheMouse)
		return FALSE;

	const MouseIO *mouse = TheMouse->getMouseStatus();
	if (!mouse)
		return FALSE;

	const UnsignedInt eventCount = TheMouse->getButtonOrWheelEventCount();

	if (!m_haveMousePos)
	{
		m_lastMousePos = mouse->pos;
		m_lastMouseEventCount = eventCount;
		m_mouseTravel = 0;
		m_haveMousePos = TRUE;
		return FALSE;
	}

	m_mouseTravel += abs(mouse->pos.x - m_lastMousePos.x) + abs(mouse->pos.y - m_lastMousePos.y);
	const Bool buttonOrWheel = eventCount != m_lastMouseEventCount;

	m_lastMousePos = mouse->pos;
	m_lastMouseEventCount = eventCount;

	return buttonOrWheel || m_mouseTravel >= m_settings.mouseTakeoverPixels;
}

//-------------------------------------------------------------------------------------------------
/** Automated testing only: the environment variable CONTROLLERMOD_TEST_INPUT lists button presses
	as "ms:BUTTON+BUTTON;ms:BUTTON", timed from the first battlefield frame. Each press is held for
	150 ms. LSN, LSNE, LSE ... LSNW push the left stick fully in that compass direction; RSN ... RSNW
	the right stick; LT pulls the left trigger fully; DISC makes the pad read as unplugged and SWAP
	as another pad in another slot; @Name clicks the menu gadget whose window name ends with Name
	(as soon as it is on screen; for multiplayer tests); LOOK:Name|Name centres the camera on the
	player's oldest object whose type name contains one of the Names (so a scripted A press can pick
	a worker or a building wherever the map put it); AIM:Name|Name pushes the open wheel's stick
	towards the slice with that name for as long as the step lasts; SNAP:label appends what the
	controller sees to controllermod_snaps.txt (see GameControllerTestState.cpp). A step can hold
	for longer: "ms-ms:BUTTON"
	(from-to). A leading "shell;" times the steps from the first frame instead, for the menus; a
	later "battle;" marker times the steps after it from the first battlefield frame again, so menu
	steps and battle steps can share one script whatever the loading takes.
	When the timer starts, controllermod_test_start.txt is written to the working folder
	so a test script can line up its screenshots. Without the variable this does nothing, and a
	build configured with CONTROLLERMOD_TEST_INPUT=OFF (any build for players) ignores it. */
//-------------------------------------------------------------------------------------------------
void GameController::loadTestInput()
{
	m_testSteps.clear();
	m_testStartMs = 0;
	m_testBattleStartMs = 0;
#if CONTROLLERMOD_ENABLE_TEST_INPUT
	const char *spec = getenv("CONTROLLERMOD_TEST_INPUT");
	if (!spec || !*spec)
		return;
	m_testFromStart = strncmp(spec, "shell;", 6) == 0;
	if (m_testFromStart)
		spec += 6;

	static const struct { const char *name; UnsignedInt bit; } names[] =
	{
		{ "A", ControllerState::BUTTON_A }, { "B", ControllerState::BUTTON_B },
		{ "X", ControllerState::BUTTON_X }, { "Y", ControllerState::BUTTON_Y },
		{ "LB", ControllerState::BUTTON_LB }, { "RB", ControllerState::BUTTON_RB },
		{ "VIEW", ControllerState::BUTTON_VIEW }, { "MENU", ControllerState::BUTTON_MENU },
		{ "LS", ControllerState::BUTTON_LS }, { "RS", ControllerState::BUTTON_RS },
		{ "UP", ControllerState::BUTTON_DPAD_UP }, { "DOWN", ControllerState::BUTTON_DPAD_DOWN },
		{ "LEFT", ControllerState::BUTTON_DPAD_LEFT }, { "RIGHT", ControllerState::BUTTON_DPAD_RIGHT },
		{ "RT", ControllerState::BUTTON_RT }
	};

	const char *p = spec;
	Bool fromBattle = FALSE;
	while (*p)
	{
		if (strncmp(p, "battle;", 7) == 0)
		{
			fromBattle = TRUE;
			p += 7;
			continue;
		}
		TestStep step;
		step.fromBattle = fromBattle && m_testFromStart;
		step.atMs = (UnsignedInt)strtoul(p, (char **)&p, 10);
		step.untilMs = step.atMs + 150;
		if (*p == '-')
		{
			++p;
			step.untilMs = (UnsignedInt)strtoul(p, (char **)&p, 10);
		}
		step.buttons = 0;
		step.stickX = step.stickY = 0.0f;
		step.hasStick = FALSE;
		step.rightX = step.rightY = 0.0f;
		step.hasRight = FALSE;
		step.holdLT = FALSE;
		step.disconnect = FALSE;
		step.swapSlot = FALSE;
		step.clickName[0] = 0;
		step.clickFired = FALSE;
		step.lookName[0] = 0;
		step.lookFired = FALSE;
		step.aimName[0] = 0;
		step.snapLabel[0] = 0;
		step.snapFired = FALSE;
		if (*p == ':')
			++p;
		while (*p && *p != ';')
		{
			char token[48];
			Int n = 0;
			while (*p && *p != ';' && *p != '+' && n < 47)
				token[n++] = *p++;
			token[n] = 0;
			if (token[0] == '@')
				strcpy(step.clickName, token + 1);
			if (strncmp(token, "LOOK:", 5) == 0)
				strcpy(step.lookName, token + 5);
			if (strncmp(token, "AIM:", 4) == 0)
				strcpy(step.aimName, token + 4);
			if (strncmp(token, "SNAP:", 5) == 0)
				strcpy(step.snapLabel, token + 5);
			if (strcmp(token, "LT") == 0)
				step.holdLT = TRUE;
			if (strcmp(token, "DISC") == 0)
				step.disconnect = TRUE;
			if (strcmp(token, "SWAP") == 0)
				step.swapSlot = TRUE;
			for (Int i = 0; i < (Int)(sizeof(names) / sizeof(names[0])); ++i)
			{
				if (strcmp(token, names[i].name) == 0)
					step.buttons |= names[i].bit;
			}
			static const char *const directions[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
			for (Int d = 0; d < 8; ++d)
			{
				const Real a = d * 45.0f * PI / 180.0f;
				if (token[0] == 'L' && token[1] == 'S' && strcmp(token + 2, directions[d]) == 0)
				{
					step.stickX = (Real)sin((double)a);
					step.stickY = (Real)cos((double)a);
					step.hasStick = TRUE;
				}
				if (token[0] == 'R' && token[1] == 'S' && token[2] && strcmp(token + 2, directions[d]) == 0)
				{
					step.rightX = (Real)sin((double)a);
					step.rightY = (Real)cos((double)a);
					step.hasRight = TRUE;
				}
			}
			if (*p == '+')
				++p;
		}
		if (*p == ';')
			++p;
		if (step.buttons || step.hasStick || step.hasRight || step.holdLT || step.disconnect || step.swapSlot || step.clickName[0] || step.lookName[0]
			|| step.aimName[0] || step.snapLabel[0])
			m_testSteps.push_back(step);
	}
#endif
}

#if CONTROLLERMOD_ENABLE_TEST_INPUT
namespace
{
	struct TestLookSearch
	{
		char names[48];   ///< lower case, alternatives separated by '|'
		Object *found;
	};

	void findTestLookObject(Object *obj, void *userData)
	{
		TestLookSearch *search = (TestLookSearch *)userData;
		if (!obj || obj->isEffectivelyDead() || !obj->getTemplate())
			return;
		if (search->found && search->found->getID() < obj->getID())
			return;
		char type[128];
		strlcpy(type, obj->getTemplate()->getName().str(), sizeof(type));
		for (char *c = type; *c; ++c)
			*c = (char)tolower(*c);
		char names[48];
		strlcpy(names, search->names, sizeof(names));
		for (char *name = strtok(names, "|"); name; name = strtok(nullptr, "|"))
		{
			if (strstr(type, name))
			{
				search->found = obj;
				return;
			}
		}
	}
}

/// Automated testing only: LOOK:Name centres the camera on the player's oldest object of that type.
static void testLookAt(const char *names)
{
	Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	if (!player || !TheTacticalView)
		return;
	TestLookSearch search;
	strlcpy(search.names, names, sizeof(search.names));
	for (char *c = search.names; *c; ++c)
		*c = (char)tolower(*c);
	search.found = nullptr;
	player->iterateObjects(findTestLookObject, &search);
	if (search.found)
		TheTacticalView->userLookAt(search.found->getPosition());
}
#endif

/// Automated testing only: DISC and SWAP steps change what the device reported, before the game
/// looks at the connection (the timer is started by applyTestInput).
void GameController::applyTestConnection(ControllerState *state, Int *slot)
{
	if (m_testSteps.empty() || m_testStartMs == 0 || !state->connected)
		return;
	const UnsignedInt now = timeGetTime();
	for (size_t i = 0; i < m_testSteps.size(); ++i)
	{
		if (!isTestStepActive(m_testSteps[i], now))
			continue;
		if (m_testSteps[i].disconnect)
			state->clear();
		if (m_testSteps[i].swapSlot)
			*slot += 1;
	}
}

/// Automated testing only: whether a step is being held now (on its own clock, see loadTestInput).
Bool GameController::isTestStepActive(const TestStep &step, UnsignedInt nowMs) const
{
	const UnsignedInt start = step.fromBattle ? m_testBattleStartMs : m_testStartMs;
	if (start == 0)
		return FALSE;
	const UnsignedInt t = nowMs - start;
	return t >= step.atMs && t < step.untilMs;
}

void GameController::applyTestInput(ControllerState *state, Bool inBattle)
{
	if (m_testSteps.empty() || !state->connected)
		return;
	const UnsignedInt now = timeGetTime();
	if (m_testStartMs == 0)
	{
		if (!inBattle && !m_testFromStart)
			return;
		m_testStartMs = now;
		FILE *f = fopen("controllermod_test_start.txt", "w");
		if (f)
		{
			fprintf(f, "%u\n", now);
			fclose(f);
		}
	}
	if (inBattle && m_testBattleStartMs == 0)
		m_testBattleStartMs = now;
	for (size_t i = 0; i < m_testSteps.size(); ++i)
	{
		if (isTestStepActive(m_testSteps[i], now))
		{
			state->buttons |= m_testSteps[i].buttons;
			if (m_testSteps[i].hasStick)
			{
				state->leftX = m_testSteps[i].stickX;
				state->leftY = m_testSteps[i].stickY;
			}
			if (m_testSteps[i].hasRight)
			{
				state->rightX = m_testSteps[i].rightX;
				state->rightY = m_testSteps[i].rightY;
			}
			if (m_testSteps[i].holdLT)
				state->leftTrigger = 1.0f;
			if (m_testSteps[i].clickName[0] && !m_testSteps[i].clickFired)
			{
				m_testSteps[i].clickFired = TRUE;
				m_testPendingClick = m_testSteps[i].clickName;
			}
#if CONTROLLERMOD_ENABLE_TEST_INPUT
			if (m_testSteps[i].lookName[0] && !m_testSteps[i].lookFired && inBattle)
			{
				m_testSteps[i].lookFired = TRUE;
				testLookAt(m_testSteps[i].lookName);
			}
			Int aimSide = 0;
			Real aimDegrees = 0.0f;
			if (m_testSteps[i].aimName[0] && testAimAt(m_testSteps[i].aimName, &aimSide, &aimDegrees))
			{
				const Real a = aimDegrees * PI / 180.0f;
				if (aimSide == 0)
				{
					state->leftX = (Real)sin((double)a);
					state->leftY = (Real)cos((double)a);
				}
				else
				{
					state->rightX = (Real)sin((double)a);
					state->rightY = (Real)cos((double)a);
				}
			}
			if (m_testSteps[i].snapLabel[0] && !m_testSteps[i].snapFired)
			{
				m_testSteps[i].snapFired = TRUE;
				writeTestSnapshot(m_testSteps[i].snapLabel, now);
			}
#endif
		}
	}
}

//-------------------------------------------------------------------------------------------------
void GameController::update()
{
	updateInput();
	updateCursorLock();
#if CONTROLLERMOD_ENABLE_TEST_INPUT
	updateTestState(timeGetTime());
#endif
}

//-------------------------------------------------------------------------------------------------
/** While the controller is in use on the battlefield, the mouse cursor is parked on the reticle.
	Native code that reads the mouse position (guard and ability radius circles, cursor hints,
	mouse-over) then shows its feedback at the reticle instead of wherever the mouse was left.
	The real cursor is moved too, so the next mouse events report the same spot; that move is not
	counted as the player using the mouse. Any real mouse use still takes over as before. */
//-------------------------------------------------------------------------------------------------
void GameController::updateCursorLock()
{
	Bool lock = m_settings.lockMouseToReticle && TheMouse && isActiveInput() && isBattlefieldContext();
	if (lock && TheGameEngine && !TheGameEngine->isActive())
		lock = FALSE;
	if (lock && GetForegroundWindow() != ApplicationHWnd)
		lock = FALSE;

	m_cursorLocked = lock;
	if (!lock)
	{
		m_lockPlaced = FALSE;
		// Put the cursor image back at once, before anything this frame saves or reads it.
		if (m_cursorHidden && TheMouse)
		{
			if (TheMouse->getMouseCursor() == Mouse::NONE)
				TheMouse->setCursor((Mouse::MouseCursor)m_hiddenCursor);
			m_cursorHidden = FALSE;
		}
		return;
	}

	Real rx, ry;
	getReticleScreenPos(&rx, &ry);
	ICoord2D pos;
	pos.x = REAL_TO_INT(rx);
	pos.y = REAL_TO_INT(ry);
	if (TheDisplay)
	{
		const Int w = (Int)TheDisplay->getWidth();
		const Int h = (Int)TheDisplay->getHeight();
		if (pos.x < 0) pos.x = 0;
		if (pos.y < 0) pos.y = 0;
		if (pos.x > w - 1) pos.x = w - 1;
		if (pos.y > h - 1) pos.y = h - 1;
	}

	adjustCursorParkPoint(&pos);

	// The game's cursor position, for everything that reads it later this frame ...
	TheMouse->setPosition(pos.x, pos.y);
	m_lastMousePos = pos;

	// ... and the real one, so the next mouse events agree.
	POINT target;
	target.x = pos.x;
	target.y = pos.y;
	if (!ClientToScreen(ApplicationHWnd, &target))
		return;
	POINT current;
	if (!GetCursorPos(&current))
		return;

	// The player moved the real mouse since the lock last placed it, and the game has not seen
	// that movement yet: leave the cursor there for one frame, so the next mouse update sees it
	// and the mouse takes over. If the controller is being used in that frame, it wins and the
	// cursor is put back then.
	if (m_lockPlaced &&
		abs(current.x - m_lockScreenPos.x) + abs(current.y - m_lockScreenPos.y) >= m_settings.mouseTakeoverPixels)
	{
		m_lockPlaced = FALSE;
		return;
	}

	if (current.x != target.x || current.y != target.y)
		SetCursorPos(target.x, target.y);
	m_lockScreenPos.x = target.x;
	m_lockScreenPos.y = target.y;
	m_lockPlaced = TRUE;
}

//-------------------------------------------------------------------------------------------------
/** Called just before the mouse is drawn: the reticle replaces the cursor while it is locked.
	The native code picks the cursor image again every frame, so this is repeated each frame. */
//-------------------------------------------------------------------------------------------------
void GameController::applyCursorHiding()
{
	if (!TheMouse || !m_cursorLocked)
		return;
	const Mouse::MouseCursor current = TheMouse->getMouseCursor();
	if (current != Mouse::NONE)
		m_hiddenCursor = current;
	TheMouse->setCursor(Mouse::NONE);
	m_cursorHidden = TRUE;

	// The game's hover tooltip (owner name) would sit on top of the controller's own hover label.
	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
}

//-------------------------------------------------------------------------------------------------
void GameController::updateInput()
{
	if (!m_settings.enabled || !m_device)
		return;

	Real dt = TheFramePacer ? TheFramePacer->getUpdateTime() : (1.0f / 30.0f);
	dt = ControllerMath::clampf(0.0f, dt, MAX_FRAME_DT);
	m_lastDt = dt;

	ControllerState state;
	m_device->poll(&state);

	// The button layout: everything below sees what the buttons do, not which they are.
	m_rawPrevButtons = m_rawButtons;
	m_rawButtons = state.connected ? state.buttons : 0;
	state.buttons = ControllerMath::remapButtons(state.buttons, m_settings.buttonMap);

	// A different pad in the same poll (the one in use was unplugged and another took over) is a
	// disconnect and a new connection: nothing held on either pad carries over.
	Int slot = m_device->getActiveSlot();
	applyTestConnection(&state, &slot);
	if (state.connected && m_wasConnected && slot != m_lastSlot)
	{
		onDisconnected();
		onConnected();
	}
	else if (state.connected && !m_wasConnected)
		onConnected();
	else if (!state.connected && m_wasConnected)
		onDisconnected();
	m_wasConnected = state.connected;
	m_lastSlot = state.connected ? slot : -1;
	m_state = state;

	if (!state.connected)
		return;

	// RT acts as a button, with hysteresis, so it gets clean press edges like the others.
	m_rtDown = ControllerMath::updateTriggerButton(m_rtDown, state.rightTrigger, m_settings.triggerPress, m_settings.triggerRelease);
	if (m_rtDown)
		state.buttons |= ControllerState::BUTTON_RT;
	applyTestInput(&state, isBattlefieldContext());
	m_state.buttons = state.buttons;

	// Never act while the game window is in the background.
	Bool appActive = TheGameEngine ? TheGameEngine->isActive() : TRUE;
#if CONTROLLERMOD_ENABLE_TEST_INPUT
	// Automated testing only: scripted input drives a window that is not in front (two game
	// windows side by side in a multiplayer test). Player builds never do this.
	if (!m_testSteps.empty())
		appActive = TRUE;
#endif
	updateEmergencyReset(timeGetTime(), appActive);
	if (!appActive)
	{
		if (!m_suspended)
		{
			// The mouse is what comes back to the game first: nothing the controller armed may be
			// left for it (a force-attack or waypoint mode, the cursor lock, a gesture or a line).
			m_suspended = TRUE;
			endOrderMode(FALSE);
			resetTransientInput();
			m_active = FALSE;
		}
		m_prevButtons = state.buttons;
		return;
	}
	if (m_suspended)
	{
		m_suspended = FALSE;
		m_needNeutral = TRUE;
		m_haveMousePos = FALSE;
	}

	// After connecting, regaining focus or loading, wait until the pad is at rest so a button or
	// stick that was already held does not count as a fresh input.
	if (m_needNeutral)
	{
		m_prevButtons = state.buttons;
		if (!isNeutral(state))
			return;
		m_needNeutral = FALSE;
		m_haveMousePos = FALSE;   // mouse baselines went stale while waiting
	}

	const UnsignedInt pressed = state.buttons & ~m_prevButtons;
	const UnsignedInt released = m_prevButtons & ~state.buttons;
	m_prevButtons = state.buttons;
	const UnsignedInt nowMs = timeGetTime();

	// --- 1. Resolve input ownership before any motion is processed ----------------------------
	const Bool inBattle = isBattlefieldContext();
	if (inBattle != m_wasInBattle)
	{
		// Entering or leaving the battlefield (load, game menu, end screen): drop transient state.
		// The cursor is repositioned during loads, so restart the mouse takeover measurement too.
		m_wasInBattle = inBattle;
		m_reticleOffset.x = m_reticleOffset.y = 0.0f;
		m_helpVisible = FALSE;
		m_haveMousePos = FALSE;
		m_mouseTravel = 0;
		if (m_panelOpen)
		{
			m_panelOpen = FALSE;
			setScienceWindowShown(FALSE);
		}
		m_returnPanel = PANEL_NONE;
		endOrderMode(FALSE);
		beginInputContext();
		// A menu opened while the pad was in use shows its focus straight away; the cursor was
		// moved by the battlefield lock, so its last position starts fresh.
		m_menuFocusShown = m_active;
		m_menuCursorKnown = FALSE;
		m_menuRepeatDir = -1;
	}

	if (inBattle)
	{
		// A target click sent last frame has been processed now: still armed means it was refused.
		if (m_checkTargetResult)
		{
			if (isTargetingMode() && TheInGameUI->getGUICommand() == m_checkTargetCommand)
				showFeedback("Not a valid target");
			m_checkTargetResult = FALSE;
		}

		// Targeting or placement ended without B (done, or by the mouse): B must not reopen the panel later.
		const Bool nativeMode = isPlacementMode() || isTargetingMode();
		if (m_wasNativeMode && !nativeMode && !m_panelOpen && m_orderMode == ORDERMODE_NONE)
			m_returnPanel = PANEL_NONE;
		m_wasNativeMode = nativeMode;
	}

	// Start-up videos and other full-screen movies: A, B or Start skip them.
	if (!inBattle && !m_settingsOpen && skipMovieWithPad(pressed))
	{
		clearMotion();
		return;
	}

	// The controller settings screen owns the pad while it is open. In the menus View opens
	// it; in a match, Y from the help. A frame that began with it open is its own, including the
	// press that closes it: that B, Menu or View must not also reach the menu or the battlefield
	// underneath.
	const Bool settingsOwnedFrame = m_settingsOpen;
	if (!m_settingsOpen && !inBattle && (pressed & ControllerState::BUTTON_VIEW))
		openSettingsScreen();
	else if (m_settingsOpen)
		updateSettingsScreen(pressed, state.buttons, state, nowMs);
	if (m_settingsOpen || settingsOwnedFrame)
	{
		m_orderHint = GameMessage::MSG_INVALID;
		clearMotion();
		return;
	}

	// What is under the reticle, as seen in the frame the player is looking at, before any action.
	if (inBattle && !m_helpVisible)
		updateHover();

	handleButtons(pressed, released, state.buttons, nowMs);   // may open or close help or the game menu (and then begins a new context)

	// Everything that is not the battlefield is one of the game's own menus. In a match,
	// Menu keeps its battlefield meaning (it closes the pause menu); outside one it jumps to Play.
	if (!inBattle && !m_helpVisible)
	{
		if (isMatchChatOpen() && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame())
			updateMatchChat(pressed, menuDirection(state.buttons, state, nowMs), nowMs);
		else
			updateMenuNavigation(pressed, state.buttons, state, nowMs);
	}
	if (m_matchChatPendingUntilMs != 0 && (Int)(nowMs - m_matchChatPendingUntilMs) > 0)
		m_matchChatPendingUntilMs = 0;   // the game did not open the chat box (an observer's allies chat)

	if (inBattle)
	{
		updatePanel();
		updateOrderMode();
		updateLineMove(nowMs, state.buttons);
		updatePendingBuildingWheel(nowMs);
	}
	else
	{
		m_lineState = LINE_IDLE;
		m_pendingWheelObject = INVALID_ID;
	}

	if (inBattle && !m_helpVisible && !m_panelOpen && !isPlacementMode() && TheInGameUI->getInputEnabled())
	{
		updateSelectionGesture(nowMs);
		updateOrderHint();
	}
	else
	{
		m_orderHint = GameMessage::MSG_INVALID;
	}

	// --- 2. Sticks and triggers ---------------------------------------------------------------
	Real lx, ly, rx, ry;
	ControllerMath::processStick(state.leftX, state.leftY, m_settings.innerDeadzoneLeft, m_settings.outerLimit,
		m_settings.responseExponent, &lx, &ly);
	ControllerMath::processStick(state.rightX, state.rightY, m_settings.innerDeadzoneRight, m_settings.outerLimit,
		m_settings.responseExponent, &rx, &ry);
	const Real boost = ControllerMath::processTrigger(state.leftTrigger, m_settings.triggerDeadzone);
	const Real fast = ControllerMath::processTrigger(state.rightTrigger, m_settings.triggerDeadzone);

	// Whichever device was used last owns the reticle. The mouse check runs every frame so its
	// baselines stay current; controller input wins a tie.
	const Bool mouseActed = mouseActedThisFrame();
	const Bool anyInput = pressed != 0 || lx != 0.0f || ly != 0.0f || rx != 0.0f || ry != 0.0f || boost > 0.0f;
	if (anyInput)
	{
		m_active = TRUE;
		m_mouseTravel = 0;
	}
	else if (mouseActed)
	{
		// The mouse took over: an edge offset belongs to controller panning, so it goes too, and
		// the mouse must not inherit a force-attack or waypoint mode it cannot see.
		m_active = FALSE;
		m_mouseTravel = 0;
		m_reticleOffset.x = m_reticleOffset.y = 0.0f;
		endOrderMode(FALSE);
		// A held A (brush), a pending bumper or a held X must not finish over what the mouse does.
		resetTransientInput();
	}

	// After a context change each stick must come back to rest before it counts again. The two
	// sticks are released separately, so one still held (say the right stick that pointed at a
	// wheel) never locks the other.
	for (Int s = 0; s < 2; ++s)
	{
		if (m_needStickNeutral[s] && isStickNeutral(state, s))
			m_needStickNeutral[s] = FALSE;
	}

	// An open wheel owns the sticks: they point at sectors (the camera stays still).
	if (inBattle && m_panelOpen && m_settings.wheelMenus)
		updateWheelSticks(state.leftX, state.leftY, state.rightX, state.rightY);

	// --- 3. Camera, only when the battlefield owns the sticks ---------------------------------
	const Bool worldOwnsSticks = inBattle && !m_helpVisible && !m_panelOpen && TheInGameUI->getInputEnabled();
	if (!worldOwnsSticks)
	{
		clearMotion();
		return;
	}
	if (m_needStickNeutral[0])
		lx = ly = 0.0f;
	if (m_needStickNeutral[1])
		rx = ry = 0.0f;

	m_boost = boost;
	m_rtAnalog = fast;
	m_rotX = rx;
	m_rotY = ry;

	// Smooth the pan while the stick is held, but stop at once when it returns to the deadzone
	// so the camera never slides past a small target.
	if (lx == 0.0f && ly == 0.0f)
	{
		m_panX = m_panY = 0.0f;
	}
	else
	{
		const Real alpha = ControllerMath::smoothingAlpha(dt, m_settings.smoothingMs / 1000.0f);
		m_panX += (lx - m_panX) * alpha;
		m_panY += (ly - m_panY) * alpha;
	}

	updateCamera(dt);
}

//-------------------------------------------------------------------------------------------------
void GameController::handleButtons(UnsignedInt pressed, UnsignedInt released, UnsignedInt held, UnsignedInt nowMs)
{
	if (!TheGameLogic || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame() || TheGameLogic->isLoadingMap())
		return;

	// The chat box, the "waiting for players" screen and the on-screen keyboard keep every button,
	// Menu included (Start is the keyboard's Done).
	if (m_keyboardOpen || isMatchChatOpen() || isDisconnectScreenOpen())
		return;

	// Menu behaves exactly like Esc: the game's own handler toggles the in-game menu. It is checked
	// before anything that depends on the battlefield, so the menu can always be closed again with
	// the controller.
	if (pressed & ControllerState::BUTTON_MENU)
	{
		m_helpVisible = FALSE;
		beginInputContext();
		TheMessageStream->appendMessage(GameMessage::MSG_META_OPTIONS);
		return;
	}

	if (!isBattlefieldContext())
		return;

	// While the help is open it owns the controller: View or B close it, Y opens the controller
	// settings, and in a multiplayer match A and X open the chat (everyone, allies).
	if (m_helpVisible)
	{
		if (pressed & (ControllerState::BUTTON_VIEW | ControllerState::BUTTON_B))
		{
			m_helpVisible = FALSE;
			beginInputContext();
		}
		else if (pressed & ControllerState::BUTTON_Y)
		{
			openSettingsScreen();
		}
		else if (pressed & (ControllerState::BUTTON_A | ControllerState::BUTTON_X))
		{
			if (TheGameLogic->isInMultiplayerGame() && !TheGameLogic->isInReplayGame())
			{
				m_helpVisible = FALSE;
				beginInputContext();
				openMatchChat((pressed & ControllerState::BUTTON_X) != 0, nowMs);
			}
		}
		return;
	}

	if (pressed & ControllerState::BUTTON_VIEW)
	{
		m_helpVisible = TRUE;
		beginInputContext();
		return;
	}

	// Scripted scenes disable player input; no selecting, ordering or commands then, like the mouse.
	if (!TheInGameUI->getInputEnabled())
	{
		cancelGesture();
		closePanel();
		endOrderMode(FALSE);
		return;
	}

	// D-pad left/right jump the camera to a base or an alert. They also work while a command or a
	// building is being aimed, so a power can be aimed at the alert. Panels use the D-pad themselves.
	if (!m_panelOpen)
	{
		if (pressed & ControllerState::BUTTON_DPAD_LEFT)
			jumpToBase();
		if (pressed & ControllerState::BUTTON_DPAD_RIGHT)
			jumpToAlert();
	}

	// The native mode decides who owns A, B and X: placement, then targeting, then an order mode,
	// then the panel.
	if (isPlacementMode())
	{
		handlePlacementButtons(pressed);
		return;
	}

	const Bool resetView = (pressed & ControllerState::BUTTON_RS) != 0 && !m_panelOpen;
	if (resetView)
	{
		// Same as the native middle mouse click: default yaw, pitch and zoom, ground focus kept.
		TheTacticalView->userResetPivotToGround();
		TheTacticalView->userSetAngleToDefault();
		TheTacticalView->userSetPitchToDefault();
		TheTacticalView->userSetZoomToDefault();
	}

	if (isTargetingMode())
	{
		handleTargetButtons(pressed);
		return;
	}

	if (m_orderMode != ORDERMODE_NONE)
	{
		handleOrderModeButtons(pressed);
		return;
	}

	if (m_panelOpen)
	{
		handlePanelButtons(pressed, held, nowMs);
		return;
	}

	// B closes the promotion window first, however it was opened.
	if ((pressed & ControllerState::BUTTON_B) && closeScienceWindow())
	{
		beginInputContext();
		return;
	}

	if (pressed & ControllerState::BUTTON_Y)
	{
		openPanel(PANEL_COMMANDS);
		return;
	}
	if (pressed & ControllerState::BUTTON_DPAD_UP)
	{
		openPanel(PANEL_POWERS);
		return;
	}
	if (pressed & ControllerState::BUTTON_DPAD_DOWN)
	{
		openPanel(PANEL_GROUPS);
		return;
	}

	handleWorldButtons(pressed, released, held, nowMs);
}

//-------------------------------------------------------------------------------------------------
void GameController::getReticleScreenPos(Real *x, Real *y) const
{
	Int ox = 0, oy = 0;
	TheTacticalView->getOrigin(&ox, &oy);
	*x = ox + TheTacticalView->getWidth() * 0.5f + m_reticleOffset.x;
	*y = oy + TheTacticalView->getHeight() * 0.5f + m_reticleOffset.y;
}

//-------------------------------------------------------------------------------------------------
/** World XY displacement for one pixel of screen movement at the reticle, to the right and down.
	Measured on the horizontal plane through the camera pivot, so it follows zoom, pitch and yaw. */
//-------------------------------------------------------------------------------------------------
Bool GameController::computeReticleBasis(Real reticleX, Real reticleY, Coord2D *worldPerPxRight, Coord2D *worldPerPxDown) const
{
	const Real z = TheTacticalView->getPosition().z;
	ICoord2D s0, s1, s2;
	s0.x = REAL_TO_INT(reticleX);
	s0.y = REAL_TO_INT(reticleY);
	s1.x = s0.x + (Int)BASIS_PROBE_PX;
	s1.y = s0.y;
	s2.x = s0.x;
	s2.y = s0.y + (Int)BASIS_PROBE_PX;

	Coord3D w0, w1, w2;
	if (TheTacticalView->screenToWorldAtZ(&s0, &w0, z) == PlaneClass::NO_INTERSECTION)
		return FALSE;
	if (TheTacticalView->screenToWorldAtZ(&s1, &w1, z) == PlaneClass::NO_INTERSECTION)
		return FALSE;
	if (TheTacticalView->screenToWorldAtZ(&s2, &w2, z) == PlaneClass::NO_INTERSECTION)
		return FALSE;

	worldPerPxRight->x = (w1.x - w0.x) / BASIS_PROBE_PX;
	worldPerPxRight->y = (w1.y - w0.y) / BASIS_PROBE_PX;
	worldPerPxDown->x = (w2.x - w0.x) / BASIS_PROBE_PX;
	worldPerPxDown->y = (w2.y - w0.y) / BASIS_PROBE_PX;

	return length2D(*worldPerPxRight) > 1e-5f && length2D(*worldPerPxDown) > 1e-5f;
}

//-------------------------------------------------------------------------------------------------
/** Pan, boost, rotate and zoom. Pan goes through View::userScrollBy, the same call keyboard
	scrolling uses, so camera locks, map constraints and terrain following behave natively. */
//-------------------------------------------------------------------------------------------------
void GameController::updateCamera(Real dt)
{
	View *view = TheTacticalView;
	const Int viewWidth = view->getWidth();
	const Int viewHeight = view->getHeight();
	if (viewWidth <= 0 || viewHeight <= 0 || dt <= 0.0f)
		return;

	// --- Right stick: rotation and zoom -------------------------------------------------------
	const Bool placing = isPlacementMode();
	const Bool precise = placing || isTargetingMode() || m_orderMode != ORDERMODE_NONE;

	if (m_rotX != 0.0f)
	{
		const Real dir = m_settings.invertRotate ? -1.0f : 1.0f;
		const Real deltaAngle = DEG_TO_RADF(m_settings.yawRateDegrees) * m_rotX * dt * dir;
		if (placing)
		{
			// While placing, the right stick turns the building, and the camera yaw stays fixed.
			if (!TheInGameUI->isPlacementAnchored())
				TheInGameUI->setPendingPlaceAngle(TheInGameUI->getPlacementAngle() - deltaAngle);
		}
		else
		{
			view->userSetAngle(view->getAngle() + deltaAngle);
		}
	}
	if (m_rotY != 0.0f)
	{
		// Up zooms out (higher camera), down zooms in, unless inverted.
		const Real dir = m_settings.invertZoom ? -1.0f : 1.0f;
		const Real range = TheGlobalData->m_maxCameraHeight - TheGlobalData->m_minCameraHeight;
		view->userZoom(m_settings.zoomRate * range * m_rotY * dt * dir);
	}

	// --- Reticle basis ------------------------------------------------------------------------
	Real reticleX, reticleY;
	getReticleScreenPos(&reticleX, &reticleY);

	Coord2D right, down;
	if (!computeReticleBasis(reticleX, reticleY, &right, &down))
	{
		m_haveLastCameraPos = FALSE;
		return;
	}

	// An edge offset only makes sense while the camera is held at the border by our own panning.
	// If something else jumped the camera (minimap click, view bookmark, camera lock), drop it.
	const Coord3D pos = view->getPosition();   // a copy: scrolling below changes the view's position
	if (m_haveLastCameraPos)
	{
		const Real jumpX = pos.x - m_lastCameraPos.x;
		const Real jumpY = pos.y - m_lastCameraPos.y;
		const Real jumpLimit = 0.25f * viewWidth * length2D(right);
		if (jumpX * jumpX + jumpY * jumpY > jumpLimit * jumpLimit)
			m_reticleOffset.x = m_reticleOffset.y = 0.0f;
	}
	if (!m_settings.edgeOffset)
		m_reticleOffset.x = m_reticleOffset.y = 0.0f;

	// --- Left stick: pan request in screen pixels at the reticle ------------------------------
	// LT is a boost on the battlefield. While targeting or placing, LT is slow and RT is fast
	// (RT's battlefield job, type cycling, does not apply there).
	const Real boost = precise
		? (1.0f - (1.0f - m_settings.precisionMultiplier) * m_boost) * (1.0f + (m_settings.boostMultiplier - 1.0f) * m_rtAnalog)
		: 1.0f + (m_settings.boostMultiplier - 1.0f) * m_boost;
	const Real pxPerSecond = m_settings.panRate * viewWidth * boost;
	Real reqX = m_panX * pxPerSecond * dt;
	Real reqY = -m_panY * pxPerSecond * dt;   // stick up = screen up = negative screen Y

	// --- Map edge ------------------------------------------------------------------------------
	// Reversing walks the reticle back first; the border clip is predicted from the view's own
	// constraint area (nothing is measured after the fact, so unrelated camera movement cannot be
	// mistaken for the border); offset no longer needed is handed back at pan speed.
	// See ControllerMath::resolveEdgeOffset, which is unit tested.
	Region2D area;
	const Bool hasArea = m_settings.edgeOffset && view->getCameraAreaConstraints(&area);
	if (!hasArea)
	{
		area.lo.x = area.lo.y = area.hi.x = area.hi.y = 0.0f;
	}
	ControllerMath::resolveEdgeOffset(&reqX, &reqY, &m_reticleOffset.x, &m_reticleOffset.y,
		pos.x, pos.y, right.x, right.y, down.x, down.y,
		hasArea != FALSE, area.lo.x, area.lo.y, area.hi.x, area.hi.y,
		m_settings.panRate * viewWidth * dt);

	const Real maxOffsetX = ControllerMath::clampf(0.0f, viewWidth * 0.5f - RETICLE_MARGIN_PX, 100000.0f);
	const Real maxOffsetY = ControllerMath::clampf(0.0f, viewHeight * 0.5f - RETICLE_MARGIN_PX, 100000.0f);
	m_reticleOffset.x = ControllerMath::clampf(-maxOffsetX, m_reticleOffset.x, maxOffsetX);
	m_reticleOffset.y = ControllerMath::clampf(-maxOffsetY, m_reticleOffset.y, maxOffsetY);

	m_lastCameraPos = pos;
	m_haveLastCameraPos = TRUE;

	if (fabs(reqX) < 0.001f && fabs(reqY) < 0.001f)
		return;

	// --- Convert pixels to View::scrollBy units ------------------------------------------------
	// scrollBy maps its delta linearly to a camera-relative world move that does not depend on
	// zoom. Its scale (world units per scroll unit) is measured from our own scrolls, so no engine
	// constants are copied here.
	const Real rightLen = length2D(right);
	const Real downLen = length2D(down);
	const Coord3D before = view->getPosition();

	if (!m_scrollScaleValid)
	{
		// One unit probe along both axes to learn the scale. Its movement is subtracted below.
		Coord2D probe;
		probe.x = 1.0f;
		probe.y = 1.0f;
		if (!view->userScrollBy(&probe))
			return;
		const Coord3D after = view->getPosition();
		Coord2D moved;
		moved.x = after.x - before.x;
		moved.y = after.y - before.y;
		Real movedRightPx, movedDownPx;
		if (!solveInBasis(moved, right, down, &movedRightPx, &movedDownPx))
			return;
		m_worldPerScrollX = (Real)fabs(movedRightPx) * rightLen;
		m_worldPerScrollY = (Real)fabs(movedDownPx) * downLen;
		m_scrollScaleValid = m_worldPerScrollX > 1e-5f && m_worldPerScrollY > 1e-5f;
		if (!m_scrollScaleValid)
			return;
		reqX -= movedRightPx;
		reqY -= movedDownPx;
	}

	Coord2D delta;
	delta.x = reqX * rightLen / m_worldPerScrollX;
	delta.y = reqY * downLen / m_worldPerScrollY;

	const Coord3D start = view->getPosition();
	if (!view->userScrollBy(&delta))
		return;   // camera is locked by a script

	// Refine the scale from what actually happened, in case resolution or FOV changed.
	const Coord3D end = view->getPosition();
	Coord2D moved;
	moved.x = end.x - start.x;
	moved.y = end.y - start.y;
	Real movedRightPx, movedDownPx;
	if (solveInBasis(moved, right, down, &movedRightPx, &movedDownPx))
	{
		const Real blend = 0.2f;
		if (fabs(delta.x) > 0.01f)
			m_worldPerScrollX += ((Real)fabs(movedRightPx * rightLen / delta.x) - m_worldPerScrollX) * blend;
		if (fabs(delta.y) > 0.01f)
			m_worldPerScrollY += ((Real)fabs(movedDownPx * downLen / delta.y) - m_worldPerScrollY) * blend;
	}

	m_lastCameraPos = end;
}

//-------------------------------------------------------------------------------------------------
/** The owner and type the local player is allowed to see for an object. Mirrors the native mouse
	tooltip (InGameUI::handleMouseOverHint, MSG_MOUSEOVER_DRAWABLE_HINT): containers report their
	apparent controlling player, and disguised units (e.g. a disguised Bomb Truck) show their
	disguise to everyone except allies of the real owner and observers. Keep in sync with it. */
//-------------------------------------------------------------------------------------------------
void GameController::getApparentIdentity(const Object *obj, const Player *local,
	const Player **apparentPlayer, const ThingTemplate **apparentTemplate)
{
	const Player *player = nullptr;
	const ThingTemplate *thingTemplate = obj->getTemplate();

	ContainModuleInterface *contain = obj->getContain();
	if (contain)
		player = contain->getApparentControllingPlayer(local);

	if (player == nullptr)
		player = obj->getControllingPlayer();

	if (player && obj->isKindOf(KINDOF_DISGUISER))
	{
		StealthUpdate *stealth = obj->getStealth();
		if (stealth && stealth->isDisguised())
		{
			if (player->getRelationship(local->getDefaultTeam()) != ALLIES && local->isPlayerActive())
			{
				const Player *disguisedPlayer = ThePlayerList->getNthPlayer(stealth->getDisguisedPlayerIndex());
				const ThingTemplate *disguisedTemplate = stealth->getDisguisedTemplate();
				if (disguisedPlayer)
					player = disguisedPlayer;
				if (disguisedTemplate)
					thingTemplate = disguisedTemplate;
			}
		}
	}

	*apparentPlayer = player;
	*apparentTemplate = thingTemplate;
}

//-------------------------------------------------------------------------------------------------
/** What is under the reticle. Uses the same picking and the same identity rules as the mouse
	hover, so nothing hidden by shroud, stealth or disguise is revealed. Presentation only. */
//-------------------------------------------------------------------------------------------------
void GameController::updateHover()
{
	const UnsignedInt previousHover = m_hoverDrawableID;
	m_hoverDrawableID = 0;
	m_hoverAssisted = FALSE;
	m_hoverRelation = HOVER_NONE;
	m_hoverName.clear();
	m_hoverHasGround = FALSE;

	Real rx, ry;
	getReticleScreenPos(&rx, &ry);
	ICoord2D pixel;
	pixel.x = REAL_TO_INT(rx);
	pixel.y = REAL_TO_INT(ry);

	m_hoverHasGround = TheTacticalView->screenToTerrain(&pixel, &m_hoverGround);

	Bool assisted = FALSE;
	Drawable *draw = findHoverDrawable(previousHover, &assisted);
	if (!draw)
		return;

	Object *obj = draw->getObject();
	if (!obj || obj->isEffectivelyDead())
		return;

	Player *local = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	if (!local)
		return;

	const Player *player = nullptr;
	const ThingTemplate *thingTemplate = nullptr;
	getApparentIdentity(obj, local, &player, &thingTemplate);
	if (!player || !thingTemplate)
		return;

	if (player == local)
		m_hoverRelation = HOVER_OWN;
	else
	{
		switch (local->getRelationship(player->getDefaultTeam()))
		{
			case ALLIES:  m_hoverRelation = HOVER_ALLY; break;
			case ENEMIES: m_hoverRelation = HOVER_ENEMY; break;
			default:      m_hoverRelation = HOVER_NEUTRAL; break;
		}
	}

	m_hoverDrawableID = (UnsignedInt)draw->getID();
	m_hoverAssisted = assisted;
	m_hoverName = thingTemplate->getDisplayName();
	if (m_hoverName.isEmpty())
	{
		// Same fallback as the native tooltip, using the real template name for the string lookup.
		AsciiString label;
		label.format("ThingTemplate:%s", obj->getTemplate()->getName().str());
		m_hoverName = TheGameText->fetch(label);
	}
}

//-------------------------------------------------------------------------------------------------
// Drawing
//-------------------------------------------------------------------------------------------------
DisplayString *GameController::makeString()
{
	if (!TheDisplayStringManager)
		return nullptr;
	return TheDisplayStringManager->newDisplayString();
}

void GameController::ensureFont()
{
	// adjustFontSize already scales for the display resolution, like other HUD text, so the base
	// size is fixed here. Scaling it again would make the text too large on big screens.
	const Int baseSize = 9;
	const Int pointSize = TheGlobalLanguageData ? TheGlobalLanguageData->adjustFontSize(baseSize) : baseSize;
	if (!m_font || m_fontSize != pointSize)
	{
		m_font = TheWindowManager->winFindFont("Tahoma", pointSize, TRUE);
		m_fontSize = pointSize;
	}
}

Int GameController::textLineHeight()
{
	if (!TheWindowManager)
		return 14;
	ensureFont();
	return (m_font && m_font->height > 0) ? m_font->height + 2 : 14;
}

void GameController::drawText(DisplayString *str, const UnicodeString &text, Int x, Int y, UnsignedInt color)
{
	if (!str || !TheWindowManager || !TheDisplay)
		return;

	ensureFont();
	if (str->getFont() != m_font)
		str->setFont(m_font);
	// With another button layout, the texts name the button that now does each thing.
	const UnicodeString shown = m_textRemapOff ? text : buttonNamesFor(text);
	if (str->getText() != shown)
		str->setText(shown);
	str->draw(x, y, color, GameMakeColor(0, 0, 0, 200));
}

/// The animated Rangefinder reticle, or the original one with the ClassicReticle setting. Both
/// only draw; the aim point and everything the buttons do are the same with either.
void GameController::drawReticle(Int cx, Int cy, Bool active)
{
	if (m_settings.classicReticle)
		drawClassicReticle(cx, cy, active);
	else
		drawRangefinder(cx, cy, active);
}

void GameController::drawClassicReticle(Int cx, Int cy, Bool active)
{
	const Real scale = TheDisplay->getHeight() / FONT_REFERENCE_HEIGHT;
	const Int arm = REAL_TO_INT(12.0f * scale) < 6 ? 6 : REAL_TO_INT(12.0f * scale);
	const Int gap = REAL_TO_INT(5.0f * scale) < 3 ? 3 : REAL_TO_INT(5.0f * scale);
	const Real width = scale < 1.0f ? 1.5f : 2.0f * scale;
	const UnsignedByte alpha = active ? 230 : 80;

	UnsignedInt color = GameMakeColor(255, 255, 255, alpha);
	switch (m_hoverRelation)
	{
		case HOVER_OWN:     color = GameMakeColor(80, 255, 80, alpha); break;
		case HOVER_ALLY:    color = GameMakeColor(80, 200, 255, alpha); break;
		case HOVER_NEUTRAL: color = GameMakeColor(255, 230, 80, alpha); break;
		case HOVER_ENEMY:   color = GameMakeColor(255, 70, 60, alpha); break;
		default: break;
	}
	const UnsignedInt shadow = GameMakeColor(0, 0, 0, active ? 160 : 60);

	// Crosshair with a centre gap, drawn over a thin dark shadow so it reads on bright terrain.
	for (Int pass = 0; pass < 2; ++pass)
	{
		const UnsignedInt c = pass == 0 ? shadow : color;
		const Int o = pass == 0 ? 1 : 0;
		TheDisplay->drawLine(cx - gap - arm + o, cy + o, cx - gap + o, cy + o, width, c);
		TheDisplay->drawLine(cx + gap + o, cy + o, cx + gap + arm + o, cy + o, width, c);
		TheDisplay->drawLine(cx + o, cy - gap - arm + o, cx + o, cy - gap + o, width, c);
		TheDisplay->drawLine(cx + o, cy + gap + o, cx + o, cy + gap + arm + o, width, c);
	}

	// Hovering something: add corner brackets, so the state is shown by shape and not only colour.
	if (m_hoverRelation != HOVER_NONE && active)
	{
		// Brackets go on the actual candidate; with target assist it is not under the crosshair.
		Int bx = cx;
		Int by = cy;
		if (m_hoverAssisted)
		{
			Drawable *candidate = TheGameClient->findDrawableByID((DrawableID)m_hoverDrawableID);
			ICoord2D s;
			if (candidate && TheTacticalView->worldToScreen(candidate->getPosition(), &s))
			{
				bx = s.x;
				by = s.y;
			}
		}
		const Int b = gap + arm + REAL_TO_INT(6.0f * scale);
		const Int l = REAL_TO_INT(7.0f * scale) < 4 ? 4 : REAL_TO_INT(7.0f * scale);
		TheDisplay->drawLine(bx - b, by - b, bx - b + l, by - b, width, color);
		TheDisplay->drawLine(bx - b, by - b, bx - b, by - b + l, width, color);
		TheDisplay->drawLine(bx + b, by - b, bx + b - l, by - b, width, color);
		TheDisplay->drawLine(bx + b, by - b, bx + b, by - b + l, width, color);
		TheDisplay->drawLine(bx - b, by + b, bx - b + l, by + b, width, color);
		TheDisplay->drawLine(bx - b, by + b, bx - b, by + b - l, width, color);
		TheDisplay->drawLine(bx + b, by + b, bx + b - l, by + b, width, color);
		TheDisplay->drawLine(bx + b, by + b, bx + b, by + b - l, width, color);
	}
}

void GameController::drawWorldOverlay()
{
	if (!m_settings.enabled || !m_state.connected || m_suspended || !TheDisplay || !isBattlefieldContext())
		return;

	// An open wheel sits on top of the reticle; its labels would only show through the middle.
	if (m_panelOpen && m_settings.wheelMenus)
		return;

	Real rx, ry;
	getReticleScreenPos(&rx, &ry);
	const Int cx = REAL_TO_INT(rx);
	const Int cy = REAL_TO_INT(ry);
	const Bool active = isActiveInput();

	// Map edge offset active: a faint marker at the centre shows where the reticle will return to.
	if (active && (m_reticleOffset.x != 0.0f || m_reticleOffset.y != 0.0f))
	{
		Int ox = 0, oy = 0;
		TheTacticalView->getOrigin(&ox, &oy);
		const Int ax = ox + TheTacticalView->getWidth() / 2;
		const Int ay = oy + TheTacticalView->getHeight() / 2;
		const UnsignedInt faint = GameMakeColor(255, 255, 255, 70);
		TheDisplay->drawOpenRect(ax - 3, ay - 3, 7, 7, 1.0f, faint);
		TheDisplay->drawLine(ax, ay, cx, cy, 1.0f, GameMakeColor(255, 255, 255, 35));
	}

	if (active)
		drawLineMove();
	drawReticle(cx, cy, active);
	if (active)
		drawBrush(cx, cy);

	// Name of what is under the reticle. Text as well as colour, per the spec.
	if (active)
		drawModeHint(cx, cy);

	if (active && m_hoverRelation != HOVER_NONE && !isPlacementMode())
	{
		if (!m_hoverString)
			m_hoverString = makeString();
		UnicodeString label = m_hoverName;
		switch (m_hoverRelation)
		{
			case HOVER_OWN:     label.concat(L"  (yours)"); break;
			case HOVER_ALLY:    label.concat(L"  (ally)"); break;
			case HOVER_NEUTRAL: label.concat(L"  (neutral)"); break;
			case HOVER_ENEMY:   label.concat(L"  (enemy)"); break;
			default: break;
		}
		const Int offsetY = REAL_TO_INT(30.0f * TheDisplay->getHeight() / FONT_REFERENCE_HEIGHT);
		drawText(m_hoverString, label, cx + offsetY, cy + offsetY, GameMakeColor(255, 255, 255, 230));
	}

	if (active)
		drawSelectionInfo(cx, cy);
}

void GameController::drawHelp()
{
	static const char *const helpLines[] =
	{
		"CONTROLLER HELP - Zero Hour Controller Mod " ZH_CONTROLLER_MOD_VERSION,
		"Left stick: move camera   LT: faster   Right stick: rotate / zoom   R-stick click: reset",
		"A: select (a building opens its wheel)   A A: all of that type on screen   Hold A: paint select",
		"X: order   X X: guard there   Hold X + pan: line (L-stick click while holding: guard line)   B: cancel",
		"LB: whole army   RB: army on screen   LB+RB together: unload vehicles/buildings   RT: cycle unit types",
		"Y: command wheel - stick points, A uses, X or LB: one out of the queue (hold: all); 2 wheels: L-stick click uses",
		"D-pad up: powers wheel (D-pad down in it: promotions)   D-pad down: groups wheel (Y there assigns)",
		"D-pad left: jump to your bases   D-pad right: jump to recent alerts",
		"Aiming a command or building: A/X confirm, LT slow, RT fast, RS rotates a building, B back",
		"Menus: D-pad moves, A selects, B back, Start: Play Game, View: controller settings",
		"Menu: game menu   View or B: close this help   Y: controller settings (buttons, camera, sticks, timing)",
		"Multiplayer, with this help open: A: chat to everyone   X: chat to allies",
		"Buttons lost? Hold both stick clicks for 3 s, or press Ctrl+Shift+F12, for the default buttons"
	};
	const Int count = (Int)(sizeof(helpLines) / sizeof(helpLines[0]));

	const Int lineHeight = textLineHeight() + 4;
	const Int pad = lineHeight / 2;

	// Size the box to the widest line.
	Int widest = 0;
	for (Int i = 0; i < count && i < MAX_TEXT_LINES; ++i)
	{
		if (!m_lines[i])
			m_lines[i] = makeString();
		if (!m_lines[i])
			return;
		m_lines[i]->setFont(m_font);
		m_lines[i]->setText(toUnicode(helpLines[i]));
		if (m_lines[i]->getWidth() > widest)
			widest = m_lines[i]->getWidth();
	}

	const Int boxW = widest + pad * 2;
	const Int boxH = lineHeight * count + pad * 2;
	const Int boxX = (TheDisplay->getWidth() - boxW) / 2;
	const Int boxY = REAL_TO_INT(TheDisplay->getHeight() * 0.18f);

	TheDisplay->drawFillRect(boxX, boxY, boxW, boxH, GameMakeColor(0, 0, 0, 190));
	TheDisplay->drawOpenRect(boxX, boxY, boxW, boxH, 1.0f, GameMakeColor(255, 255, 255, 120));

	for (Int i = 0; i < count && i < MAX_TEXT_LINES; ++i)
	{
		const UnsignedInt color = i == 0 ? GameMakeColor(255, 220, 120, 255) : GameMakeColor(235, 235, 235, 255);
		drawText(m_lines[i], toUnicode(helpLines[i]), boxX + pad, boxY + pad + i * lineHeight, color);
	}
}

void GameController::drawDiagnostics()
{
	// Internal state, not button prompts: shown as it is.
	m_textRemapOff = TRUE;
	// Left edge, below the area the game uses for its own messages.
	const Int lineHeight = textLineHeight();
	const Int x = 8;
	Int y = REAL_TO_INT(TheDisplay->getHeight() * 0.30f);

	char buf[256];
	UnicodeString text;
	Int line = 0;

	const Coord3D &pos = TheTacticalView->getPosition();

	snprintf(buf, sizeof(buf), "%s | %s slot %d  %s%s%s%s",
		CONTROLLER_MOD_BUILD,
		m_device ? m_device->getBackendName() : "none",
		m_device ? m_device->getActiveSlot() : -1,
		m_state.connected ? "connected" : "disconnected",
		isActiveInput() ? " ACTIVE" : " (mouse)",
		m_cursorLocked ? " cursor-locked" : "",
		m_needNeutral ? " release-all-buttons" :
			(m_needStickNeutral[0] && m_needStickNeutral[1]) ? " release-sticks" :
			m_needStickNeutral[0] ? " release-left-stick" :
			m_needStickNeutral[1] ? " release-right-stick" : "");
	text = toUnicode(buf);
	drawText(m_lines[line] ? m_lines[line] : (m_lines[line] = makeString()), text, x, y, GameMakeColor(200, 255, 200, 255));
	y += lineHeight; ++line;

	snprintf(buf, sizeof(buf), "LS raw %+.2f %+.2f -> pan %+.2f %+.2f   RS raw %+.2f %+.2f -> %+.2f %+.2f   LT %.2f RT %.2f",
		m_state.leftX, m_state.leftY, m_panX, m_panY, m_state.rightX, m_state.rightY, m_rotX, m_rotY,
		m_state.leftTrigger, m_state.rightTrigger);
	text = toUnicode(buf);
	drawText(m_lines[line] ? m_lines[line] : (m_lines[line] = makeString()), text, x, y, GameMakeColor(220, 220, 220, 255));
	y += lineHeight; ++line;

	snprintf(buf, sizeof(buf), "Cam %.0f %.0f %.0f  yaw %.1f  height %.0f  zoom %.2f  dt %.1f ms  scroll scale %.3f/%.3f",
		pos.x, pos.y, pos.z, TheTacticalView->getAngle() * 180.0f / PI, TheTacticalView->getHeightAboveGround(),
		TheTacticalView->getZoom(), m_lastDt * 1000.0f, m_worldPerScrollX, m_worldPerScrollY);
	text = toUnicode(buf);
	drawText(m_lines[line] ? m_lines[line] : (m_lines[line] = makeString()), text, x, y, GameMakeColor(220, 220, 220, 255));
	y += lineHeight; ++line;

	if (m_hoverHasGround)
		snprintf(buf, sizeof(buf), "Reticle offset %+.0f %+.0f px  ground %.0f %.0f %.0f  hover id %u",
			m_reticleOffset.x, m_reticleOffset.y, m_hoverGround.x, m_hoverGround.y, m_hoverGround.z, m_hoverDrawableID);
	else
		snprintf(buf, sizeof(buf), "Reticle offset %+.0f %+.0f px  ground: NO TERRAIN HIT  hover id %u",
			m_reticleOffset.x, m_reticleOffset.y, m_hoverDrawableID);
	text = toUnicode(buf);
	drawText(m_lines[line] ? m_lines[line] : (m_lines[line] = makeString()), text, x, y, GameMakeColor(220, 220, 220, 255));
	y += lineHeight; ++line;

	static const char *const gestureNames[] = { "idle", "pressed", "brush" };
	static const char *const panelNames[] = { "commands", "powers", "groups" };
	static const char *const orderModeNames[] = { "", " FORCE-ATTACK", " WAYPOINTS" };
	std::vector<PanelEntry> barCommands, barQueue, powers;
	collectPanelEntries(PANEL_COMMANDS, REGION_COMMANDS, &barCommands);
	collectPanelEntries(PANEL_COMMANDS, REGION_QUEUE, &barQueue);
	collectPanelEntries(PANEL_POWERS, REGION_POWERS_USE, &powers);
	snprintf(buf, sizeof(buf), "A %s  brush %d  cohort %d  type %d  hint %d  assisted %d  selected %d  bar %d cmds %d queue %d powers  panel %s/%d%s%s%s",
		gestureNames[m_gesture], (Int)m_brushIDs.size(), (Int)m_cohort.size(), m_typeIndex, m_orderHint,
		(Int)m_hoverAssisted, TheInGameUI->getSelectCount(), (Int)barCommands.size(), (Int)barQueue.size(), (Int)powers.size(),
		(m_panelOpen && m_panelKind >= 0) ? panelNames[m_panelKind] : "closed", m_panelRegion,
		isPlacementMode() ? " PLACING" : "", isTargetingMode() ? " TARGETING" : "", orderModeNames[m_orderMode]);
	text = toUnicode(buf);
	drawText(m_lines[line] ? m_lines[line] : (m_lines[line] = makeString()), text, x, y, GameMakeColor(220, 220, 220, 255));
	m_textRemapOff = FALSE;
}

void GameController::drawScreenOverlay()
{
	applyCursorHiding();

	if (!m_settings.enabled || !m_state.connected || m_suspended || !TheDisplay)
		return;

	if (m_settingsOpen)
	{
		drawSettingsScreen();
		return;
	}
	drawSettingsMessage();

	// The game's own menus: the focus frame and what the buttons do (or the on-screen keyboard).
	if (!isBattlefieldContext())
	{
		if (m_keyboardOpen)
		{
			drawKeyboard();
		}
		else if (isMatchChatOpen() && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame())
		{
			// The chat box opened with the Enter key: say how to type with the controller.
			if (!m_menuLegend)
				m_menuLegend = makeString();
			drawText(m_menuLegend, toUnicode("A: type with the controller   B: close the chat"), 8,
				(Int)TheDisplay->getHeight() - textLineHeight() - 4, GameMakeColor(255, 220, 120, 255));
		}
		else
		{
			drawMenuFocus();
		}
		return;
	}

	drawPanelFocus();

	if (m_helpVisible)
		drawHelp();
	else if (m_settings.showDiagnostics && !(m_panelOpen && m_settings.wheelMenus))   // not over a wheel
		drawDiagnostics();
}
