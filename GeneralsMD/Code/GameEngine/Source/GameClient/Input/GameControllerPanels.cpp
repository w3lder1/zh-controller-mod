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

// GameControllerPanels.cpp ///////////////////////////////////////////////////////////////////////
// ControllerMod @feature Generals Powers, control groups, base and alert jumps and
// the explicit orders a mouse player gives with Ctrl and Alt.
//
//   D-pad up     Powers panel (GameControllerCommandPanel.cpp does the focus): the power buttons at
//                the right screen edge, and the promotion window. Both are the real game windows.
//   D-pad down   Control groups 1-9 and 0: A selects the group (the panel stays open), Y assigns
//                the current selection, X clears (asks first), RT moves the camera to the group
//   D-pad left   Cycle the camera through your command centers (else your most valuable building,
//                like the native "view command center" key)
//   D-pad right  Cycle the camera through recent alerts, newest first (what the space bar jumps to)
//   Y > Orders   Force attack (one target, anything or the ground) and waypoints (a path of move
//                points), through the native force-attack and waypoint modes
//
// Groups use the native control group messages, so they are the same groups the number keys use.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameController.h"

#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Radar.h"
#include "Common/ThingTemplate.h"
#include "GameClient/Color.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/Drawable.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/InGameUI.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Squad.h"

#include <algorithm>

namespace
{
	const char *const SCIENCE_PARENT = "GeneralsExpPoints.wnd:GenExpParent";
	const char *const SCIENCE_FADE = "GenExpFade";   ///< its window transition (WindowTransitions.ini)
	const UnsignedInt ALERT_MAX_AGE_FRAMES = LOGICFRAMES_PER_SECOND * 90;
	const Real ALERT_MERGE_DISTANCE = 150.0f;
	const UnsignedInt ALERT_CYCLE_RESET_MS = 8000;   ///< after this long, D-pad right starts again at the newest
	enum { MAX_ALERTS = 16, NUM_ORDERS = 2 };

	UnicodeString toUnicodeText(const char *text)
	{
		UnicodeString u;
		u.translate(AsciiString(text));
		return u;
	}

	GameWindow *scienceWindow()
	{
		return TheWindowManager ? TheWindowManager->winGetWindowFromId(nullptr, NAMEKEY(SCIENCE_PARENT)) : nullptr;
	}

	/// Control group slots in keyboard order: 1 ... 9, 0.
	Int groupForSlot(Int slot)
	{
		return (slot + 1) % NUM_HOTKEY_SQUADS;
	}

	Squad *localGroup(Int group)
	{
		Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		return player ? player->getHotkeySquad(group) : nullptr;
	}

	struct BaseCollector
	{
		std::vector<ObjectID> commandCenters;
		ObjectID mostExpensive;
		Int mostExpensiveCost;
	};

	/// Same choice as the native "view command center" key: command centers, else the most
	/// expensive structure.
	void collectBase(Object *obj, void *userData)
	{
		BaseCollector *c = (BaseCollector *)userData;
		if (!obj || obj->isEffectivelyDead())
			return;
		if (obj->isKindOf(KINDOF_COMMANDCENTER))
		{
			c->commandCenters.push_back(obj->getID());
			return;
		}
		if (!obj->isKindOf(KINDOF_STRUCTURE))
			return;
		const Int cost = obj->getTemplate()->calcCostToBuild(obj->getControllingPlayer());
		if (cost > c->mostExpensiveCost)
		{
			c->mostExpensiveCost = cost;
			c->mostExpensive = obj->getID();
		}
	}

	const char *alertLabel(RadarEventType type)
	{
		switch (type)
		{
			case RADAR_EVENT_UNDER_ATTACK:        return "Under attack";
			case RADAR_EVENT_CONSTRUCTION:        return "Construction";
			case RADAR_EVENT_UPGRADE:             return "Upgrade";
			case RADAR_EVENT_INFILTRATION:        return "Infiltration";
			case RADAR_EVENT_STEALTH_DISCOVERED:  return "Stealth unit found";
			case RADAR_EVENT_STEALTH_NEUTRALIZED: return "Stealth lost";
			case RADAR_EVENT_FAKE:                return "Unit lost";
			default:                              return "Alert";
		}
	}
}

//-------------------------------------------------------------------------------------------------
// Powers panel
//-------------------------------------------------------------------------------------------------
Bool GameController::isPanelRegionAvailable(Int kind, Int region) const
{
	if (kind == PANEL_GROUPS)
		return region == REGION_GROUPS;
	// Wheels show the queue on the build sectors themselves (count, X to take out).
	if (m_settings.wheelMenus && kind == PANEL_COMMANDS && region == REGION_QUEUE)
		return FALSE;
	if (kind == PANEL_POWERS && region == REGION_POWERS_PROMOTE)
	{
		// The promotion window fills its buttons in when it is shown, so it is offered whenever
		// the player can have promotions at all.
		Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		return scienceWindow() != nullptr && player && player->isPlayerActive();
	}
	std::vector<PanelEntry> entries;
	collectPanelEntries(kind, region, &entries);
	return !entries.empty();
}

void GameController::collectPowerEntries(Int region, std::vector<PanelEntry> *out) const
{
	out->clear();
	Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	if (!player || !TheWindowManager)
		return;

	AsciiString name;
	if (region == REGION_POWERS_USE)
	{
		// The shortcut bar at the right screen edge; its layout comes from the player template.
		const PlayerTemplate *pt = player->getPlayerTemplate();
		if (!pt || pt->getSpecialPowerShortcutWinName().isEmpty())
			return;
		const AsciiString layout = pt->getSpecialPowerShortcutWinName();
		AsciiString parentName = layout;
		parentName.concat(":GenPowersShortcutBarParent");
		Int count = pt->getSpecialPowerShortcutButtonCount();
		if (count > MAX_SPECIAL_POWER_SHORTCUTS)
			count = MAX_SPECIAL_POWER_SHORTCUTS;
		for (Int i = 0; i < count; ++i)
		{
			name.format("%s:ButtonCommand%d", layout.str(), i + 1);
			GameWindow *win = findGameWindow(parentName.str(), name.str());
			if (isUsableCommandButton(win))
			{
				PanelEntry e;
				e.window = win;
				e.slot = i;
				out->push_back(e);
			}
		}
		return;
	}

	// Promotions, rank 1, 3 and 8 rows; only while the promotion window is shown.
	static const char *const rowNames[3] =
	{
		"GeneralsExpPoints.wnd:ButtonRank1Number%d",
		"GeneralsExpPoints.wnd:ButtonRank3Number%d",
		"GeneralsExpPoints.wnd:ButtonRank8Number%d"
	};
	static const Int rowCounts[3] = { MAX_PURCHASE_SCIENCE_RANK_1, MAX_PURCHASE_SCIENCE_RANK_3, MAX_PURCHASE_SCIENCE_RANK_8 };
	for (Int row = 0; row < 3; ++row)
	{
		for (Int i = 0; i < rowCounts[row]; ++i)
		{
			name.format(rowNames[row], i);
			GameWindow *win = findGameWindow(SCIENCE_PARENT, name.str());
			// A wheel keeps the promotion window hidden and presses its (filled in) buttons directly.
			const Bool usable = m_settings.wheelMenus
				? (win && !win->winIsHidden() && win->winGetInputFunc() == GadgetPushButtonInput && GadgetButtonGetData(win) != nullptr)
				: isUsableCommandButton(win);
			if (usable)
			{
				PanelEntry e;
				e.window = win;
				e.slot = row * 20 + i;
				out->push_back(e);
			}
		}
	}
}

void GameController::setScienceWindowShown(Bool show)
{
	GameWindow *win = scienceWindow();
	if (!win || !TheControlBar)
		return;
	if (show && m_settings.wheelMenus)
	{
		// The wheel shows the promotions itself; the window only has to fill its buttons in.
		refreshPromotionButtons();
		m_promotionRefreshMs = timeGetTime();
		return;
	}
	if (show)
	{
		// If the player already opened it with the mouse, it is theirs to close.
		if (win->winIsHidden())
		{
			TheControlBar->showPurchaseScience();
			// Its fade-in hides the real window for a few frames and draws a picture instead; skip
			// it, so the window and its buttons are really there from the first frame.
			if (TheTransitionHandler)
				TheTransitionHandler->remove(SCIENCE_FADE, TRUE);
			m_scienceShownByUs = !win->winIsHidden();
		}
	}
	else if (m_scienceShownByUs)
	{
		closeScienceWindow();
	}
}

//-------------------------------------------------------------------------------------------------
/** Let the promotion window fill in its buttons (availability, rank, points) without showing it:
	it only does that when it is shown, so show it, finish its fade and hide it again in one go. */
//-------------------------------------------------------------------------------------------------
void GameController::refreshPromotionButtons()
{
	GameWindow *win = scienceWindow();
	if (!win || !TheControlBar || !win->winIsHidden())
		return;   // shown: it keeps itself up to date
	TheControlBar->showPurchaseScience();
	if (TheTransitionHandler)
		TheTransitionHandler->remove(SCIENCE_FADE, TRUE);
	TheControlBar->hidePurchaseScience();
}

Bool GameController::isScienceWindowShown() const
{
	GameWindow *win = scienceWindow();
	return win && !win->winIsHidden();
}

//-------------------------------------------------------------------------------------------------
/** Close the promotion window, whoever opened it. Returns TRUE if it was open. */
//-------------------------------------------------------------------------------------------------
Bool GameController::closeScienceWindow()
{
	m_scienceShownByUs = FALSE;
	GameWindow *win = scienceWindow();
	if (!win || !TheControlBar)
		return FALSE;
	// A fade still running would show the window again when it ends (and hidePurchaseScience
	// ignores a window the fade has hidden for the moment), so finish it first.
	if (TheTransitionHandler)
		TheTransitionHandler->remove(SCIENCE_FADE, TRUE);
	if (win->winIsHidden())
		return FALSE;
	TheControlBar->hidePurchaseScience();
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** The parked mouse cursor must not rest on the promotion window: its own hover tooltip would
	fight the controller's focus tooltip. Park it beside the window, over the battlefield, where no
	other window (such as the command bar below) has a tooltip either. */
//-------------------------------------------------------------------------------------------------
void GameController::adjustCursorParkPoint(ICoord2D *pos) const
{
	GameWindow *win = scienceWindow();
	if (!win || !isWindowShown(win) || !TheTacticalView)
		return;

	Int x, y, w, h;
	win->winGetScreenPosition(&x, &y);
	win->winGetSize(&w, &h);
	if (pos->x < x || pos->x >= x + w || pos->y < y || pos->y >= y + h)
		return;

	Int ox = 0, oy = 0;
	TheTacticalView->getOrigin(&ox, &oy);
	const Int viewRight = ox + TheTacticalView->getWidth();
	if (x - 16 > ox + 8)
		pos->x = x - 16;
	else if (x + w + 16 < viewRight - 8)
		pos->x = x + w + 16;
}

//-------------------------------------------------------------------------------------------------
// Orders region and order modes
//-------------------------------------------------------------------------------------------------
const char *GameController::orderEntryLabel(Int order)
{
	switch (order)
	{
		case ORDERMODE_FORCE_ATTACK: return "Force attack  (attack the ground, or anything, even your own)";
		case ORDERMODE_WAYPOINTS:    return "Waypoints  (give a path of move points, one by one)";
		default:                     return "";
	}
}

void GameController::collectOrderEntries(std::vector<PanelEntry> *out) const
{
	out->clear();
	if (!TheInGameUI || !TheInGameUI->areSelectedObjectsControllable())
		return;

	// Force attack suits anything that can shoot; waypoints need something that moves.
	Bool anyMobile = FALSE;
	const DrawableList *selected = TheInGameUI->getAllSelectedDrawables();
	for (DrawableList::const_iterator it = selected->begin(); it != selected->end(); ++it)
	{
		const Object *obj = (*it) ? (*it)->getObject() : nullptr;
		if (obj && !obj->isKindOf(KINDOF_STRUCTURE) && !obj->isKindOf(KINDOF_IMMOBILE))
		{
			anyMobile = TRUE;
			break;
		}
	}

	PanelEntry e;
	e.window = nullptr;
	e.slot = ORDERMODE_FORCE_ATTACK;
	out->push_back(e);
	if (anyMobile)
	{
		e.slot = ORDERMODE_WAYPOINTS;
		out->push_back(e);
	}
}

void GameController::beginOrderMode(Int mode)
{
	endOrderMode(FALSE);
	m_orderMode = mode;
	m_waypointCount = 0;
	if (mode == ORDERMODE_FORCE_ATTACK)
		TheInGameUI->setForceAttackMode(TRUE);
	else
		TheInGameUI->setWaypointMode(TRUE);

	m_panelOpen = FALSE;
	setScienceWindowShown(FALSE);
	m_returnPanel = PANEL_COMMANDS;
	m_repeatButton = 0;
	beginInputContext();
}

void GameController::endOrderMode(Bool backToPanel)
{
	if (m_orderMode == ORDERMODE_NONE)
		return;
	if (TheInGameUI)
	{
		if (m_orderMode == ORDERMODE_FORCE_ATTACK)
			TheInGameUI->setForceAttackMode(FALSE);
		else
			TheInGameUI->setWaypointMode(FALSE);
	}
	m_orderMode = ORDERMODE_NONE;
	m_waypointCount = 0;
	if (backToPanel)
	{
		returnToPanelIfPossible();
	}
	else
	{
		m_returnPanel = PANEL_NONE;
		beginInputContext();
	}
}

//-------------------------------------------------------------------------------------------------
/** Per frame: an order mode only lasts while it still makes sense. */
//-------------------------------------------------------------------------------------------------
void GameController::updateOrderMode()
{
	if (m_orderMode == ORDERMODE_NONE)
		return;

	if (!isActiveInput() || !TheInGameUI->getInputEnabled() || !TheInGameUI->areSelectedObjectsControllable() ||
		isPlacementMode() || isTargetingMode() || m_panelOpen)
	{
		endOrderMode(FALSE);
		return;
	}

	// A keyboard Ctrl or Alt release clears the native flag; keep it while the mode is on.
	if (m_orderMode == ORDERMODE_FORCE_ATTACK && !TheInGameUI->isInForceAttackMode())
		TheInGameUI->setForceAttackMode(TRUE);
	if (m_orderMode == ORDERMODE_WAYPOINTS && !TheInGameUI->isInWaypointMode())
		TheInGameUI->setWaypointMode(TRUE);
}

void GameController::handleOrderModeButtons(UnsignedInt pressed)
{
	if (m_orderMode == ORDERMODE_FORCE_ATTACK)
	{
		if (pressed & ControllerState::BUTTON_B)
		{
			endOrderMode(TRUE);
			return;
		}
		// One target per use, like a targeted ability. A refused target keeps the mode armed.
		if (pressed & (ControllerState::BUTTON_A | ControllerState::BUTTON_X))
		{
			if (issueOrder() != GameMessage::MSG_INVALID)
				endOrderMode(FALSE);
		}
		return;
	}

	// Waypoints: every press adds the next point of the path; B finishes. Before the first point,
	// B goes back to the Orders panel instead, like cancelling a target.
	if (pressed & ControllerState::BUTTON_B)
	{
		const Bool none = m_waypointCount == 0;
		endOrderMode(none);
		if (!none)
			showFeedback("Waypoints done");
		return;
	}
	if (pressed & (ControllerState::BUTTON_A | ControllerState::BUTTON_X))
	{
		if (issueOrder() == GameMessage::MSG_ADD_WAYPOINT)
			++m_waypointCount;
	}
}

void GameController::drawOrderEntries(Int x, Int bottomY)
{
	std::vector<PanelEntry> entries;
	collectOrderEntries(&entries);
	PanelEntry focus;
	const Bool haveFocus = focusedPanelEntry(&focus, nullptr);

	const Int lineHeight = textLineHeight() + 4;
	Int y = bottomY - lineHeight * (Int)entries.size();
	for (size_t i = 0; i < entries.size() && i < 2; ++i)
	{
		const Bool focused = haveFocus && focus.slot == entries[i].slot;
		if (!m_groupLines[i])
			m_groupLines[i] = makeString();
		AsciiString text;
		text.format("%s %s", focused ? ">" : " ", orderEntryLabel(entries[i].slot));
		drawText(m_groupLines[i], toUnicodeText(text.str()), x, y,
			focused ? GameMakeColor(255, 220, 60, 255) : GameMakeColor(220, 220, 220, 255));
		y += lineHeight;
	}
}

//-------------------------------------------------------------------------------------------------
// Control groups
//-------------------------------------------------------------------------------------------------
void GameController::handleGroupPanelButtons(UnsignedInt pressed, UnsignedInt held, UnsignedInt nowMs)
{
	// A clear waits for its confirmation: X clears, anything else keeps the group.
	if (m_groupClearPending >= 0)
	{
		if (pressed & ControllerState::BUTTON_X)
		{
			clearGroup(m_groupClearPending);
			m_groupClearPending = -1;
		}
		else if (pressed != 0)
		{
			m_groupClearPending = -1;
			showFeedback("Group kept");
		}
		return;
	}

	if (pressed & (ControllerState::BUTTON_B | ControllerState::BUTTON_DPAD_DOWN))
	{
		closePanel();
		return;
	}

	if (!m_settings.wheelMenus)
	{
		const Int step = panelStepFromButtons(pressed, held, nowMs);
		if (step != 0)
		{
			stepPanelFocus(step);
			return;
		}
	}

	PanelEntry entry;
	if (!focusedPanelEntry(&entry, nullptr))
	{
		if (pressed & (ControllerState::BUTTON_A | ControllerState::BUTTON_X | ControllerState::BUTTON_Y | ControllerState::BUTTON_RT))
			showFeedback("Point at a group with the left stick first");
		return;
	}
	const Int group = groupForSlot(entry.slot);

	if (pressed & ControllerState::BUTTON_A)
	{
		recallGroup(group);
		return;
	}
	if (pressed & ControllerState::BUTTON_Y)
	{
		assignGroup(group);
		return;
	}
	if (pressed & ControllerState::BUTTON_X)
	{
		Squad *squad = localGroup(group);
		if (!squad || squad->getLiveObjects().empty())
		{
			UnicodeString text;
			text.format(L"Group %d is already empty", group);
			showFeedback(text);
			return;
		}
		m_groupClearPending = group;
		return;
	}
	if (pressed & ControllerState::BUTTON_RT)
	{
		focusGroup(group);
		return;
	}
}

//-------------------------------------------------------------------------------------------------
/** A: select the group, as the native number key does, but without its double-press camera jump
	(RT is the separate camera action here). */
//-------------------------------------------------------------------------------------------------
void GameController::recallGroup(Int group)
{
	Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	Squad *squad = localGroup(group);
	if (!player || !squad || squad->getLiveObjects().empty())
	{
		UnicodeString text;
		text.format(L"Group %d is empty - Y assigns your selection to it", group);
		showFeedback(text);
		return;
	}

	// SelectionTranslator::onMetaSelectTeam, single-press branch.
	TheInGameUI->deselectAllDrawables();
	TheMessageStream->appendMessage((GameMessage::Type)(GameMessage::MSG_SELECT_TEAM0 + group));
	const VecObjectPtr &objects = squad->getLiveObjects();
	Int count = 0;
	for (size_t i = 0; i < objects.size(); ++i)
	{
		if (objects[i]->getControllingPlayer() == player && objects[i]->getDrawable())
		{
			TheInGameUI->selectDrawable(objects[i]->getDrawable());
			++count;
		}
	}

	UnicodeString text;
	text.format(L"Group %d selected (%d)", group, count);
	showFeedback(text);
}

void GameController::assignGroup(Int group)
{
	if (TheInGameUI->getSelectCount() == 0)
	{
		showFeedback("Select units first, then Y assigns them");
		return;
	}
	// The native Ctrl+number message; SelectionTranslator sends the selected, locally controlled IDs.
	TheMessageStream->appendMessage((GameMessage::Type)(GameMessage::MSG_META_CREATE_TEAM0 + group));

	UnicodeString text;
	text.format(L"Selection assigned to group %d", group);
	showFeedback(text);
}

void GameController::clearGroup(Int group)
{
	// The native create-team message with no members empties the group.
	TheMessageStream->appendMessage((GameMessage::Type)(GameMessage::MSG_CREATE_TEAM0 + group));

	UnicodeString text;
	text.format(L"Group %d cleared", group);
	showFeedback(text);
}

void GameController::focusGroup(Int group)
{
	Squad *squad = localGroup(group);
	if (!squad || squad->getLiveObjects().empty())
	{
		UnicodeString text;
		text.format(L"Group %d is empty", group);
		showFeedback(text);
		return;
	}
	// The native view-team message: centres the camera on the group, respecting camera locks.
	TheMessageStream->appendMessage((GameMessage::Type)(GameMessage::MSG_META_VIEW_TEAM0 + group));
}

void GameController::drawGroupPanel()
{
	const Real scale = uiScale();
	const Int boxW = REAL_TO_INT(80.0f * scale) < 44 ? 44 : REAL_TO_INT(80.0f * scale);
	const Int boxH = REAL_TO_INT(40.0f * scale) < 24 ? 24 : REAL_TO_INT(40.0f * scale);
	const Int gap = REAL_TO_INT(6.0f * scale) < 3 ? 3 : REAL_TO_INT(6.0f * scale);
	const Int lineHeight = textLineHeight();

	Int barX = 0, barY = TheDisplay->getHeight() - lineHeight * 4;
	GameWindow *bar = TheWindowManager->winGetWindowFromId(nullptr, NAMEKEY("ControlBar.wnd:ControlBarParent"));
	if (bar && !bar->winIsHidden())
		bar->winGetScreenPosition(&barX, &barY);

	const Int totalW = NUM_HOTKEY_SQUADS * boxW + (NUM_HOTKEY_SQUADS - 1) * gap;
	const Int left = ((Int)TheDisplay->getWidth() - totalW) / 2;
	const Int top = barY - boxH - gap * 2;

	PanelEntry focus;
	const Bool haveFocus = focusedPanelEntry(&focus, nullptr);
	Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;

	for (Int slot = 0; slot < NUM_HOTKEY_SQUADS; ++slot)
	{
		const Int group = groupForSlot(slot);
		Squad *squad = localGroup(group);
		const Int count = squad ? (Int)squad->getLiveObjects().size() : 0;
		const Bool focused = haveFocus && focus.slot == slot;

		const Int x = left + slot * (boxW + gap);
		TheDisplay->drawFillRect(x, top, boxW, boxH, GameMakeColor(0, 0, 0, 170));
		TheDisplay->drawOpenRect(x, top, boxW, boxH, focused ? 3.0f : 1.0f,
			focused ? GameMakeColor(255, 220, 60, 255) : GameMakeColor(160, 160, 160, 200));

		if (!m_groupLines[slot])
			m_groupLines[slot] = makeString();
		UnicodeString label;
		if (count > 0)
			label.format(L"%d: %d", group, count);
		else
			label.format(L"%d: -", group);
		drawText(m_groupLines[slot], label, x + gap, top + (boxH - lineHeight) / 2,
			count > 0 ? GameMakeColor(255, 255, 255, 255) : GameMakeColor(150, 150, 150, 255));
	}

	// The focused group's make-up, e.g. "Group 2: 4 Ranger, 2 Humvee".
	if (!m_groupLines[10])
		m_groupLines[10] = makeString();
	UnicodeString detail;
	if (haveFocus)
	{
		const Int group = groupForSlot(focus.slot);
		Squad *squad = localGroup(group);
		detail.format(L"Group %d: ", group);
		if (!squad || squad->getLiveObjects().empty())
		{
			detail.concat(L"empty");
		}
		else
		{
			// Count by type, in the order the types first appear.
			std::vector<const ThingTemplate *> types;
			std::vector<Int> counts;
			const VecObjectPtr &objects = squad->getLiveObjects();
			for (size_t i = 0; i < objects.size(); ++i)
			{
				if (player && objects[i]->getControllingPlayer() != player)
					continue;
				const ThingTemplate *t = objects[i]->getTemplate();
				size_t j = 0;
				while (j < types.size() && types[j] != t)
					++j;
				if (j == types.size())
				{
					types.push_back(t);
					counts.push_back(0);
				}
				++counts[j];
			}
			for (size_t j = 0; j < types.size() && j < 4; ++j)
			{
				UnicodeString part;
				part.format(L"%s%d ", j > 0 ? L", " : L"", counts[j]);
				detail.concat(part);
				detail.concat(types[j]->getDisplayName());
			}
			if (types.size() > 4)
				detail.concat(L", ...");
		}
	}
	drawText(m_groupLines[10], detail, left, top - lineHeight - 2, GameMakeColor(255, 255, 255, 255));

	if (!m_groupLines[11])
		m_groupLines[11] = makeString();
	UnicodeString legend;
	if (m_groupClearPending >= 0)
		legend.format(L"Clear group %d?   X: yes, clear it   any other button: keep it", m_groupClearPending);
	else
		legend = toUnicodeText("CONTROL GROUPS   LB/RB: move   A: select   Y: assign   X: clear   RT: camera   B: close");
	drawText(m_groupLines[11], legend, left, top - lineHeight * 2 - 4, GameMakeColor(255, 220, 60, 255));
}

//-------------------------------------------------------------------------------------------------
// Camera jumps
//-------------------------------------------------------------------------------------------------
void GameController::jumpToBase()
{
	Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	if (!player || !player->isPlayerActive())
		return;

	BaseCollector c;
	c.mostExpensive = INVALID_ID;
	c.mostExpensiveCost = -1;
	player->iterateObjects(collectBase, &c);

	std::vector<ObjectID> &bases = c.commandCenters;
	if (bases.empty() && c.mostExpensive != INVALID_ID)
		bases.push_back(c.mostExpensive);
	if (bases.empty())
	{
		showFeedback("No base to jump to");
		return;
	}

	// A stable order, so repeated presses visit every base in turn.
	std::sort(bases.begin(), bases.end());
	if (m_baseJumpIndex < 0 || m_baseJumpIndex >= (Int)bases.size())
		m_baseJumpIndex = 0;
	Object *target = TheGameLogic->findObjectByID(bases[m_baseJumpIndex]);
	if (target)
		TheTacticalView->userLookAt(target->getPosition());

	if (bases.size() > 1)
	{
		UnicodeString text;
		text.format(L"Base %d of %d", m_baseJumpIndex + 1, (Int)bases.size());
		showFeedback(text);
	}
	m_baseJumpIndex = (m_baseJumpIndex + 1) % (Int)bases.size();
}

void GameController::jumpToAlert()
{
	if (!TheRadar)
		return;

	Coord3D locations[MAX_ALERTS];
	UnsignedInt frames[MAX_ALERTS];
	RadarEventType types[MAX_ALERTS];
	const Int count = TheRadar->getRecentEvents(locations, frames, types, MAX_ALERTS, ALERT_MAX_AGE_FRAMES, ALERT_MERGE_DISTANCE);
	if (count == 0)
	{
		showFeedback("No recent alerts");
		return;
	}

	// Start at the newest alert when there is a new one or after a pause; otherwise go one older.
	const UnsignedInt now = timeGetTime();
	if (frames[0] != m_alertNewestFrame || m_lastAlertJumpMs == 0 || now - m_lastAlertJumpMs > ALERT_CYCLE_RESET_MS)
		m_alertJumpIndex = 0;
	else
		m_alertJumpIndex = (m_alertJumpIndex + 1) % count;
	m_alertNewestFrame = frames[0];
	m_lastAlertJumpMs = now;

	TheTacticalView->userLookAt(&locations[m_alertJumpIndex]);

	UnicodeString text;
	text.format(L"%hs (%d of %d)", alertLabel(types[m_alertJumpIndex]), m_alertJumpIndex + 1, count);
	showFeedback(text);
}
