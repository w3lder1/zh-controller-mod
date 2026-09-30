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

// GameControllerWheel.cpp ////////////////////////////////////////////////////////////////////////
// ControllerMod @feature Halo Wars style radial wheels.
//
// The wheel is only a presentation of the controller panels (Commands on Y, Powers on D-pad up,
// Groups on D-pad down): the same entries, the same regions and the same activation, so a wheel
// and the panel always do exactly the same thing. The setting WheelMenus switches back to the
// panel.
//
// Up to ten entries: one wheel in the middle on the left stick. The highlight stays when the
// stick is let go; only A uses it.
// More than ten: two wheels, the left one on the left stick and the right one on the right stick.
// Both highlights stay when the sticks are let go. The wheel pointed at last is the current one;
// clicking the left stick in (or A) uses its highlight, so the right thumb can stay on the right
// stick.
//
//   A, L-stick click  use the highlighted sector (nothing is ever used by moving or letting go)
//   X or LB        on something being built: take one out of the queue; hold it: take all out
//   D-pad up/down  switch the wheel's section (Commands / Orders, Powers / Promotions)
//   B or Y         close (groups: B or D-pad down; Y assigns there)
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameController.h"

#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "GameClient/Color.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/Drawable.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameWindow.h"
#include "GameClient/Image.h"
#include "GameClient/InGameUI.h"
#include "GameClient/View.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Object.h"
#include "GameLogic/Squad.h"

namespace
{
	const Real STICK_POINT_THRESHOLD = 0.55f;   ///< stick deflection needed to aim a wheel
	const Real STICK_HYSTERESIS_DEG = 10.0f;    ///< the current sector keeps the stick this far past its edge
	const Real RING_STEP_DEG = 2.5f;            ///< angular step of the lines that fill the ring
	const UnsignedInt HOLD_CANCEL_ALL_MS = 500; ///< X held this long on a queued item cancels all of it

	// m_wheelLines use
	enum
	{
		LINE_SECTOR_FIRST = 0,     // 0..27 sector labels
		LINE_BADGE_FIRST = 28,     // 28..55 queue count badges
		LINE_CENTER_NAME = 56,     // + side
		LINE_CENTER_INFO = 58,     // + side
		LINE_TITLE = 60,
		LINE_SECTIONS = 61,
		LINE_LEGEND = 62,
		MAX_LABELLED_SECTORS = 28
	};

	UnicodeString toUnicodeText(const char *text)
	{
		UnicodeString u;
		u.translate(AsciiString(text));
		return u;
	}

	/// Screen point at angle (degrees, 0 = up, clockwise) and radius around (cx, cy).
	void wheelPoint(Int cx, Int cy, Real degrees, Real radius, Int *x, Int *y)
	{
		const Real a = degrees * PI / 180.0f;
		*x = cx + REAL_TO_INT(radius * (Real)sin((double)a));
		*y = cy - REAL_TO_INT(radius * (Real)cos((double)a));
	}

	/// Signed smallest difference a - b in degrees, in (-180, 180].
	Real angleDelta(Real a, Real b)
	{
		Real d = a - b;
		while (d > 180.0f)
			d -= 360.0f;
		while (d <= -180.0f)
			d += 360.0f;
		return d;
	}

	/// Stick direction in degrees, 0 = up, clockwise; or -1 when the stick is not pushed far enough.
	Real stickAngle(Real x, Real y)
	{
		if (sqrt((double)(x * x + y * y)) < STICK_POINT_THRESHOLD)
			return -1.0f;
		Real angle = (Real)(atan2((double)x, (double)y) * 180.0 / PI);
		if (angle < 0.0f)
			angle += 360.0f;
		return angle;
	}

	/// The sector the stick points at, keeping the current one while the stick is still near it.
	Int sectorForAngle(Real angle, Int count, Int currentIndex)
	{
		const Real width = 360.0f / count;
		if (currentIndex >= 0 && currentIndex < count &&
			fabs(angleDelta(angle, currentIndex * width)) <= width * 0.5f + STICK_HYSTERESIS_DEG)
			return currentIndex;
		return ((Int)((angle + width * 0.5f) / width)) % count;
	}

	const char *regionTitle(Int kind, Int region)
	{
		if (kind == 0)   // commands
			return region == 0 ? "COMMANDS" : (region == 1 ? "BUILD QUEUE" : "ORDERS");
		if (kind == 1)   // powers
			return region == 0 ? "GENERALS POWERS" : "PROMOTIONS";
		return "CONTROL GROUPS";
	}

	const char *regionShortName(Int kind, Int region)
	{
		if (kind == 0)
			return region == 0 ? "Commands" : (region == 1 ? "Build queue" : "Orders");
		return region == 0 ? "Powers" : "Promotions";
	}

	Int groupForWheelSlot(Int slot)
	{
		return (slot + 1) % NUM_HOTKEY_SQUADS;
	}

	Bool isQueueable(const CommandButton *command)
	{
		if (!command)
			return FALSE;
		switch (command->getCommandType())
		{
			case GUI_COMMAND_UNIT_BUILD:
			case GUI_COMMAND_PLAYER_UPGRADE:
			case GUI_COMMAND_OBJECT_UPGRADE:
				return TRUE;
			default:
				return FALSE;
		}
	}

	Bool entryMatches(const ProductionEntry *p, const CommandButton *command)
	{
		if (command->getCommandType() == GUI_COMMAND_UNIT_BUILD)
		{
			const ThingTemplate *thing = command->getThingTemplate();
			return p->getProductionType() == PRODUCTION_UNIT && thing && p->getProductionObject() &&
				p->getProductionObject()->isEquivalentTo(thing);
		}
		return p->getProductionType() == PRODUCTION_UPGRADE && p->getProductionUpgrade() == command->getUpgradeTemplate();
	}

	DisplayString *&lineAt(DisplayString **lines, Int index)
	{
		return lines[index];
	}
}

//-------------------------------------------------------------------------------------------------
// Wheels and focus
//-------------------------------------------------------------------------------------------------
Int GameController::wheelCountFor(Int total)
{
	return total <= MAX_WHEEL_SECTORS ? 1 : 2;
}

/// The entries on one wheel: all of them, or the first/second half when there are two wheels.
void GameController::collectWheelEntries(Int side, std::vector<PanelEntry> *out, Int *wheelCountOut) const
{
	std::vector<PanelEntry> all;
	collectPanelEntries(m_panelKind, m_panelRegion, &all);
	out->clear();
	const Int total = (Int)all.size();
	const Int wheels = wheelCountFor(total);
	if (wheelCountOut)
		*wheelCountOut = wheels;
	const Int split = (wheels == 1) ? total : (total + 1) / 2;
	const Int from = (side == 0) ? 0 : split;
	const Int to = (side == 0) ? split : total;
	for (Int i = from; i < to; ++i)
		out->push_back(all[i]);
}

/// The one entry A or X would use: the highlighted sector of the single wheel, or of the only
/// wheel whose stick is pushed right now.
Bool GameController::wheelFocusedEntry(PanelEntry *entry, Int *indexOut) const
{
	if (indexOut)
		*indexOut = -1;
	Int wheels = 1;
	std::vector<PanelEntry> side0;
	collectWheelEntries(0, &side0, &wheels);

	Int side = 0;
	Int slot = m_panelSlot[m_panelKind][m_panelRegion];
	if (wheels == 1)
	{
		if (!m_wheelHighlight)
			return FALSE;
	}
	else
	{
		// The wheel pointed at last.
		if (m_dualCurrent < 0 || m_dualCurrent > 1 || !m_dualActive[m_dualCurrent])
			return FALSE;
		side = m_dualCurrent;
		slot = m_dualSlot[side];
	}

	std::vector<PanelEntry> entries;
	if (side == 0)
		entries = side0;
	else
		collectWheelEntries(1, &entries, nullptr);
	for (size_t i = 0; i < entries.size(); ++i)
	{
		if (entries[i].slot == slot)
		{
			if (indexOut)
				*indexOut = (Int)i;
			if (entry)
				*entry = entries[i];
			return TRUE;
		}
	}
	return FALSE;
}

void GameController::resetWheel()
{
	m_wheelHighlight = FALSE;
	m_dualActive[0] = m_dualActive[1] = FALSE;
	m_dualSlot[0] = m_dualSlot[1] = -1;
	m_dualCurrent = -1;
	m_stickPushed[0] = m_stickPushed[1] = FALSE;
	m_xHoldSlot = -1;
	m_xHoldProducer = INVALID_ID;
	m_xHoldDone = FALSE;
}

//-------------------------------------------------------------------------------------------------
/** Sticks while a wheel is open. One wheel: the left stick aims it. Two wheels: each stick aims
	its own wheel, and the wheel moved last becomes the current one. Highlights always stay when
	the sticks are let go. */
//-------------------------------------------------------------------------------------------------
void GameController::updateWheelSticks(Real lx, Real ly, Real rx, Real ry)
{
	if (!m_panelOpen || !m_settings.wheelMenus || m_panelKind < 0)
		return;

	PanelEntry before;
	const Bool hadFocus = wheelFocusedEntry(&before, nullptr);

	Int wheels = 1;
	std::vector<PanelEntry> side0;
	collectWheelEntries(0, &side0, &wheels);

	if (wheels == 1)
	{
		const Real angle = stickAngle(lx, ly);
		const Int n = (Int)side0.size();
		if (angle >= 0.0f && n > 0)
		{
			Int current = -1;
			for (Int i = 0; m_wheelHighlight && i < n; ++i)
			{
				if (side0[i].slot == m_panelSlot[m_panelKind][m_panelRegion])
					current = i;
			}
			const Int index = sectorForAngle(angle, n, current);
			m_panelSlot[m_panelKind][m_panelRegion] = side0[index].slot;
			m_wheelHighlight = TRUE;
		}
		m_dualActive[0] = m_dualActive[1] = FALSE;
	}
	else
	{
		std::vector<PanelEntry> side1;
		collectWheelEntries(1, &side1, nullptr);
		const std::vector<PanelEntry> *sides[2] = { &side0, &side1 };
		const Real angles[2] = { stickAngle(lx, ly), stickAngle(rx, ry) };
		for (Int s = 0; s < 2; ++s)
		{
			const Int n = (Int)sides[s]->size();

			// A highlight whose entry is gone is dropped, so A cannot use whatever took its place.
			Int current = -1;
			for (Int i = 0; m_dualActive[s] && i < n; ++i)
			{
				if ((*sides[s])[i].slot == m_dualSlot[s])
					current = i;
			}
			if (m_dualActive[s] && current < 0)
			{
				m_dualActive[s] = FALSE;
				if (m_dualCurrent == s)
					m_dualCurrent = m_dualActive[1 - s] ? 1 - s : -1;
			}

			const Bool pushed = angles[s] >= 0.0f && n > 0;
			if (pushed)
			{
				const Int index = sectorForAngle(angles[s], n, current);
				const Int slot = (*sides[s])[index].slot;
				// Pushing a stick, or moving it to another sector, makes its wheel the current one.
				if (!m_stickPushed[s] || !m_dualActive[s] || slot != m_dualSlot[s])
					m_dualCurrent = s;
				m_dualSlot[s] = slot;
				m_dualActive[s] = TRUE;
			}
			m_stickPushed[s] = pushed;
		}
		m_wheelHighlight = FALSE;
	}

	PanelEntry after;
	const Bool hasFocus = wheelFocusedEntry(&after, nullptr);
	if (hasFocus != hadFocus || (hasFocus && (after.slot != before.slot || after.window != before.window)))
	{
		m_groupClearPending = -1;
		noteFocusMovedByPlayer();
	}
}

//-------------------------------------------------------------------------------------------------
// Production queue on the wheel
//-------------------------------------------------------------------------------------------------
/// The building whose queue the wheel shows: one selected producer (the game only cancels
/// production for a single selected object).
Object *GameController::singleProducer() const
{
	if (!TheInGameUI || TheInGameUI->getSelectCount() != 1)
		return nullptr;
	const DrawableList *selected = TheInGameUI->getAllSelectedDrawables();
	if (!selected || selected->empty())
		return nullptr;
	Object *obj = selected->front() ? selected->front()->getObject() : nullptr;
	if (!obj || !obj->isLocallyControlled() || !obj->getProductionUpdateInterface())
		return nullptr;
	return obj;
}

Int GameController::queuedCount(const CommandButton *command) const
{
	if (!isQueueable(command))
		return 0;
	Object *producer = singleProducer();
	ProductionUpdateInterface *pu = producer ? producer->getProductionUpdateInterface() : nullptr;
	if (!pu)
		return 0;
	Int count = 0;
	for (const ProductionEntry *p = pu->firstProduction(); p; p = pu->nextProduction(p))
	{
		if (entryMatches(p, command))
			++count;
	}
	return count;
}

//-------------------------------------------------------------------------------------------------
/** Take the newest queued entry of this item out of the queue (all of them when 'all'), with the
	same messages the native queue buttons send (MSG_CANCEL_UNIT_CREATE / MSG_CANCEL_UPGRADE),
	which refund normally. Returns how many were cancelled. */
//-------------------------------------------------------------------------------------------------
Int GameController::cancelQueued(const CommandButton *command, Bool all)
{
	if (!isQueueable(command))
		return 0;
	Object *producer = singleProducer();
	ProductionUpdateInterface *pu = producer ? producer->getProductionUpdateInterface() : nullptr;
	if (!pu)
		return 0;

	if (command->getCommandType() != GUI_COMMAND_UNIT_BUILD)
	{
		const UpgradeTemplate *upgrade = command->getUpgradeTemplate();
		if (!upgrade || queuedCount(command) == 0)
			return 0;
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CANCEL_UPGRADE);
		msg->appendIntegerArgument(upgrade->getUpgradeNameKey());
		return 1;
	}

	std::vector<ProductionID> ids;
	for (const ProductionEntry *p = pu->firstProduction(); p; p = pu->nextProduction(p))
	{
		if (entryMatches(p, command))
			ids.push_back(p->getProductionID());
	}
	if (ids.empty())
		return 0;

	Int cancelled = 0;
	for (Int i = (Int)ids.size() - 1; i >= 0; --i)   // newest first, so the one being built goes last
	{
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CANCEL_UNIT_CREATE);
		msg->appendIntegerArgument(ids[i]);
		++cancelled;
		if (!all)
			break;
	}
	return cancelled;
}

//-------------------------------------------------------------------------------------------------
// Buttons
//-------------------------------------------------------------------------------------------------
void GameController::handleWheelButtons(UnsignedInt pressed, UnsignedInt held, UnsignedInt nowMs)
{
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

	PanelEntry entry;
	const Bool haveEntry = focusedPanelEntry(&entry, nullptr);
	const UnsignedInt useButtons = ControllerState::BUTTON_A | ControllerState::BUTTON_LS;

	// X or LB takes one out of the queue (LB can be reached while both thumbs are on the sticks).
	const UnsignedInt removeButtons = ControllerState::BUTTON_X | ControllerState::BUTTON_LB;
	if ((pressed & (useButtons | removeButtons)) && !haveEntry)
	{
		showFeedback("Point at something with a stick first");
		return;
	}

	// A, or clicking the left stick in (so the right thumb can stay on the right stick).
	if (pressed & useButtons)
	{
		useFocusedPanelEntry(entry, nowMs);
		return;
	}

	// X or LB on something being built: one out of the queue now; still holding it: all of it.
	const CommandButton *command = (haveEntry && entry.window) ? (const CommandButton *)GadgetButtonGetData(entry.window) : nullptr;
	if (pressed & removeButtons)
	{
		m_xHoldButton = (pressed & ControllerState::BUTTON_X) ? ControllerState::BUTTON_X : ControllerState::BUTTON_LB;
		m_xHoldSlot = -1;
		if (!isQueueable(command))
			return;
		if (!isFocusedCommandSeen(entry, nowMs))
		{
			showFeedback("This button just changed - press X again");
			return;
		}
		Object *producer = singleProducer();
		if (cancelQueued(command, FALSE) > 0)
		{
			showFeedback("Removed one from the queue (hold: all)");
			m_xHoldSlot = entry.slot;
			m_xHoldProducer = producer ? producer->getID() : INVALID_ID;
			m_xDownMs = nowMs;
			m_xHoldDone = FALSE;
		}
		else
		{
			showFeedback(singleProducer() ? "None of these in the queue" : "Select one building to change its queue");
		}
		return;
	}
	if (m_xHoldSlot >= 0)
	{
		// The hold belongs to the building it started on: a different selection (mouse, keyboard
		// group) or a changed button ends it, so "all" never lands on another queue.
		Object *producer = singleProducer();
		if (!(held & m_xHoldButton) || !haveEntry || entry.slot != m_xHoldSlot ||
			!producer || producer->getID() != m_xHoldProducer || !isFocusedCommandSeen(entry, nowMs))
		{
			m_xHoldSlot = -1;
		}
		else if (!m_xHoldDone && nowMs - m_xDownMs >= HOLD_CANCEL_ALL_MS)
		{
			m_xHoldDone = TRUE;
			if (cancelQueued(command, TRUE) > 0)
				showFeedback("Removed all of them from the queue");
		}
	}
}

//-------------------------------------------------------------------------------------------------
// Drawing
//-------------------------------------------------------------------------------------------------
void GameController::drawOneWheel(Int side, const std::vector<PanelEntry> &entries, Int firstIndex, Int cx, Int cy, Real outerR, Bool dual)
{
	const Int n = (Int)entries.size();
	const Real innerR = outerR * 0.42f;
	const Real midR = (outerR + innerR) * 0.5f;
	const Int lineHeight = textLineHeight();
	Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;

	// Which slot of this wheel is highlighted, if any. Of two wheels, the current one (the one A
	// uses) is bright; the other keeps a dim highlight.
	Bool highlighted = FALSE;
	Int highlightSlot = -1;
	Bool isCurrent = TRUE;
	if (dual)
	{
		highlighted = m_dualActive[side];
		highlightSlot = m_dualSlot[side];
		isCurrent = m_dualCurrent == side;
	}
	else
	{
		highlighted = m_wheelHighlight;
		highlightSlot = m_panelSlot[m_panelKind][m_panelRegion];
	}

	// --- ring ---------------------------------------------------------------------------------
	const Real fillWidth = (Real)(2.0 * PI * outerR * RING_STEP_DEG / 360.0) * 1.3f;
	const Real width = n > 0 ? 360.0f / n : 360.0f;
	for (Int i = 0; i < (n > 0 ? n : 1); ++i)
	{
		UnsignedInt inner = GameMakeColor(8, 32, 64, 190);
		UnsignedInt outer = GameMakeColor(28, 96, 168, 210);
		if (n > 0)
		{
			const Bool focused = highlighted && entries[i].slot == highlightSlot;
			const Bool disabled = entries[i].window && !BitIsSet(entries[i].window->winGetStatus(), WIN_STATUS_ENABLED);
			if (focused && isCurrent)
			{
				inner = GameMakeColor(150, 80, 10, 220);
				outer = GameMakeColor(255, 170, 40, 235);
			}
			else if (focused)
			{
				inner = GameMakeColor(70, 50, 20, 200);
				outer = GameMakeColor(150, 110, 50, 215);
			}
			else if (disabled)
			{
				inner = GameMakeColor(24, 24, 28, 190);
				outer = GameMakeColor(70, 72, 80, 205);
			}
		}
		const Real start = i * width - width * 0.5f;
		for (Real a = start + RING_STEP_DEG * 0.5f; a < start + width; a += RING_STEP_DEG)
		{
			Int x0, y0, x1, y1;
			wheelPoint(cx, cy, a, innerR, &x0, &y0);
			wheelPoint(cx, cy, a, outerR, &x1, &y1);
			TheDisplay->drawLine(x0, y0, x1, y1, fillWidth, inner, outer);
		}
	}

	// Outlines and sector separators.
	const UnsignedInt edge = GameMakeColor(170, 215, 255, 210);
	const Int circleSteps = 72;
	for (Int s = 0; s < circleSteps; ++s)
	{
		const Real a0 = 360.0f * s / circleSteps;
		const Real a1 = 360.0f * (s + 1) / circleSteps;
		Int x0, y0, x1, y1;
		wheelPoint(cx, cy, a0, outerR, &x0, &y0);
		wheelPoint(cx, cy, a1, outerR, &x1, &y1);
		TheDisplay->drawLine(x0, y0, x1, y1, 2.0f, edge);
		wheelPoint(cx, cy, a0, innerR, &x0, &y0);
		wheelPoint(cx, cy, a1, innerR, &x1, &y1);
		TheDisplay->drawLine(x0, y0, x1, y1, 2.0f, edge);
	}
	if (n > 1)
	{
		for (Int i = 0; i < n; ++i)
		{
			Int x0, y0, x1, y1;
			const Real a = i * width - width * 0.5f;
			wheelPoint(cx, cy, a, innerR, &x0, &y0);
			wheelPoint(cx, cy, a, outerR, &x1, &y1);
			TheDisplay->drawLine(x0, y0, x1, y1, 2.0f, GameMakeColor(220, 235, 255, 160));
		}
	}

	// --- sector contents: the button's own icon, else a text label; queue count badges --------
	Real iconBox = outerR - innerR;
	if (n > 0)
	{
		const Real arc = (Real)(2.0 * PI * midR / n);
		if (arc < iconBox)
			iconBox = arc;
	}
	iconBox *= 0.66f;

	for (Int i = 0; i < n; ++i)
	{
		const Int labelIndex = firstIndex + i;
		Int sx, sy;
		wheelPoint(cx, cy, i * width, (n == 1) ? (innerR + outerR) * 0.5f : midR, &sx, &sy);
		const PanelEntry &e = entries[i];
		const Bool disabled = e.window && !BitIsSet(e.window->winGetStatus(), WIN_STATUS_ENABLED);

		const Image *image = e.window ? e.window->winGetEnabledImage(0) : nullptr;
		if (image && image->getImageWidth() > 0 && image->getImageHeight() > 0)
		{
			// Fit into the box, keeping the picture's shape.
			const Real aspect = (Real)image->getImageWidth() / (Real)image->getImageHeight();
			Real w = iconBox, h = iconBox;
			if (aspect >= 1.0f)
				h = iconBox / aspect;
			else
				w = iconBox * aspect;
			const Color tint = disabled ? GameMakeColor(110, 110, 110, 255) : GameMakeColor(255, 255, 255, 255);
			TheDisplay->drawImage(image, sx - REAL_TO_INT(w * 0.5f), sy - REAL_TO_INT(h * 0.5f),
				sx + REAL_TO_INT(w * 0.5f), sy + REAL_TO_INT(h * 0.5f), tint);

			// How many of this are in the building's queue: a plain number on the picture's corner.
			const Int queued = queuedCount((const CommandButton *)GadgetButtonGetData(e.window));
			if (queued > 0 && labelIndex < MAX_LABELLED_SECTORS)
			{
				DisplayString *&badge = lineAt(m_wheelLines, LINE_BADGE_FIRST + labelIndex);
				if (!badge)
					badge = makeString();
				if (badge)
				{
					ensureFont();
					if (badge->getFont() != m_font)
						badge->setFont(m_font);
					UnicodeString text;
					text.format(L"%d", queued);
					if (badge->getText() != text)
						badge->setText(text);
					const Int bw = badge->getWidth() + 8;
					const Int bx = sx + REAL_TO_INT(w * 0.5f) - bw + 4;
					const Int by = sy - REAL_TO_INT(h * 0.5f) - 4;
					TheDisplay->drawFillRect(bx, by, bw, lineHeight, GameMakeColor(0, 0, 0, 210));
					TheDisplay->drawOpenRect(bx, by, bw, lineHeight, 1.0f, GameMakeColor(255, 220, 60, 255));
					badge->draw(bx + 4, by, GameMakeColor(255, 230, 120, 255), GameMakeColor(0, 0, 0, 0));
				}
			}
			continue;
		}

		// Text sectors: groups, orders, or a button without a picture.
		if (labelIndex >= MAX_LABELLED_SECTORS)
			continue;
		UnicodeString label;
		if (m_panelKind == PANEL_GROUPS)
		{
			const Int group = groupForWheelSlot(e.slot);
			Squad *squad = player ? player->getHotkeySquad(group) : nullptr;
			const Int count = squad ? (Int)squad->getLiveObjects().size() : 0;
			if (count > 0)
				label.format(L"%d: %d", group, count);
			else
				label.format(L"%d: -", group);
		}
		else
		{
			label = panelEntryName(e);
		}
		DisplayString *&str = lineAt(m_wheelLines, LINE_SECTOR_FIRST + labelIndex);
		if (!str)
			str = makeString();
		if (!str)
			continue;
		ensureFont();
		if (str->getFont() != m_font)
			str->setFont(m_font);
		if (str->getText() != label)
			str->setText(label);
		const Int tw = str->getWidth();
		str->draw(sx - tw / 2, sy - lineHeight / 2, disabled ? GameMakeColor(150, 150, 150, 255) : GameMakeColor(255, 255, 255, 255),
			GameMakeColor(0, 0, 0, 200));
	}

	// --- centre: what this wheel points at ---------------------------------------------------
	UnicodeString name, info;
	PanelEntry focus;
	Bool haveFocus = FALSE;
	for (Int i = 0; highlighted && i < n; ++i)
	{
		if (entries[i].slot == highlightSlot)
		{
			focus = entries[i];
			haveFocus = TRUE;
		}
	}
	if (n == 0)
	{
		name = toUnicodeText(m_panelKind == PANEL_POWERS && m_panelRegion == REGION_POWERS_PROMOTE ? "No promotions" : "Nothing here");
	}
	else if (!haveFocus)
	{
		name = toUnicodeText(dual ? (side == 0 ? "Left stick" : "Right stick") : regionTitle(m_panelKind, m_panelRegion));
		info = toUnicodeText(dual ? "points here" : "Point with the left stick");
	}
	else
	{
		name = panelEntryName(focus);
		const CommandButton *command = focus.window ? (const CommandButton *)GadgetButtonGetData(focus.window) : nullptr;
		const Int queued = queuedCount(command);
		if (focus.window && !BitIsSet(focus.window->winGetStatus(), WIN_STATUS_ENABLED))
			info = toUnicodeText("Not available");
		else if (dual && !isCurrent)
			info = toUnicodeText("(move this stick to use)");
		else if (queued > 0)
			info.format(L"%d in the queue - X or LB: remove one", queued);
		else if (m_panelKind == PANEL_GROUPS)
		{
			Squad *squad = player ? player->getHotkeySquad(groupForWheelSlot(focus.slot)) : nullptr;
			const Int count = squad ? (Int)squad->getLiveObjects().size() : 0;
			if (count > 0)
				info.format(L"%d unit%s", count, count == 1 ? L"" : L"s");
			else
				info = toUnicodeText("Empty");
		}
		else if (m_panelKind == PANEL_POWERS && m_panelRegion == REGION_POWERS_PROMOTE && player)
			info.format(L"%d point%s to spend", player->getSciencePurchasePoints(), player->getSciencePurchasePoints() == 1 ? L"" : L"s");
	}

	// The info line can be an instruction ("X or LB: remove one"): it names the buttons of the
	// current layout. The name line is a unit or group name and is shown as it is.
	const UnicodeString centerText[2] = { name, buttonNamesFor(info) };
	for (Int k = 0; k < 2; ++k)
	{
		if (centerText[k].isEmpty())
			continue;
		DisplayString *&str = lineAt(m_wheelLines, (k == 0 ? LINE_CENTER_NAME : LINE_CENTER_INFO) + side);
		if (!str)
			str = makeString();
		if (!str)
			continue;
		ensureFont();
		if (str->getFont() != m_font)
			str->setFont(m_font);
		if (str->getText() != centerText[k])
			str->setText(centerText[k]);
		const Int tw = str->getWidth();
		const Int y = cy - lineHeight + k * (lineHeight + 2);
		str->draw(cx - tw / 2, y, k == 0 ? GameMakeColor(255, 220, 120, 255) : GameMakeColor(220, 230, 240, 255), GameMakeColor(0, 0, 0, 220));
	}
}

void GameController::drawWheel()
{
	if (!m_panelOpen || m_panelKind < 0 || !TheDisplay || !TheTacticalView)
		return;

	Int wheels = 1;
	std::vector<PanelEntry> side0, side1;
	collectWheelEntries(0, &side0, &wheels);
	if (wheels > 1)
		collectWheelEntries(1, &side1, nullptr);

	Int ox = 0, oy = 0;
	TheTacticalView->getOrigin(&ox, &oy);
	const Int viewW = TheTacticalView->getWidth();
	const Int viewH = TheTacticalView->getHeight();
	const Int lineHeight = textLineHeight();

	Int top, bottom;
	if (wheels == 1)
	{
		// One wheel in the middle of the play area.
		const Real outerR = viewH * 0.36f;
		const Int cx = ox + viewW / 2;
		const Int cy = oy + viewH / 2;
		drawOneWheel(0, side0, 0, cx, cy, outerR, FALSE);
		top = cy - REAL_TO_INT(outerR);
		bottom = cy + REAL_TO_INT(outerR);
	}
	else
	{
		// Two wheels: left stick on the left, right stick on the right.
		const Real outerR = viewH * 0.30f;
		const Int cy = oy + REAL_TO_INT(viewH * 0.46f);
		drawOneWheel(0, side0, 0, ox + REAL_TO_INT(viewW * 0.29f), cy, outerR, TRUE);
		drawOneWheel(1, side1, (Int)side0.size(), ox + REAL_TO_INT(viewW * 0.71f), cy, outerR, TRUE);
		top = cy - REAL_TO_INT(outerR);
		bottom = cy + REAL_TO_INT(outerR);
	}

	// --- title above, legend below, across the middle ----------------------------------------
	const Int cx = ox + viewW / 2;
	AsciiString title = regionTitle(m_panelKind, m_panelRegion);
	AsciiString sections;
	if (m_panelKind != PANEL_GROUPS)
	{
		for (Int r = 0; r < panelRegionCount(m_panelKind); ++r)
		{
			if (r == m_panelRegion || !isPanelRegionAvailable(m_panelKind, r))
				continue;
			sections.concat(sections.isEmpty() ? "D-pad up/down: " : ", ");
			sections.concat(regionShortName(m_panelKind, r));
		}
	}

	// The legend only names what applies to this wheel right now.
	Bool anyQueueable = FALSE;
	if (m_panelKind == PANEL_COMMANDS && m_panelRegion == REGION_COMMANDS && singleProducer())
	{
		for (Int s = 0; s < 2 && !anyQueueable; ++s)
		{
			const std::vector<PanelEntry> &list = (s == 0) ? side0 : side1;
			for (size_t i = 0; i < list.size() && !anyQueueable; ++i)
			{
				if (list[i].window && isQueueable((const CommandButton *)GadgetButtonGetData(list[i].window)))
					anyQueueable = TRUE;
			}
		}
	}

	AsciiString legend;
	const char *stickText = (wheels > 1) ? "Either stick: point   L-stick click or A:" : "Left stick: point   A or L-stick click:";
	if (m_panelKind == PANEL_GROUPS)
	{
		if (m_groupClearPending >= 0)
			legend.format("Clear group %d?   X: yes   any other button: keep it", m_groupClearPending);
		else
			legend = "Left stick: point   A: select group   Y: assign selection   X: clear   RT: camera   B: close";
	}
	else if (m_panelKind == PANEL_POWERS && m_panelRegion == REGION_POWERS_PROMOTE)
		legend.format("%s buy   B or Y: close", stickText);
	else if (m_panelKind == PANEL_COMMANDS && m_panelRegion == REGION_ORDERS)
		legend.format("%s choose   B or Y: close", stickText);
	else if (anyQueueable)
		legend.format("%s build   X or LB: remove one (hold: all)   B or Y: close", stickText);
	else
		legend.format("%s use   B or Y: close", stickText);

	const char *texts[3] = { title.str(), sections.str(), legend.str() };
	const Int lines[3] = { LINE_TITLE, LINE_SECTIONS, LINE_LEGEND };
	const Int ys[3] = { top - lineHeight * 2 - 6, top - lineHeight - 4, bottom + 6 };
	const Color colors[3] = { GameMakeColor(255, 220, 60, 255), GameMakeColor(200, 210, 220, 255), GameMakeColor(255, 220, 60, 255) };
	for (Int k = 0; k < 3; ++k)
	{
		if (!texts[k] || !*texts[k])
			continue;
		DisplayString *&str = lineAt(m_wheelLines, lines[k]);
		if (!str)
			str = makeString();
		if (!str)
			continue;
		ensureFont();
		if (str->getFont() != m_font)
			str->setFont(m_font);
		// The legend names the buttons of the current layout; the title and the
		// section list name no remappable buttons.
		const UnicodeString text = (lines[k] == LINE_LEGEND) ? buttonNamesFor(toUnicodeText(texts[k])) : toUnicodeText(texts[k]);
		if (str->getText() != text)
			str->setText(text);
		const Int tw = str->getWidth();
		str->draw(cx - tw / 2, ys[k], colors[k], GameMakeColor(0, 0, 0, 220));
	}
}
