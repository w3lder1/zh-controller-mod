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

// GameControllerCommandPanel.cpp /////////////////////////////////////////////////////////////////
// ControllerMod @feature The controller's panels, targeting and building
// placement.
//
//   Y              focus the command bar of the current selection (Y or B closes it)
//   D-pad up       focus the Generals Powers: the power buttons, or the promotion window
//   D-pad down     the control groups (GameControllerPanels.cpp)
//   LB/RB, D-pad   move the focus; D-pad up/down switches between the regions of a panel
//                  (Commands / Build queue / Orders, Powers / Promotions)
//   A              use the focused button, exactly like clicking it
//   X              in the build queue: cancel the focused item (native cancel and refund)
//   Targeting      a command that needs a target arms the native GUI command; A or X confirms
//                  at the reticle, B goes back to the panel it came from
//   Placement      a build command shows the native building ghost at the reticle; the right
//                  stick rotates it, LT moves precisely, A or X places, B goes back
//
// The command bar is never re-implemented. Buttons are the real game windows and are "pressed"
// with the same GBM_SELECTED message a mouse click sends, so costs, prerequisites, queues, sounds
// and every command type behave natively, including mods and other layouts. Targeting and
// placement are confirmed with the same cooked click message the mouse produces, sent only when
// the native translator that owns that mode will consume it.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameController.h"
#include "GameClient/ControllerMath.h"

#include "Common/AudioEventRTS.h"
#include "Common/BuildAssistant.h"
#include "Common/GameAudio.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Science.h"
#include "Common/ThingTemplate.h"
#include "GameClient/Color.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/Eva.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"

namespace
{
	const UnsignedInt PANEL_FEEDBACK_MS = 1500;
	const UnsignedInt COMMAND_CHANGED_GUARD_MS = 400;  ///< a button that changed by itself this recently is not pressed
	const UnsignedInt SELL_CONFIRM_MS = 2500;

	enum
	{
		COMMAND_SLOTS = MAX_COMMANDS_PER_SET,
		QUEUE_SLOTS = MAX_BUILD_QUEUE_BUTTONS,
		CANCEL_CONSTRUCTION_SLOT = 100   // sorts after the regular command slots
	};

	UnicodeString toUnicodeText(const char *text)
	{
		UnicodeString u;
		u.translate(AsciiString(text));
		return u;
	}

	/// Removes the '&' hotkey marker from a button label, using the same rule as the button text
	/// renderer (an '&' followed by a printable character marks that character as the hotkey).
	UnicodeString withoutHotkeyMarker(const UnicodeString &label)
	{
		UnicodeString result;
		const WideChar *text = label.str();
		for (; text && *text; ++text)
		{
			if (*text == L'&' && text[1] > L' ')
				continue;
			result.concat(*text);
		}
		return result;
	}

	/// The display name of a command button, as the command bar tooltip uses it.
	UnicodeString commandName(const CommandButton *command)
	{
		if (!command)
			return UnicodeString::TheEmptyString;
		if (command->getTextLabel().isNotEmpty())
			return withoutHotkeyMarker(TheGameText->fetch(command->getTextLabel()));
		// Promotions have no label of their own; the tooltip names them after their science.
		if (command->getCommandType() == GUI_COMMAND_PURCHASE_SCIENCE && !command->getScienceVec().empty() && TheScienceStore)
		{
			UnicodeString name, description;
			if (TheScienceStore->getNameAndDescription(command->getScienceVec()[0], name, description) && !name.isEmpty())
				return name;
		}
		return toUnicodeText(command->getName().str());
	}

	/// Commands that GUICommandTranslator completes on a left click at the target. Anything else
	/// (context commands, special powers) is completed by the context evaluator.
	Bool isCompletedByGuiTranslator(const CommandButton *command)
	{
		if (!command || command->isContextCommand())
			return FALSE;
		switch (command->getCommandType())
		{
			case GUI_COMMAND_FIRE_WEAPON:
			case GUI_COMMAND_EVACUATE:
			case GUI_COMMAND_GUARD:
			case GUI_COMMAND_GUARD_WITHOUT_PURSUIT:
			case GUI_COMMAND_GUARD_FLYING_UNITS_ONLY:
			case GUI_COMMAND_ATTACK_MOVE:
			case GUI_COMMAND_SET_RALLY_POINT:
			case GUICOMMANDMODE_PLACE_BEACON:
				return TRUE;
			default:
				return FALSE;
		}
	}

	const char *regionName(Int kind, Int region)
	{
		if (kind == 0)   // PANEL_COMMANDS
			return region == 0 ? "Commands" : (region == 1 ? "Build queue" : "Orders");
		return region == 0 ? "Powers" : "Promotions";
	}
}

//-------------------------------------------------------------------------------------------------
// Window helpers
//-------------------------------------------------------------------------------------------------
GameWindow *GameController::findGameWindow(const char *parentName, const char *name)
{
	if (!TheWindowManager)
		return nullptr;
	GameWindow *parent = TheWindowManager->winGetWindowFromId(nullptr, NAMEKEY(parentName));
	if (!parent)
		return nullptr;
	return TheWindowManager->winGetWindowFromId(parent, NAMEKEY(name));
}

/// A window counts as shown only if it and all of its parents are visible.
Bool GameController::isWindowShown(GameWindow *win)
{
	for (GameWindow *w = win; w; w = w->winGetParent())
	{
		if (w->winIsHidden())
			return FALSE;
	}
	return win != nullptr;
}

/// A button the player could click right now: shown, and carrying a command.
Bool GameController::isUsableCommandButton(GameWindow *win)
{
	return win && isWindowShown(win) && win->winGetInputFunc() == GadgetPushButtonInput &&
		GadgetButtonGetData(win) != nullptr;
}

Int GameController::panelRegionCount(Int kind)
{
	switch (kind)
	{
		case PANEL_COMMANDS: return 3;
		case PANEL_POWERS:   return 2;
		default:             return 1;
	}
}

//-------------------------------------------------------------------------------------------------
// Mode queries
//-------------------------------------------------------------------------------------------------
Bool GameController::isPlacementMode() const
{
	return TheInGameUI && TheInGameUI->getPendingPlaceType() != nullptr;
}

Bool GameController::isTargetingMode() const
{
	return TheInGameUI && TheInGameUI->getGUICommand() != nullptr && !isPlacementMode();
}

Bool GameController::isDrivingPlacement() const
{
	return isActiveInput() && isPlacementMode() && isBattlefieldContext();
}

Bool GameController::getPlacementCursor(ICoord2D *pixel) const
{
	if (!pixel || !isDrivingPlacement())
		return FALSE;
	Real rx, ry;
	getReticleScreenPos(&rx, &ry);
	pixel->x = REAL_TO_INT(rx);
	pixel->y = REAL_TO_INT(ry);
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** The same cooked click MetaEventTranslator makes from a mouse release: a point region and no
	modifier keys. Only called when PlaceEventTranslator (anchored placement) or
	GUICommandTranslator (armed command) will consume it, so it can never fall through into a
	selection or an ordinary order. */
//-------------------------------------------------------------------------------------------------
void GameController::sendClickAtReticle()
{
	Real rx, ry;
	getReticleScreenPos(&rx, &ry);
	IRegion2D region;
	region.lo.x = region.hi.x = REAL_TO_INT(rx);
	region.lo.y = region.hi.y = REAL_TO_INT(ry);

	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_MOUSE_LEFT_CLICK);
	msg->appendPixelRegionArgument(region);
	msg->appendIntegerArgument(0);
}

void GameController::showFeedback(const char *text)
{
	showFeedback(toUnicodeText(text));
}

void GameController::showFeedback(const UnicodeString &text)
{
	m_orderFeedback = text;
	m_orderFeedbackUntilMs = timeGetTime() + PANEL_FEEDBACK_MS;
}

//-------------------------------------------------------------------------------------------------
// Panels
//-------------------------------------------------------------------------------------------------
void GameController::collectPanelEntries(Int kind, Int region, std::vector<PanelEntry> *out) const
{
	out->clear();
	if (!TheWindowManager)
		return;

	if (kind == PANEL_POWERS)
	{
		collectPowerEntries(region, out);
		return;
	}
	if (kind == PANEL_GROUPS)
	{
		for (Int i = 0; i < NUM_HOTKEY_SQUADS; ++i)
		{
			PanelEntry e;
			e.window = nullptr;
			e.slot = i;
			out->push_back(e);
		}
		return;
	}
	if (region == REGION_ORDERS)
	{
		collectOrderEntries(out);
		return;
	}

	AsciiString name;
	if (region == REGION_COMMANDS)
	{
		for (Int i = 0; i < COMMAND_SLOTS; ++i)
		{
			name.format("ControlBar.wnd:ButtonCommand%02d", i + 1);
			GameWindow *win = findGameWindow("ControlBar.wnd:CommandWindow", name.str());
			if (isUsableCommandButton(win))
			{
				PanelEntry e;
				e.window = win;
				e.slot = i;
				out->push_back(e);
			}
		}
		GameWindow *cancel = findGameWindow("ControlBar.wnd:UnderConstructionWindow", "ControlBar.wnd:ButtonCancelConstruction");
		if (isUsableCommandButton(cancel))
		{
			PanelEntry e;
			e.window = cancel;
			e.slot = CANCEL_CONSTRUCTION_SLOT;
			out->push_back(e);
		}
	}
	else
	{
		for (Int i = 0; i < QUEUE_SLOTS; ++i)
		{
			name.format("ControlBar.wnd:ButtonQueue%02d", i + 1);
			GameWindow *win = findGameWindow("ControlBar.wnd:ProductionQueueWindow", name.str());
			if (isUsableCommandButton(win))
			{
				PanelEntry e;
				e.window = win;
				e.slot = i;
				out->push_back(e);
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** The focused entry. Focus is remembered by slot, so it stays on the same button while the bar
	updates; if that button disappears, the nearest surviving slot takes over. */
//-------------------------------------------------------------------------------------------------
Bool GameController::focusedPanelEntry(PanelEntry *entry, Int *indexOut) const
{
	if (indexOut)
		*indexOut = -1;
	if (m_panelKind < 0)
		return FALSE;

	// A wheel only has a focus once the player points at a sector, and only that exact one.
	if (m_settings.wheelMenus)
		return wheelFocusedEntry(entry, indexOut);

	std::vector<PanelEntry> entries;
	collectPanelEntries(m_panelKind, m_panelRegion, &entries);
	if (entries.empty())
		return FALSE;

	const Int wanted = m_panelSlot[m_panelKind][m_panelRegion];
	Int best = 0;
	Int bestDistance = 1 << 30;
	for (size_t i = 0; i < entries.size(); ++i)
	{
		const Int d = abs(entries[i].slot - wanted);
		if (d < bestDistance)
		{
			bestDistance = d;
			best = (Int)i;
		}
	}
	if (indexOut)
		*indexOut = best;
	if (entry)
		*entry = entries[best];
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** The player moved the focus: whatever is on the focused button now is what they chose to look
	at, so it is not treated as a button that changed by itself. */
//-------------------------------------------------------------------------------------------------
void GameController::noteFocusMovedByPlayer()
{
	PanelEntry entry;
	m_focusCommand = nullptr;
	if (focusedPanelEntry(&entry, nullptr) && entry.window)
		m_focusCommand = (const CommandButton *)GadgetButtonGetData(entry.window);
	m_focusChangedMs = 0;
}

//-------------------------------------------------------------------------------------------------
/** True when the command on the focused entry is one the player has had time to see. A button
	that changed by itself (an upgrade, a prerequisite, a finished queue item) is refused for a
	moment. This runs right before a button acts: updatePanel only notices a change after this
	frame's buttons, and the command bar is refreshed after the wheel is drawn, so without it the
	first frame after a change could use a command that was never on screen. */
//-------------------------------------------------------------------------------------------------
Bool GameController::isFocusedCommandSeen(const PanelEntry &entry, UnsignedInt nowMs)
{
	if (!entry.window)
		return TRUE;
	const CommandButton *command = (const CommandButton *)GadgetButtonGetData(entry.window);
	if (command != m_focusCommand)
	{
		m_focusCommand = command;
		m_focusChangedMs = nowMs;
	}
	return m_focusChangedMs == 0 || nowMs - m_focusChangedMs >= COMMAND_CHANGED_GUARD_MS;
}

void GameController::openPanel(Int kind)
{
	// The first region with something in it; the Powers panel starts on Promotions when there
	// are points to spend.
	Int region = -1;
	for (Int r = 0; r < panelRegionCount(kind); ++r)
	{
		if (isPanelRegionAvailable(kind, r))
		{
			region = r;
			break;
		}
	}
	if (kind == PANEL_POWERS)
	{
		Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		if (player && player->getSciencePurchasePoints() > 0 && isPanelRegionAvailable(kind, REGION_POWERS_PROMOTE))
			region = REGION_POWERS_PROMOTE;
	}
	if (region < 0)
	{
		if (kind == PANEL_POWERS)
			showFeedback("No generals powers yet");
		else
			showFeedback(TheInGameUI->getSelectCount() == 0 ? "Select something first" : "No commands for this selection");
		return;
	}

	cancelGesture();
	m_panelOpen = TRUE;
	m_panelKind = kind;
	m_panelRegion = region;
	m_repeatButton = 0;
	m_groupClearPending = -1;
	m_sellConfirmUntilMs = 0;
	resetWheel();
	setScienceWindowShown(kind == PANEL_POWERS && region == REGION_POWERS_PROMOTE);
	noteFocusMovedByPlayer();
	beginInputContext();
}

void GameController::closePanel()
{
	if (!m_panelOpen)
		return;
	m_panelOpen = FALSE;
	m_repeatButton = 0;
	m_groupClearPending = -1;
	// Leaving the powers panel leaves the promotion window too, whoever opened it.
	if (m_panelKind == PANEL_POWERS)
		closeScienceWindow();
	else
		setScienceWindowShown(FALSE);
	beginInputContext();
	// The left stick pans at once, even if it is still held from pointing at the wheel. The right
	// stick still has to be let go first, so a stick left on the right wheel
	// does not spin the camera.
	m_needStickNeutral[0] = FALSE;
}

//-------------------------------------------------------------------------------------------------
/** Bumpers or D-pad left/right step the focus; holding repeats after a short delay.
	Returns -1, 0 or +1. */
//-------------------------------------------------------------------------------------------------
Int GameController::panelStepFromButtons(UnsignedInt pressed, UnsignedInt held, UnsignedInt nowMs)
{
	const UnsignedInt prevButtons = ControllerState::BUTTON_LB | ControllerState::BUTTON_DPAD_LEFT;
	const UnsignedInt nextButtons = ControllerState::BUTTON_RB | ControllerState::BUTTON_DPAD_RIGHT;
	if (pressed & (prevButtons | nextButtons))
	{
		m_repeatButton = pressed & (prevButtons | nextButtons);
		m_repeatNextMs = nowMs + (UnsignedInt)m_settings.repeatDelayMs;
		return (pressed & prevButtons) ? -1 : 1;
	}
	if (m_repeatButton && (held & m_repeatButton))
	{
		if ((Int)(nowMs - m_repeatNextMs) >= 0)
		{
			m_repeatNextMs = nowMs + (UnsignedInt)m_settings.repeatRateMs;
			return (m_repeatButton & prevButtons) ? -1 : 1;
		}
	}
	else
	{
		m_repeatButton = 0;
	}
	return 0;
}

void GameController::stepPanelFocus(Int delta)
{
	std::vector<PanelEntry> entries;
	collectPanelEntries(m_panelKind, m_panelRegion, &entries);
	if (entries.empty())
		return;

	Int index = 0;
	focusedPanelEntry(nullptr, &index);
	if (index < 0)
		index = 0;
	const Int count = (Int)entries.size();
	index = ((index + delta) % count + count) % count;   // wraps around, like reading a row
	m_panelSlot[m_panelKind][m_panelRegion] = entries[index].slot;
	noteFocusMovedByPlayer();
}

void GameController::switchPanelRegion(Int delta)
{
	const Int count = panelRegionCount(m_panelKind);
	for (Int step = 1; step < count; ++step)
	{
		const Int region = ((m_panelRegion + delta * step) % count + count) % count;
		if (isPanelRegionAvailable(m_panelKind, region))
		{
			m_panelRegion = region;
			resetWheel();
			setScienceWindowShown(m_panelKind == PANEL_POWERS && region == REGION_POWERS_PROMOTE);
			noteFocusMovedByPlayer();
			return;
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** "Click" a game button: the same checks, sound and message as a real mouse click on it
	(GadgetPushButtonInput -> owner system callback -> ControlBar::processContextSensitiveButtonClick). */
//-------------------------------------------------------------------------------------------------
void GameController::activatePanelWindow(GameWindow *win)
{
	if (!win)
		return;

	// A disabled button ignores clicks natively; say why instead of doing nothing.
	if (!BitIsSet(win->winGetStatus(), WIN_STATUS_ENABLED))
	{
		showFeedback("Not available right now");
		static AudioEventRTS noCanDo("NoCanDoSound");
		if (TheAudio)
			TheAudio->addAudioEvent(&noCanDo);
		return;
	}

	// Selling several objects at once is irreversible: ask for a second press first.
	const CommandButton *command = (const CommandButton *)GadgetButtonGetData(win);
	const Int selected = TheInGameUI->getSelectCount();
	if (command && command->getCommandType() == GUI_COMMAND_SELL && selected > 1)
	{
		const UnsignedInt now = timeGetTime();
		if (m_sellConfirmUntilMs == 0 || (Int)(now - m_sellConfirmUntilMs) > 0)
		{
			m_sellConfirmUntilMs = now + SELL_CONFIRM_MS;
			UnicodeString text;
			text.format(L"Sell all %d selected? Press A again to confirm", selected);
			showFeedback(text);
			return;
		}
		m_sellConfirmUntilMs = 0;
	}

	AudioEventRTS buttonClick;
	buttonClick.setEventName("GUIClick");
	if (TheAudio)
		TheAudio->addAudioEvent(&buttonClick);

	TheWindowManager->winSendSystemMsg(win->winGetOwner(), GBM_SELECTED, (WindowMsgData)win, 0);
}

void GameController::activatePanelEntry(const PanelEntry &entry)
{
	if (entry.window)
	{
		activatePanelWindow(entry.window);
		return;
	}
	if (m_panelKind == PANEL_COMMANDS && m_panelRegion == REGION_ORDERS)
		beginOrderMode(entry.slot);
}

//-------------------------------------------------------------------------------------------------
void GameController::handlePanelButtons(UnsignedInt pressed, UnsignedInt held, UnsignedInt nowMs)
{
	if (m_settings.wheelMenus)
	{
		handleWheelButtons(pressed, held, nowMs);
		return;
	}

	if (m_panelKind == PANEL_GROUPS)
	{
		handleGroupPanelButtons(pressed, held, nowMs);
		return;
	}

	if (pressed & (ControllerState::BUTTON_B | ControllerState::BUTTON_Y))
	{
		closePanel();
		return;
	}

	if (pressed & ControllerState::BUTTON_DPAD_UP)
	{
		switchPanelRegion(-1);
		return;
	}
	if (pressed & ControllerState::BUTTON_DPAD_DOWN)
	{
		switchPanelRegion(1);
		return;
	}

	const Int step = panelStepFromButtons(pressed, held, nowMs);
	if (step != 0)
	{
		stepPanelFocus(step);
		return;
	}

	PanelEntry entry;
	if (!focusedPanelEntry(&entry, nullptr))
		return;
	const Bool inQueue = m_panelKind == PANEL_COMMANDS && m_panelRegion == REGION_QUEUE;

	if (pressed & ControllerState::BUTTON_A)
	{
		useFocusedPanelEntry(entry, nowMs);
		return;
	}

	if (pressed & ControllerState::BUTTON_X)
	{
		// X only cancels a queue item the player has visibly focused, never anything else.
		if (inQueue)
		{
			if (!isFocusedCommandSeen(entry, nowMs))
			{
				showFeedback("This item just changed - press X again");
				return;
			}
			activatePanelWindow(entry.window);
		}
		return;
	}
}

//-------------------------------------------------------------------------------------------------
/** A on the focused panel entry or highlighted wheel sector. */
//-------------------------------------------------------------------------------------------------
void GameController::useFocusedPanelEntry(const PanelEntry &entry, UnsignedInt nowMs)
{
	if (m_panelKind == PANEL_COMMANDS && m_panelRegion == REGION_QUEUE)
	{
		showFeedback("Press X to cancel this item");
		return;
	}

	// The button under the focus just changed by itself (an upgrade, a prerequisite, the old
	// button disappeared): the player has not seen the new one yet.
	if (!isFocusedCommandSeen(entry, nowMs))
	{
		showFeedback("This button just changed - press A again");
		return;
	}

	const Int kind = m_panelKind;
	activatePanelEntry(entry);

	// A command that needs a world target, a building site or an order target leaves the
	// panel; B comes back to it.
	if (m_panelOpen && (isPlacementMode() || isTargetingMode() || m_orderMode != ORDERMODE_NONE))
	{
		m_panelOpen = FALSE;
		setScienceWindowShown(FALSE);
		m_returnPanel = kind;
		m_repeatButton = 0;
		beginInputContext();
		// The left stick moves the reticle or building at once; the right stick,
		// which turns a building, still has to be let go first.
		m_needStickNeutral[0] = FALSE;
	}
}

/// The name a panel entry shows: the command's own name, the group, or the order.
UnicodeString GameController::panelEntryName(const PanelEntry &entry) const
{
	if (entry.window)
		return commandName((const CommandButton *)GadgetButtonGetData(entry.window));
	if (m_panelKind == PANEL_GROUPS)
	{
		UnicodeString text;
		text.format(L"Group %d", (entry.slot + 1) % NUM_HOTKEY_SQUADS);
		return text;
	}
	return toUnicodeText(entry.slot == ORDERMODE_FORCE_ATTACK ? "Force attack" : "Waypoints");
}

//-------------------------------------------------------------------------------------------------
/** Per frame while a panel is open: keep it valid and show the native tooltip for the focus. */
//-------------------------------------------------------------------------------------------------
void GameController::updatePanel()
{
	if (!m_panelOpen)
		return;

	// Something else took over: a mouse click armed a command or started a placement.
	if (isPlacementMode() || isTargetingMode())
	{
		m_panelOpen = FALSE;
		setScienceWindowShown(FALSE);
		m_returnPanel = m_panelKind;
		return;
	}

	if (m_panelKind == PANEL_GROUPS)
		return;

	// Wheels keep the promotion window hidden and read its buttons; refresh them now and then so
	// availability follows purchases and rank.
	if (m_settings.wheelMenus && m_panelKind == PANEL_POWERS && m_panelRegion == REGION_POWERS_PROMOTE)
	{
		const UnsignedInt now = timeGetTime();
		if (now - m_promotionRefreshMs > 500)
		{
			refreshPromotionButtons();
			m_promotionRefreshMs = now;
		}
	}

	// The promotion window was closed another way (its own Done button, Esc): leave Promotions.
	if (!m_settings.wheelMenus && m_panelKind == PANEL_POWERS && m_panelRegion == REGION_POWERS_PROMOTE && !isScienceWindowShown())
	{
		m_scienceShownByUs = FALSE;
		if (isPanelRegionAvailable(PANEL_POWERS, REGION_POWERS_USE))
		{
			m_panelRegion = REGION_POWERS_USE;
			noteFocusMovedByPlayer();
		}
		else
		{
			closePanel();
			return;
		}
	}

	if (!isPanelRegionAvailable(m_panelKind, m_panelRegion))
	{
		switchPanelRegion(1);
		if (!isPanelRegionAvailable(m_panelKind, m_panelRegion))
		{
			// The selection is gone or has no commands any more.
			closePanel();
			return;
		}
	}

	PanelEntry entry;
	if (!focusedPanelEntry(&entry, nullptr))
	{
		// A single wheel's highlighted entry is gone: nothing stays highlighted, so A cannot hit
		// a different entry that moved into its place. (Two wheels are aimed live.)
		m_wheelHighlight = FALSE;
		return;
	}

	const CommandButton *command = entry.window ? (const CommandButton *)GadgetButtonGetData(entry.window) : nullptr;
	if (command != m_focusCommand)
	{
		m_focusCommand = command;
		m_focusChangedMs = timeGetTime();
	}

	// The command bar tooltip stays up while it is requested every frame, as mouse hover does.
	if (entry.window && TheControlBar)
		TheControlBar->showBuildTooltipLayout(entry.window);
}

//-------------------------------------------------------------------------------------------------
// Targeting
//-------------------------------------------------------------------------------------------------
void GameController::handleTargetButtons(UnsignedInt pressed)
{
	if (pressed & ControllerState::BUTTON_B)
	{
		const CommandButton *command = TheInGameUI->getGUICommand();
		const Bool generalsPower = m_returnPanel == PANEL_POWERS || (command &&
			(command->getCommandType() == GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT ||
			 command->getCommandType() == GUI_COMMAND_SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT));
		TheInGameUI->setGUICommand(nullptr);
		if (generalsPower)
			cancelPowerToBattlefield();
		else
			returnToPanelIfPossible();
		return;
	}
	// A and X are the same confirmation; pressed together they confirm once.
	if (pressed & (ControllerState::BUTTON_A | ControllerState::BUTTON_X))
		confirmTarget();
}

void GameController::confirmTarget()
{
	const CommandButton *command = TheInGameUI->getGUICommand();
	if (!command)
		return;

	if (isCompletedByGuiTranslator(command))
	{
		sendClickAtReticle();
		m_checkTargetResult = TRUE;
		m_checkTargetCommand = command;
	}
	else
	{
		// Context commands and special powers: the same evaluator a click would reach.
		issueOrder();
		if (TheInGameUI->getGUICommand() == command)
			showFeedback("Not a valid target");
	}
}

//-------------------------------------------------------------------------------------------------
// Placement
//-------------------------------------------------------------------------------------------------
void GameController::handlePlacementButtons(UnsignedInt pressed)
{
	if (pressed & ControllerState::BUTTON_B)
	{
		// Native cancel: nothing is paid until the building is actually placed.
		TheInGameUI->placeBuildAvailable(nullptr, nullptr);
		if (m_returnPanel == PANEL_POWERS)
			cancelPowerToBattlefield();
		else
			returnToPanelIfPossible();
		return;
	}
	if (pressed & (ControllerState::BUTTON_A | ControllerState::BUTTON_X))
		confirmPlacement();
}

//-------------------------------------------------------------------------------------------------
/** Anchor the native placement at the reticle and send the click that commits it. The native
	commit then checks money, legality, shroud and the builder, and gives its own feedback. The
	money check is repeated here first because a failed money check lets the click fall through. */
//-------------------------------------------------------------------------------------------------
void GameController::confirmPlacement()
{
	const ThingTemplate *build = TheInGameUI->getPendingPlaceType();
	Object *builder = TheGameLogic->findObjectByID(TheInGameUI->getPendingPlaceSourceObjectID());
	if (!build || !builder)
	{
		TheInGameUI->placeBuildAvailable(nullptr, nullptr);
		return;
	}

	Real rx, ry;
	getReticleScreenPos(&rx, &ry);
	ICoord2D pixel;
	pixel.x = REAL_TO_INT(rx);
	pixel.y = REAL_TO_INT(ry);
	Coord3D world;
	if (!TheTacticalView->screenToTerrain(&pixel, &world))
	{
		showFeedback("Can't build there");
		return;
	}

	const CanMakeType cmt = TheBuildAssistant->canMakeUnit(builder, build);
	if (cmt != CANMAKE_OK)
	{
		// Same feedback as PlaceEventTranslator gives for these cases.
		if (cmt == CANMAKE_NO_MONEY)
		{
			TheEva->setShouldPlay(EVA_InsufficientFunds);
			TheInGameUI->message("GUI:NotEnoughMoneyToBuild");
		}
		else if (cmt == CANMAKE_QUEUE_FULL)
			TheInGameUI->message("GUI:ProductionQueueFull");
		else if (cmt == CANMAKE_PARKING_PLACES_FULL)
			TheInGameUI->message("GUI:ParkingPlacesFull");
		else if (cmt == CANMAKE_MAXED_OUT_FOR_PLAYER)
			TheInGameUI->message("GUI:UnitMaxedOut");
		else
			TheInGameUI->placeBuildAvailable(nullptr, nullptr);
		return;
	}

	// Start and end at the same point: the ghost keeps the rotation the player gave it.
	TheInGameUI->setPlacementStart(&pixel);
	sendClickAtReticle();
}

//-------------------------------------------------------------------------------------------------
/** B from targeting, placement or an order mode: back to the panel it came from, on the same
	region if that still has something, otherwise to the battlefield. */
//-------------------------------------------------------------------------------------------------
void GameController::returnToPanelIfPossible()
{
	const Int kind = m_returnPanel;
	m_returnPanel = PANEL_NONE;
	beginInputContext();
	if (kind == PANEL_NONE)
		return;
	if (kind == PANEL_COMMANDS && TheInGameUI->getSelectCount() == 0)
		return;

	Int region = (kind == m_panelKind) ? m_panelRegion : 0;
	if (!isPanelRegionAvailable(kind, region))
	{
		region = -1;
		for (Int r = 0; r < panelRegionCount(kind); ++r)
		{
			if (isPanelRegionAvailable(kind, r))
			{
				region = r;
				break;
			}
		}
		if (region < 0)
			return;
	}

	m_panelOpen = TRUE;
	m_panelKind = kind;
	m_panelRegion = region;
	m_repeatButton = 0;
	setScienceWindowShown(kind == PANEL_POWERS && region == REGION_POWERS_PROMOTE);
	noteFocusMovedByPlayer();
}

//-------------------------------------------------------------------------------------------------
/** B while aiming a generals power: drop it and go straight back to the
	battlefield with nothing selected, instead of back to the Powers panel. */
//-------------------------------------------------------------------------------------------------
void GameController::cancelPowerToBattlefield()
{
	m_returnPanel = PANEL_NONE;
	if (TheInGameUI->getSelectCount() > 0)
	{
		TheInGameUI->deselectAllDrawables();
		captureCohortFromSelection();
	}
	beginInputContext();
	showFeedback("Power cancelled");
}

//-------------------------------------------------------------------------------------------------
// Drawing
//-------------------------------------------------------------------------------------------------
void GameController::drawPanelFocus()
{
	if (!m_panelOpen)
		return;

	if (m_settings.wheelMenus)
	{
		drawWheel();
		return;
	}

	if (m_panelKind == PANEL_GROUPS)
	{
		drawGroupPanel();
		return;
	}

	PanelEntry entry;
	const Bool haveFocus = focusedPanelEntry(&entry, nullptr);
	if (haveFocus && entry.window)
	{
		Int x, y, w, h;
		entry.window->winGetScreenPosition(&x, &y);
		entry.window->winGetSize(&w, &h);
		const UnsignedInt focus = GameMakeColor(255, 220, 60, 255);
		TheDisplay->drawOpenRect(x - 3, y - 3, w + 6, h + 6, 3.0f, focus);
		TheDisplay->drawOpenRect(x - 6, y - 6, w + 12, h + 12, 1.0f, GameMakeColor(0, 0, 0, 200));
	}

	// Legend just above the command bar.
	const Int lineHeight = textLineHeight();
	Int barX = 16, barY = TheDisplay->getHeight() - lineHeight * 4;
	GameWindow *bar = TheWindowManager->winGetWindowFromId(nullptr, NAMEKEY("ControlBar.wnd:ControlBarParent"));
	if (bar && !bar->winIsHidden())
		bar->winGetScreenPosition(&barX, &barY);
	const Int legendX = barX + 16;
	const Int legendY = barY - lineHeight - 4;

	if (m_panelKind == PANEL_COMMANDS && m_panelRegion == REGION_ORDERS)
		drawOrderEntries(legendX, legendY - 4);

	if (!m_panelLines[0])
		m_panelLines[0] = makeString();

	// "D-pad up/down: <other regions that have something>"
	AsciiString others;
	for (Int r = 0; r < panelRegionCount(m_panelKind); ++r)
	{
		if (r == m_panelRegion || !isPanelRegionAvailable(m_panelKind, r))
			continue;
		if (others.isNotEmpty())
			others.concat(", ");
		others.concat(regionName(m_panelKind, r));
	}

	AsciiString legend;
	if (m_panelKind == PANEL_POWERS && m_panelRegion == REGION_POWERS_PROMOTE)
	{
		Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		legend.format("PROMOTIONS (%d point%s to spend)   LB/RB: move   A: buy", player ? player->getSciencePurchasePoints() : 0,
			(player && player->getSciencePurchasePoints() == 1) ? "" : "s");
	}
	else if (m_panelKind == PANEL_POWERS)
		legend = "GENERALS POWERS   LB/RB: move   A: use";
	else if (m_panelRegion == REGION_QUEUE)
		legend = "BUILD QUEUE   LB/RB: move   X: cancel item";
	else if (m_panelRegion == REGION_ORDERS)
		legend = "ORDERS   LB/RB: move   A: choose";
	else
		legend = "COMMANDS   LB/RB: move   A: use";
	if (others.isNotEmpty())
	{
		legend.concat("   D-pad up/down: ");
		legend.concat(others);
	}
	legend.concat("   B or Y: close");

	drawText(m_panelLines[0], toUnicodeText(legend.str()), legendX, legendY, GameMakeColor(255, 220, 60, 255));
}

void GameController::drawModeHint(Int cx, Int cy)
{
	UnicodeString text;
	if (isPlacementMode())
	{
		const ThingTemplate *build = TheInGameUI->getPendingPlaceType();
		text = toUnicodeText("Place ");
		text.concat(build ? build->getDisplayName() : UnicodeString::TheEmptyString);
		text.concat(toUnicodeText("   RS: rotate   LT: slow   RT: fast   A/X: place   B: back"));
	}
	else if (isTargetingMode())
	{
		text = commandName(TheInGameUI->getGUICommand());
		text.concat(toUnicodeText(":   A/X: choose target   LT/RT: slow/fast   B: back"));
	}
	else if (m_orderMode == ORDERMODE_FORCE_ATTACK)
	{
		text = toUnicodeText("Force attack:   A/X: choose target (ground or anything)   B: back");
	}
	else if (m_orderMode == ORDERMODE_WAYPOINTS)
	{
		UnicodeString count;
		count.format(L"Waypoints (%d):   A/X: add a waypoint   B: done", m_waypointCount);
		text = count;
	}
	else
	{
		return;
	}

	if (!m_panelLines[1])
		m_panelLines[1] = makeString();
	const Int offset = REAL_TO_INT(30.0f * uiScale());
	drawText(m_panelLines[1], text, cx + offset, cy - offset - textLineHeight(), GameMakeColor(255, 220, 60, 255));
}
