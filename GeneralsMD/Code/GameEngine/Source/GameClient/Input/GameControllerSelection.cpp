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

// GameControllerSelection.cpp ////////////////////////////////////////////////////////////////////
// ControllerMod @feature Selection and orders with the game controller.
//
//   A tap          select the own unit or building under the reticle; empty ground clears
//   A double tap   select visible own units of the same type (native double click)
//   A hold         paint brush: own units touched by a growing circle, committed on release
//   X              the native context order at the reticle (same evaluator as a mouse click)
//   B              cancel the gesture / armed command / placement, otherwise clear selection
//   LB / RB        army across the map / on screen (native "select all" filter: no dozers,
//                  harvesters or IGNORES_SELECT_ALL, and never structures)
//   RT             cycle All -> each unit type -> All within the selected group
//
// Selection always uses the complete native transaction: InGameUI select/deselect plus the
// MSG_CREATE_SELECTED_GROUP / MSG_DESTROY_SELECTED_GROUP messages, so the visible selection and
// the one the command pipeline uses never differ. Orders go through
// GameClient::evaluateContextCommand, like the mouse and the minimap. At most one selection or
// order action happens per frame, because the command translator only learns about a new
// selection when its message has been processed.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameController.h"
#include "GameClient/ControllerMath.h"

#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingTemplate.h"
#include "GameClient/Color.h"
#include "GameClient/CommandXlat.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameText.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Mouse.h"
#include "GameClient/SelectionInfo.h"
#include "GameClient/SelectionXlat.h"
#include "GameClient/View.h"
#include "Common/AcademyStats.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Object.h"

#include <algorithm>

namespace
{
	const UnsignedInt FEEDBACK_MS = 1500;
	const UnsignedInt BUMPER_CHORD_MS = 100;   ///< how long one bumper waits for the other (LB+RB)
	const Real LINE_SAMPLE_DISTANCE = 8.0f;    ///< line move: world distance between recorded points
	const Real LINE_MIN_LENGTH = 30.0f;        ///< line move: shorter lines keep the normal order
	const Int LINE_MAX_POINTS = 512;

	struct LineUnit
	{
		ObjectID id;
		Real along;   ///< position along the line's direction, to hand out points without crossings
	};

	bool lineUnitLess(const LineUnit &a, const LineUnit &b)
	{
		return a.along < b.along;
	}

	UnicodeString toUnicodeText(const char *text)
	{
		UnicodeString u;
		u.translate(AsciiString(text));
		return u;
	}

	/// What a click at this target would do. In force-attack mode (Ctrl for the mouse, the Orders
	/// region for the controller) the native click uses the force-attack evaluator instead.
	GameMessage::Type evaluateOrder(Drawable *draw, const Coord3D *pos, CommandTranslator::CommandEvaluateType type)
	{
		if (TheInGameUI->isInForceAttackMode())
			return TheGameClient->evaluateForceAttack(draw, pos, type);
		return TheGameClient->evaluateContextCommand(draw, pos, type);
	}

	Drawable *drawableForObject(ObjectID id)
	{
		if (id == INVALID_ID || !TheGameLogic)
			return nullptr;
		Object *obj = TheGameLogic->findObjectByID(id);
		return obj ? obj->getDrawable() : nullptr;
	}

	/// Own units the brush may take: the native drag-select rules (own, visible, selectable,
	/// never structures) plus not inside a container.
	Bool isBrushable(Drawable *draw)
	{
		if (!draw || !CanSelectDrawable(draw, TRUE))
			return FALSE;
		const Object *obj = draw->getObject();
		return obj && !obj->isContained() && obj->isMassSelectable();
	}

	/// Own unit or building a tap may select.
	Bool isTapSelectable(Drawable *draw)
	{
		if (!draw || !CanSelectDrawable(draw, FALSE))
			return FALSE;
		const Object *obj = draw->getObject();
		return obj && obj->isLocallyControlled() && !obj->isContained();
	}

	struct BrushCollect
	{
		Real cx, cy, radius;
		Real pxPerWorld;   ///< screen pixels per world unit at the reticle
		Real padding;      ///< extra hit area in pixels
		std::vector<ObjectID> *ids;
	};

	Bool brushCallback(Drawable *draw, void *userData)
	{
		BrushCollect *c = (BrushCollect *)userData;
		if (!isBrushable(draw))
			return FALSE;
		ICoord2D s;
		if (!TheTacticalView->worldToScreen(draw->getPosition(), &s))
			return FALSE;
		// A unit is touched when the circle reaches its footprint, not only its centre: its
		// geometry radius on screen plus a little padding.
		const Real unitRadius = draw->getObject()->getGeometryInfo().getBoundingCircleRadius() * c->pxPerWorld + c->padding;
		const Real reach = c->radius + unitRadius;
		const Real dx = s.x - c->cx;
		const Real dy = s.y - c->cy;
		if (dx * dx + dy * dy > reach * reach)
			return FALSE;
		const ObjectID id = draw->getObject()->getID();
		if (std::find(c->ids->begin(), c->ids->end(), id) == c->ids->end())
			c->ids->push_back(id);
		return TRUE;
	}

	bool lessByTemplateName(const ThingTemplate *a, const ThingTemplate *b)
	{
		return strcmp(a->getName().str(), b->getName().str()) < 0;
	}

	/// Anything the mouse could hover and click: visible, not shrouded, alive, selectable by the
	/// native rules (own, allied, neutral or enemy).
	Bool isHoverEligible(Drawable *draw)
	{
		if (!draw || draw->isDrawableEffectivelyHidden() || draw->getFullyObscuredByShroud())
			return FALSE;
		const Object *obj = draw->getObject();
		if (!obj || (obj->isEffectivelyDead() && !obj->isKindOf(KINDOF_ALWAYS_SELECTABLE)))
			return FALSE;
		return CanSelectDrawable(draw, FALSE);
	}

	struct AssistCollect
	{
		Real cx, cy, radiusSq;
		Drawable *best;
		Real bestDistSq;
		ObjectID bestID;
	};

	Bool assistCallback(Drawable *draw, void *userData)
	{
		AssistCollect *c = (AssistCollect *)userData;
		if (!isHoverEligible(draw))
			return FALSE;
		ICoord2D s;
		if (!TheTacticalView->worldToScreen(draw->getPosition(), &s))
			return FALSE;
		const Real dx = s.x - c->cx;
		const Real dy = s.y - c->cy;
		const Real d2 = dx * dx + dy * dy;
		if (d2 > c->radiusSq)
			return FALSE;
		const ObjectID id = draw->getObject()->getID();
		// Nearest wins; equal distances are broken by ObjectID, never by list order.
		if (!c->best || d2 < c->bestDistSq || (d2 == c->bestDistSq && id < c->bestID))
		{
			c->best = draw;
			c->bestDistSq = d2;
			c->bestID = id;
		}
		return TRUE;
	}
}

//-------------------------------------------------------------------------------------------------
/** What the reticle points at. The object directly under the crosshair always wins. Otherwise
	the current target is kept while it stays within the retention radius, so the bracket does not
	flicker between neighbours, and only then is the nearest eligible object within the acquisition
	radius taken. Uses only what the mouse could see and click. */
//-------------------------------------------------------------------------------------------------
Drawable *GameController::findHoverDrawable(UnsignedInt previousID, Bool *assisted) const
{
	*assisted = FALSE;

	Real rx, ry;
	getReticleScreenPos(&rx, &ry);
	ICoord2D pixel;
	pixel.x = REAL_TO_INT(rx);
	pixel.y = REAL_TO_INT(ry);

	Drawable *direct = TheTacticalView->pickDrawable(&pixel, FALSE, PICK_TYPE_SELECTABLE);
	if (isHoverEligible(direct))
		return direct;

	if (m_settings.assistRadius <= 0.0f)
		return nullptr;

	const Real scale = uiScale();
	if (previousID != 0)
	{
		Drawable *previous = TheGameClient->findDrawableByID((DrawableID)previousID);
		ICoord2D s;
		if (isHoverEligible(previous) && TheTacticalView->worldToScreen(previous->getPosition(), &s))
		{
			const Real dx = s.x - rx;
			const Real dy = s.y - ry;
			const Real keep = m_settings.assistRetention * scale;
			if (dx * dx + dy * dy <= keep * keep)
			{
				*assisted = TRUE;
				return previous;
			}
		}
	}

	const Real radius = m_settings.assistRadius * scale;
	const Int r = REAL_TO_INT(radius) + 1;
	IRegion2D region;
	region.lo.x = pixel.x - r;
	region.lo.y = pixel.y - r;
	region.hi.x = pixel.x + r;
	region.hi.y = pixel.y + r;

	AssistCollect collect;
	collect.cx = rx;
	collect.cy = ry;
	collect.radiusSq = radius * radius;
	collect.best = nullptr;
	collect.bestDistSq = 0.0f;
	collect.bestID = INVALID_ID;
	TheTacticalView->iterateDrawablesInRegion(&region, assistCallback, &collect);
	if (collect.best)
		*assisted = TRUE;
	return collect.best;
}

//-------------------------------------------------------------------------------------------------
/** Buttons while the battlefield owns the controller (after Menu, help and right stick click). */
//-------------------------------------------------------------------------------------------------
void GameController::handleWorldButtons(UnsignedInt pressed, UnsignedInt released, UnsignedInt held, UnsignedInt nowMs)
{
	// B wins over A and X pressed in the same frame: it always means back / cancel.
	if (pressed & ControllerState::BUTTON_B)
	{
		if (m_lineState == LINE_DRAWING)
		{
			m_lineState = LINE_IDLE;
			m_linePoints.clear();
			m_orderFeedback = toUnicodeText("Line cancelled");
			m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
			return;
		}
		m_lineState = LINE_IDLE;
		cancelOrDeselect();
		return;
	}

	// Clicking the left stick in while holding X for a line switches it between move and guard.
	if ((pressed & ControllerState::BUTTON_LS) && m_lineState != LINE_IDLE && (held & ControllerState::BUTTON_X))
	{
		m_lineGuard = !m_lineGuard;
		m_orderFeedback = toUnicodeText(m_lineGuard ? "Line: GUARD (click the left stick again: move)" : "Line: MOVE (click the left stick again: guard)");
		m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
		return;
	}

	// Letting go of X after drawing a line sends the units along it.
	if ((released & ControllerState::BUTTON_X) && m_lineState != LINE_IDLE)
	{
		if (m_lineState == LINE_DRAWING && m_lineGeneration == m_contextGeneration)
			finishLineMove();
		m_lineState = LINE_IDLE;
		m_linePoints.clear();
		return;
	}

	if (pressed & ControllerState::BUTTON_A)
	{
		// Capture the candidate the player can see bracketed right now. It is revalidated on release.
		m_bumperPending = 0;
		m_lineState = LINE_IDLE;
		m_gesture = GESTURE_PRESSED;
		m_gestureDownMs = nowMs;
		m_gestureGeneration = m_contextGeneration;
		m_brushIDs.clear();
		m_tapCandidate = INVALID_ID;
		Drawable *hover = m_hoverDrawableID ? TheGameClient->findDrawableByID((DrawableID)m_hoverDrawableID) : nullptr;
		if (hover && hover->getObject())
			m_tapCandidate = hover->getObject()->getID();
	}

	if ((released & ControllerState::BUTTON_A) && m_gesture != GESTURE_IDLE)
	{
		if (m_gestureGeneration != m_contextGeneration)
			cancelGesture();
		else if (m_gesture == GESTURE_BRUSH)
			commitBrush();
		else
			onTapReleased(nowMs);
		m_gesture = GESTURE_IDLE;
		m_brushIDs.clear();
		return;
	}

	// While A is down nothing else selects or orders, so a gesture can never also send an order.
	if (m_gesture != GESTURE_IDLE || (held & ControllerState::BUTTON_A))
		return;

	if (pressed & ControllerState::BUTTON_X)
	{
		// A quick second X turns the order into guard at that spot, like the native double click.
		if (m_settings.doubleTapXGuard && m_lastOrderMs != 0 && m_lastOrderGeneration == m_contextGeneration &&
			nowMs - m_lastOrderMs <= (UnsignedInt)m_settings.doubleTapMs)
		{
			issueGuard();
			m_lastOrderMs = 0;
			m_lineState = LINE_IDLE;
		}
		else
		{
			issueOrder();
			m_lastOrderMs = nowMs;
			m_lastOrderGeneration = m_contextGeneration;
			// Keep holding X and pan: a line (see updateLineMove).
			beginLineCandidate(nowMs);
		}
		return;
	}
	// LB and RB pressed together unload the selected vehicles. A single bumper waits a moment for
	// its partner before it selects the army, so the chord never also changes the selection.
	const UnsignedInt bumpers = ControllerState::BUTTON_LB | ControllerState::BUTTON_RB;
	if ((pressed & bumpers) && (held & bumpers) == bumpers)
	{
		m_bumperPending = 0;
		evacuateSelectedVehicles();
		return;
	}
	if (pressed & bumpers)
	{
		m_bumperPending = pressed & bumpers;
		m_bumperDownMs = nowMs;
		m_bumperGeneration = m_contextGeneration;
	}
	if (m_bumperPending)
	{
		if (m_bumperGeneration != m_contextGeneration)
		{
			m_bumperPending = 0;
		}
		else if (!(held & m_bumperPending) || nowMs - m_bumperDownMs >= BUMPER_CHORD_MS)
		{
			const Bool acrossMap = (m_bumperPending & ControllerState::BUTTON_LB) != 0;
			m_bumperPending = 0;
			selectArmy(acrossMap);
			return;
		}
	}
	if (pressed & ControllerState::BUTTON_RT)
	{
		cycleTypeSubset();
		return;
	}
}

//-------------------------------------------------------------------------------------------------
void GameController::cancelGesture()
{
	m_gesture = GESTURE_IDLE;
	m_brushIDs.clear();
	m_tapCandidate = INVALID_ID;
	m_brushRadius = 0.0f;
}

//-------------------------------------------------------------------------------------------------
/** B: back out of the most transient thing first, and only then clear the selection. B never
	cancels paid work such as production or construction. */
//-------------------------------------------------------------------------------------------------
void GameController::cancelOrDeselect()
{
	if (m_gesture != GESTURE_IDLE)
	{
		cancelGesture();
		m_orderFeedback = toUnicodeText("Selection cancelled");
		m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
		return;
	}
	if (TheInGameUI->getGUICommand())
	{
		// Same as the native right click deselect path.
		TheInGameUI->setGUICommand(nullptr);
		m_orderFeedback = toUnicodeText("Command cancelled");
		m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
		return;
	}
	if (TheInGameUI->getPendingPlaceType())
	{
		TheInGameUI->placeBuildAvailable(nullptr, nullptr);
		return;
	}
	if (TheInGameUI->getSelectCount() > 0)
	{
		TheInGameUI->deselectAllDrawables();
		captureCohortFromSelection();
	}
}

//-------------------------------------------------------------------------------------------------
/** A released before the hold threshold: select immediately, or same-type select on a quick
	second tap. A tap on empty ground clears the selection; a tap on something that is not ours
	(or that died meanwhile) keeps the selection unchanged. */
//-------------------------------------------------------------------------------------------------
void GameController::onTapReleased(UnsignedInt nowMs)
{
	if (m_tapCandidate == INVALID_ID)
	{
		if (TheInGameUI->getSelectCount() > 0)
			TheInGameUI->deselectAllDrawables();
		captureCohortFromSelection();
		m_lastTapObject = INVALID_ID;
		return;
	}

	Object *obj = TheGameLogic->findObjectByID(m_tapCandidate);
	Drawable *draw = obj ? obj->getDrawable() : nullptr;
	if (!obj || !isTapSelectable(draw))
	{
		if (!obj || obj->isEffectivelyDead())
		{
			m_orderFeedback = toUnicodeText("Target lost");
			m_orderFeedbackUntilMs = nowMs + FEEDBACK_MS;
		}
		m_lastTapObject = INVALID_ID;
		return;
	}

	Bool sameTypeSelect = FALSE;
	if (m_lastTapObject != INVALID_ID && m_lastTapGeneration == m_contextGeneration &&
		nowMs - m_lastTapReleaseMs <= (UnsignedInt)m_settings.doubleTapMs && draw->isMassSelectable())
	{
		const Object *previous = TheGameLogic->findObjectByID(m_lastTapObject);
		sameTypeSelect = previous && previous->getTemplate()->isEquivalentTo(obj->getTemplate());
	}

	if (sameTypeSelect)
	{
		// Exactly the native double click: this one, then all matching units on screen.
		selectSingle(draw, FALSE);
		TheInGameUI->selectMatchingAcrossScreen();
		m_lastTapObject = INVALID_ID;   // a third tap starts over
	}
	else
	{
		selectSingle(draw, TRUE);
		m_lastTapObject = obj->getID();
		m_lastTapReleaseMs = nowMs;
		m_lastTapGeneration = m_contextGeneration;

		// A tapped building opens its command wheel straight away. The command bar
		// fills in for the new selection a frame or so later; updatePendingBuildingWheel waits for it.
		if (m_settings.wheelMenus && m_settings.openWheelOnBuilding && obj->isKindOf(KINDOF_STRUCTURE) && obj->isLocallyControlled())
		{
			m_pendingWheelObject = obj->getID();
			m_pendingWheelUntilMs = nowMs + 700;
		}
	}
	captureCohortFromSelection();
}

//-------------------------------------------------------------------------------------------------
/** Open the command wheel for a building just selected with A, once the command bar is showing
	that building's commands. Gives up quietly if the selection changes, something else takes
	over, or the building has no commands. */
//-------------------------------------------------------------------------------------------------
void GameController::updatePendingBuildingWheel(UnsignedInt nowMs)
{
	if (m_pendingWheelObject == INVALID_ID)
		return;

	const ObjectID id = m_pendingWheelObject;
	Object *obj = TheGameLogic->findObjectByID(id);
	Drawable *draw = obj ? obj->getDrawable() : nullptr;
	const Bool stillSelected = draw && draw->isSelected() && TheInGameUI->getSelectCount() == 1;
	if (!stillSelected || m_panelOpen || isPlacementMode() || isTargetingMode() || m_orderMode != ORDERMODE_NONE ||
		m_gesture != GESTURE_IDLE || (Int)(nowMs - m_pendingWheelUntilMs) > 0)
	{
		m_pendingWheelObject = INVALID_ID;
		return;
	}

	if (!TheControlBar || !TheControlBar->isDrivingContextUI(draw))
		return;   // the command bar has not caught up with the new selection yet
	if (!isPanelRegionAvailable(PANEL_COMMANDS, REGION_COMMANDS))
		return;   // keep waiting until the timeout; some buildings have no commands at all

	m_pendingWheelObject = INVALID_ID;
	openPanel(PANEL_COMMANDS);
}

//-------------------------------------------------------------------------------------------------
void GameController::selectSingle(Drawable *draw, Bool playSound)
{
	TheInGameUI->deselectAllDrawables();
	TheInGameUI->selectDrawable(draw);
	GameMessage *msg = TheMessageStream->appendMessage(playSound ?
		GameMessage::MSG_CREATE_SELECTED_GROUP : GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
	msg->appendBooleanArgument(TRUE);
	msg->appendObjectIDArgument(draw->getObject()->getID());
	TheInGameUI->clearAttackMoveToMode();
}

//-------------------------------------------------------------------------------------------------
/** LB / RB. Uses the native "select all" filter (the Q hotkey): no dozers, harvesters or
	IGNORES_SELECT_ALL units, and only mass-selectable units, so never structures. The previous
	selection is replaced, like the native patch 1.03 fix does for mixed selections. */
//-------------------------------------------------------------------------------------------------
void GameController::selectArmy(Bool acrossMap)
{
	KindOfMaskType requiredKindofs;
	KindOfMaskType disqualifyingKindofs;
	disqualifyingKindofs.set(KINDOF_DOZER);
	disqualifyingKindofs.set(KINDOF_HARVESTER);
	disqualifyingKindofs.set(KINDOF_IGNORES_SELECT_ALL);

	TheInGameUI->deselectAllDrawables();
	if (acrossMap)
		TheInGameUI->selectAllUnitsByTypeAcrossMap(requiredKindofs, disqualifyingKindofs);
	else
		TheInGameUI->selectAllUnitsByTypeAcrossScreen(requiredKindofs, disqualifyingKindofs);
	TheInGameUI->clearAttackMoveToMode();

	captureCohortFromSelection();
	if (m_cohort.empty())
	{
		m_orderFeedback = toUnicodeText(acrossMap ? "No army units" : "No army units on screen");
		m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
	}
}

//-------------------------------------------------------------------------------------------------
/** Brush on release: the touched units that are still selectable replace the selection. An
	empty brush clears it, like a tap on empty ground. */
//-------------------------------------------------------------------------------------------------
void GameController::commitBrush()
{
	std::vector<ObjectID> ids;
	for (size_t i = 0; i < m_brushIDs.size(); ++i)
	{
		if (isBrushable(drawableForObject(m_brushIDs[i])))
			ids.push_back(m_brushIDs[i]);
	}
	applySelection(ids);
	captureCohortFromSelection();
}

//-------------------------------------------------------------------------------------------------
void GameController::collectBrush()
{
	Real rx, ry;
	getReticleScreenPos(&rx, &ry);

	// Pixels per world unit at the reticle, to put unit footprints on screen.
	Real pxPerWorld = 1.0f;
	Coord2D right, down;
	if (computeReticleBasis(rx, ry, &right, &down))
	{
		const Real worldPerPx = (Real)sqrt((double)(right.x * right.x + right.y * right.y));
		if (worldPerPx > 1e-5f)
			pxPerWorld = 1.0f / worldPerPx;
	}
	const Real padding = m_settings.brushUnitPadding * uiScale();
	// Search wide enough for the biggest units whose footprint can reach into the circle.
	const Int r = REAL_TO_INT(m_brushRadius + padding + 40.0f * pxPerWorld) + 1;

	IRegion2D region;
	region.lo.x = REAL_TO_INT(rx) - r;
	region.lo.y = REAL_TO_INT(ry) - r;
	region.hi.x = REAL_TO_INT(rx) + r;
	region.hi.y = REAL_TO_INT(ry) + r;

	BrushCollect collect;
	collect.cx = rx;
	collect.cy = ry;
	collect.radius = m_brushRadius;
	collect.pxPerWorld = pxPerWorld;
	collect.padding = padding;
	collect.ids = &m_brushIDs;
	TheTacticalView->iterateDrawablesInRegion(&region, brushCallback, &collect);
}

//-------------------------------------------------------------------------------------------------
/** Per frame: turn a long press into the brush, grow it, and gather units under it. */
//-------------------------------------------------------------------------------------------------
void GameController::updateSelectionGesture(UnsignedInt nowMs)
{
	if (m_gesture == GESTURE_IDLE)
		return;

	if (m_gestureGeneration != m_contextGeneration)
	{
		cancelGesture();
		return;
	}

	const UnsignedInt heldMs = nowMs - m_gestureDownMs;
	if (m_gesture == GESTURE_PRESSED && heldMs >= (UnsignedInt)m_settings.holdSelectMs)
	{
		m_gesture = GESTURE_BRUSH;
		m_brushIDs.clear();
	}

	if (m_gesture == GESTURE_BRUSH)
	{
		Real t = 1.0f;
		if (m_settings.brushGrowMs > 0)
			t = ControllerMath::clampf(0.0f, (Real)(heldMs - m_settings.holdSelectMs) / (Real)m_settings.brushGrowMs, 1.0f);
		m_brushRadius = (m_settings.brushRadiusMin + (m_settings.brushRadiusMax - m_settings.brushRadiusMin) * t) * uiScale();
		collectBrush();
	}
}

//-------------------------------------------------------------------------------------------------
void GameController::readSelectedIDs(std::vector<ObjectID> *out) const
{
	out->clear();
	const DrawableList *selected = TheInGameUI->getAllSelectedDrawables();
	for (DrawableListCIt it = selected->begin(); it != selected->end(); ++it)
	{
		const Drawable *draw = *it;
		const Object *obj = draw ? draw->getObject() : nullptr;
		if (obj && obj->isLocallyControlled())
			out->push_back(obj->getID());
	}
	std::sort(out->begin(), out->end());
}

//-------------------------------------------------------------------------------------------------
/** A new selection gesture (or a selection made some other way) starts a new RT cohort. */
//-------------------------------------------------------------------------------------------------
void GameController::captureCohortFromSelection()
{
	readSelectedIDs(&m_cohort);
	m_appliedSelection = m_cohort;
	m_typeIndex = 0;
	m_typeShown = nullptr;
	m_typeLabel.clear();
}

//-------------------------------------------------------------------------------------------------
/** Replace the selection with exactly these objects, as one native transaction. */
//-------------------------------------------------------------------------------------------------
void GameController::applySelection(const std::vector<ObjectID> &ids)
{
	TheInGameUI->deselectAllDrawables();
	TheInGameUI->setDisplayedMaxWarning(FALSE);

	std::vector<ObjectID> chosen;
	for (size_t i = 0; i < ids.size(); ++i)
	{
		Drawable *draw = drawableForObject(ids[i]);
		if (!draw || !draw->getObject())
			continue;

		// Respect the optional selection size limit, like every native selection path.
		if (TheInGameUI->getMaxSelectCount() > 0 && TheInGameUI->getSelectCount() >= TheInGameUI->getMaxSelectCount())
		{
			if (!TheInGameUI->getDisplayedMaxWarning())
			{
				TheInGameUI->setDisplayedMaxWarning(TRUE);
				UnicodeString text;
				text.format(TheGameText->fetch("GUI:MaxSelectionSize").str(), TheInGameUI->getMaxSelectCount());
				TheInGameUI->message(text);
			}
			break;
		}

		TheInGameUI->selectDrawable(draw);
		chosen.push_back(ids[i]);
	}
	sendLogicSelection(chosen, TRUE);
	TheInGameUI->clearAttackMoveToMode();
}

//-------------------------------------------------------------------------------------------------
/** Tell the game logic that these objects are the selection, with the native selection message.
	A GameMessage holds at most 255 arguments (its count is a byte), so a big selection goes in
	several messages: the first starts a new group and the others add to it. Only the first can
	play the selection voice. Nothing is sent for an empty list. */
//-------------------------------------------------------------------------------------------------
void GameController::sendLogicSelection(const std::vector<ObjectID> &ids, Bool playSound)
{
	const size_t MAX_IDS_PER_MESSAGE = 254;   // plus the "new group" flag makes 255 arguments
	for (size_t first = 0; first < ids.size(); first += MAX_IDS_PER_MESSAGE)
	{
		GameMessage *msg = TheMessageStream->appendMessage((playSound && first == 0) ?
			GameMessage::MSG_CREATE_SELECTED_GROUP : GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
		msg->appendBooleanArgument(first == 0);
		for (size_t i = first; i < ids.size() && i < first + MAX_IDS_PER_MESSAGE; ++i)
			msg->appendObjectIDArgument(ids[i]);
	}
}

//-------------------------------------------------------------------------------------------------
/** RT: All -> each type present (stable order) -> All. The parent cohort is remembered, so going
	back to All restores everyone who is still alive and selectable. */
//-------------------------------------------------------------------------------------------------
void GameController::cycleTypeSubset()
{
	// A selection changed by the mouse, a group hotkey, etc. starts a new cohort. Members the game
	// dropped by itself (died, entered a transport, became unselectable) do not: the rest of the
	// cohort stays.
	std::vector<ObjectID> current;
	readSelectedIDs(&current);
	std::vector<ObjectID> expected;
	for (size_t i = 0; i < m_appliedSelection.size(); ++i)
	{
		if (isTapSelectable(drawableForObject(m_appliedSelection[i])))
			expected.push_back(m_appliedSelection[i]);
	}
	if (current != expected)
		captureCohortFromSelection();

	// Prune members that died, were captured, entered something or became unselectable.
	std::vector<ObjectID> alive;
	std::vector<const ThingTemplate *> types;
	for (size_t i = 0; i < m_cohort.size(); ++i)
	{
		Drawable *draw = drawableForObject(m_cohort[i]);
		if (!isTapSelectable(draw))
			continue;
		alive.push_back(m_cohort[i]);
		const ThingTemplate *tt = draw->getObject()->getTemplate();
		Bool known = FALSE;
		for (size_t t = 0; t < types.size() && !known; ++t)
			known = types[t]->isEquivalentTo(tt);
		if (!known)
			types.push_back(tt);
	}
	m_cohort = alive;
	std::sort(types.begin(), types.end(), lessByTemplateName);

	// Where the cycle is now, in the (possibly smaller) list of types. The type shown may be gone
	// entirely: then the next step is All of the survivors.
	Int position = 0;
	if (m_typeShown)
	{
		position = -1;
		for (size_t t = 0; t < types.size(); ++t)
		{
			if (types[t]->isEquivalentTo(m_typeShown))
				position = (Int)t + 1;
		}
	}
	std::vector<ObjectID> everyone = m_cohort;
	std::sort(everyone.begin(), everyone.end());
	const Int next = ControllerMath::nextTypeCycleIndex(position, (Int)types.size(), current == everyone);
	if (next < 0)
	{
		m_orderFeedback = toUnicodeText(m_cohort.empty() ? "Nothing selected" : "Only one unit type selected");
		m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
		return;
	}
	m_typeIndex = next;

	std::vector<ObjectID> subset;
	const ThingTemplate *wanted = m_typeIndex > 0 ? types[m_typeIndex - 1] : nullptr;
	for (size_t i = 0; i < m_cohort.size(); ++i)
	{
		const Object *obj = TheGameLogic->findObjectByID(m_cohort[i]);
		if (obj && (!wanted || obj->getTemplate()->isEquivalentTo(wanted)))
			subset.push_back(m_cohort[i]);
	}

	applySelection(subset);
	readSelectedIDs(&m_appliedSelection);
	m_typeShown = wanted;

	UnicodeString label;
	if (wanted)
		label.format(L"%s (%d)", wanted->getDisplayName().str(), (Int)subset.size());
	else
		label.format(L"All types (%d)", (Int)subset.size());
	m_typeLabel = m_typeIndex > 0 ? label : UnicodeString::TheEmptyString;
	m_orderFeedback = label;
	m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
}

//-------------------------------------------------------------------------------------------------
/** The target for X, exactly as a mouse click would pick it at the reticle pixel. Target assist
	only substitutes a nearby object when the evaluator has something specific to do with it
	(attack, enter, repair, ...), never to turn a move next to your own unit into nothing. */
//-------------------------------------------------------------------------------------------------
Bool GameController::resolveOrderTarget(Drawable **target, Coord3D *pos)
{
	*target = nullptr;

	Real rx, ry;
	getReticleScreenPos(&rx, &ry);
	ICoord2D pixel;
	pixel.x = REAL_TO_INT(rx);
	pixel.y = REAL_TO_INT(ry);

	if (!TheTacticalView->screenToTerrain(&pixel, pos))
		return FALSE;

	const Bool forceAttack = TheInGameUI->isInForceAttackMode();
	Drawable *draw = TheTacticalView->pickDrawable(&pixel, forceAttack, (PickType)getPickTypesForContext(forceAttack));

	// Same as the native click: dead units must not block positional commands.
	Object *obj = draw ? draw->getObject() : nullptr;
	if (!obj || (obj->isEffectivelyDead() && !obj->isKindOf(KINDOF_ALWAYS_SELECTABLE)))
		draw = nullptr;

	// With a command armed, behave exactly like a mouse click: no assist, no fallback (see above).
	const Bool guiCommandArmed = TheInGameUI->getGUICommand() != nullptr;

	if (!draw && !guiCommandArmed && !forceAttack && m_hoverAssisted && m_hoverDrawableID)
	{
		Drawable *assist = TheGameClient->findDrawableByID((DrawableID)m_hoverDrawableID);
		Object *assistObj = assist ? assist->getObject() : nullptr;
		if (assistObj && !assistObj->isEffectivelyDead())
		{
			const GameMessage::Type t = TheGameClient->evaluateContextCommand(assist, assist->getPosition(), CommandTranslator::EVALUATE_ONLY);
			if (t != GameMessage::MSG_INVALID && t != GameMessage::MSG_DO_MOVETO && t != GameMessage::MSG_DO_MOVETO_HINT)
			{
				draw = assist;
				*pos = *assist->getPosition();
			}
		}
	}

	// A click on one of our own units with nothing specific to do would do nothing natively;
	// for X, fall back to the ground under it so X is always "go there" at worst.
	if (draw && !guiCommandArmed && evaluateOrder(draw, pos, CommandTranslator::EVALUATE_ONLY) == GameMessage::MSG_INVALID)
		draw = nullptr;

	*target = draw;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** X: one native context order per press. */
//-------------------------------------------------------------------------------------------------
Int GameController::issueOrder()
{
	const UnsignedInt now = timeGetTime();
	const CommandButton *command = TheInGameUI->getGUICommand();
	const Bool controllable = TheInGameUI->areSelectedObjectsControllable() ||
		(command && command->getCommandType() == GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT);
	if (!controllable)
	{
		m_orderFeedback = toUnicodeText("Select units first (A, LB or RB)");
		m_orderFeedbackUntilMs = now + FEEDBACK_MS;
		return GameMessage::MSG_INVALID;
	}

	Drawable *draw = nullptr;
	Coord3D pos;
	if (!resolveOrderTarget(&draw, &pos))
	{
		m_orderFeedback = toUnicodeText("No ground under the reticle");
		m_orderFeedbackUntilMs = now + FEEDBACK_MS;
		return GameMessage::MSG_INVALID;
	}

	const GameMessage::Type t = evaluateOrder(draw, &pos, CommandTranslator::DO_COMMAND);
	TheInGameUI->clearAttackMoveToMode();

	const char *label = orderHintLabel(t);
	m_orderFeedback = toUnicodeText(t == GameMessage::MSG_INVALID ? "No order possible here" : (label ? label : "Order given"));
	m_orderFeedbackUntilMs = now + FEEDBACK_MS;
	return t;
}

//-------------------------------------------------------------------------------------------------
/** Double-tap X: exactly the native double-click guard (CommandTranslator, MSG_MOUSE_RIGHT_DOUBLE_CLICK
	with double-click attack move): guard the ground position under the reticle. */
//-------------------------------------------------------------------------------------------------
void GameController::issueGuard()
{
	if (!TheInGameUI->areSelectedObjectsControllable())
		return;

	Real rx, ry;
	getReticleScreenPos(&rx, &ry);
	ICoord2D pixel;
	pixel.x = REAL_TO_INT(rx);
	pixel.y = REAL_TO_INT(ry);
	Coord3D pos;
	if (!TheTacticalView->screenToTerrain(&pixel, &pos))
		return;

	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_GUARD_POSITION);
	msg->appendLocationArgument(pos);
	msg->appendIntegerArgument(GUARDMODE_NORMAL);

	ThePlayerList->getLocalPlayer()->getAcademyStats()->recordDoubleClickAttackMoveOrderGiven();

	// The native hint circle is placed at the mouse position; put it where the guard order went.
	const ICoord2D mousePos = TheMouse->getMouseStatus()->pos;
	TheMouse->setPosition(pixel.x, pixel.y);
	TheInGameUI->triggerDoubleClickAttackMoveGuardHint();
	TheMouse->setPosition(mousePos.x, mousePos.y);

	m_orderFeedback = toUnicodeText("Guard");
	m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
}

//-------------------------------------------------------------------------------------------------
/** LB+RB: unload the selected vehicles and garrisoned buildings, exactly like
	their Evacuate button. Only objects whose own command set has Evacuate and that hold someone
	take part. The native Evacuate message acts on the whole selected group (and flying aircraft
	in it would be told to stop and "unload" where they are), so when only some of the selection
	qualifies, the game's selection is narrowed to those objects for the order and then restored,
	the same way line move gives each unit its own order. The visible selection never changes. */
//-------------------------------------------------------------------------------------------------
void GameController::evacuateSelectedVehicles()
{
	std::vector<ObjectID> loadedIDs;
	DrawableList loadedDraws;
	const DrawableList *selected = TheInGameUI->getAllSelectedDrawables();
	for (DrawableList::const_iterator it = selected->begin(); it != selected->end(); ++it)
	{
		Object *obj = (*it) ? (*it)->getObject() : nullptr;
		if (!obj || !obj->isLocallyControlled() || obj->isEffectivelyDead())
			continue;
		ContainModuleInterface *contain = obj->getContain();
		if (!contain || contain->getContainCount() <= 0 || obj->isDisabledByType(DISABLED_SUBDUED))
			continue;

		const CommandSet *commands = TheControlBar ? TheControlBar->findCommandSet(obj->getCommandSetString()) : nullptr;
		Bool canEvacuate = FALSE;
		for (Int i = 0; commands && i < MAX_COMMANDS_PER_SET && !canEvacuate; ++i)
		{
			const CommandButton *button = commands->getCommandButton(i);
			canEvacuate = button && button->getCommandType() == GUI_COMMAND_EVACUATE;
		}
		if (canEvacuate)
		{
			loadedIDs.push_back(obj->getID());
			loadedDraws.push_back(*it);
		}
	}

	const Int loaded = (Int)loadedIDs.size();
	if (loaded == 0)
	{
		m_orderFeedback = toUnicodeText("LB+RB: nothing selected has anyone inside");
		m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
		return;
	}

	std::vector<ObjectID> everyone;
	readSelectedIDs(&everyone);
	const Bool narrowed = loadedIDs.size() != everyone.size();

	pickAndPlayUnitVoiceResponse(&loadedDraws, GameMessage::MSG_EVACUATE);
	if (narrowed)
		sendLogicSelection(loadedIDs, FALSE);
	TheMessageStream->appendMessage(GameMessage::MSG_EVACUATE);
	if (narrowed)
		sendLogicSelection(everyone, FALSE);

	UnicodeString text;
	if (loaded == 1)
		text = toUnicodeText("Unloading");
	else
		text.format(L"Unloading %d", loaded);
	m_orderFeedback = text;
	m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
}

//-------------------------------------------------------------------------------------------------
// Line move: hold X and pan to draw a line on the
// ground; on release the selected units spread evenly along it. The X press itself already gave
// the normal order, so a short press is unchanged. Each unit gets the ordinary move order to its
// own point (a one-unit MSG_CREATE_SELECTED_GROUP followed by MSG_DO_MOVETO), and the whole
// selection is then restored, all with messages normal play sends.
//-------------------------------------------------------------------------------------------------
Bool GameController::reticleGround(Coord3D *pos) const
{
	Real rx, ry;
	getReticleScreenPos(&rx, &ry);
	ICoord2D pixel;
	pixel.x = REAL_TO_INT(rx);
	pixel.y = REAL_TO_INT(ry);
	return TheTacticalView->screenToTerrain(&pixel, pos);
}

/// The selected units a line can move: ours, alive, able to move and not inside something.
void GameController::collectLineUnits(std::vector<ObjectID> *out) const
{
	out->clear();
	const DrawableList *selected = TheInGameUI->getAllSelectedDrawables();
	for (DrawableListCIt it = selected->begin(); it != selected->end(); ++it)
	{
		const Object *obj = (*it) ? (*it)->getObject() : nullptr;
		if (!obj || !obj->isLocallyControlled() || obj->isEffectivelyDead() || obj->isContained())
			continue;
		if (obj->isKindOf(KINDOF_STRUCTURE) || obj->isKindOf(KINDOF_IMMOBILE) || !obj->getAI())
			continue;
		out->push_back(obj->getID());
	}
}

void GameController::beginLineCandidate(UnsignedInt nowMs)
{
	m_lineState = LINE_IDLE;
	m_linePoints.clear();
	m_lineGuard = FALSE;
	if (!m_settings.lineMove)
		return;
	std::vector<ObjectID> units;
	collectLineUnits(&units);
	Coord3D start;
	if (units.size() < 2 || !reticleGround(&start))
		return;
	m_linePoints.push_back(start);
	m_lineState = LINE_PRESSED;
	m_lineDownMs = nowMs;
	m_lineGeneration = m_contextGeneration;
}

//-------------------------------------------------------------------------------------------------
/** Per frame: after the hold time the line follows the reticle over the ground as the camera pans. */
//-------------------------------------------------------------------------------------------------
void GameController::updateLineMove(UnsignedInt nowMs, UnsignedInt held)
{
	if (m_lineState == LINE_IDLE)
		return;
	if (!(held & ControllerState::BUTTON_X) || m_lineGeneration != m_contextGeneration || m_panelOpen ||
		isPlacementMode() || isTargetingMode() || !isActiveInput() || m_linePoints.empty())
	{
		m_lineState = LINE_IDLE;
		m_linePoints.clear();
		return;
	}

	if (m_lineState == LINE_PRESSED)
	{
		if (nowMs - m_lineDownMs < (UnsignedInt)m_settings.lineMoveHoldMs)
			return;
		m_lineState = LINE_DRAWING;
	}

	Coord3D pos;
	if (!reticleGround(&pos))
		return;
	const Coord3D &last = m_linePoints.back();
	const Real dx = pos.x - last.x;
	const Real dy = pos.y - last.y;
	if (dx * dx + dy * dy >= LINE_SAMPLE_DISTANCE * LINE_SAMPLE_DISTANCE && (Int)m_linePoints.size() < LINE_MAX_POINTS)
		m_linePoints.push_back(pos);

	std::vector<ObjectID> units;
	collectLineUnits(&units);
	UnicodeString text;
	text.format(L"Line %s (%d)   release X: send   L-stick click: %s   B: cancel",
		m_lineGuard ? L"GUARD" : L"MOVE", (Int)units.size(), m_lineGuard ? L"move" : L"guard");
	m_orderFeedback = text;
	m_orderFeedbackUntilMs = timeGetTime() + 200;
}

/// count points spaced evenly along the drawn line (ControllerMath, unit tested).
Bool GameController::computeLineTargets(Int count, std::vector<Coord3D> *out) const
{
	out->clear();
	const Int n = (Int)m_linePoints.size();
	if (n < 1 || count < 1)
		return FALSE;
	std::vector<Real> xs(n), ys(n), zs(n), ox(count), oy(count), oz(count);
	for (Int i = 0; i < n; ++i)
	{
		xs[i] = m_linePoints[i].x;
		ys[i] = m_linePoints[i].y;
		zs[i] = m_linePoints[i].z;
	}
	if (!ControllerMath::spacePointsAlongPath(&xs[0], &ys[0], &zs[0], n, count, &ox[0], &oy[0], &oz[0]))
		return FALSE;
	for (Int k = 0; k < count; ++k)
	{
		Coord3D c;
		c.x = ox[k];
		c.y = oy[k];
		c.z = oz[k];
		out->push_back(c);
	}
	return TRUE;
}

void GameController::finishLineMove()
{
	std::vector<ObjectID> ids;
	collectLineUnits(&ids);
	const Int n = (Int)m_linePoints.size();
	if (ids.size() < 2 || n < 2)
		return;

	std::vector<Real> xs(n), ys(n);
	for (Int i = 0; i < n; ++i)
	{
		xs[i] = m_linePoints[i].x;
		ys[i] = m_linePoints[i].y;
	}
	if (ControllerMath::pathLength(&xs[0], &ys[0], n) < LINE_MIN_LENGTH)
	{
		m_orderFeedback = toUnicodeText("Line too short - normal order kept");
		m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
		return;
	}

	std::vector<Coord3D> targets;
	if (!computeLineTargets((Int)ids.size(), &targets))
		return;

	// Hand the points out in order along the line's overall direction, so paths do not cross.
	Real dirX = m_linePoints.back().x - m_linePoints.front().x;
	Real dirY = m_linePoints.back().y - m_linePoints.front().y;
	std::vector<LineUnit> units;
	for (size_t i = 0; i < ids.size(); ++i)
	{
		Object *obj = TheGameLogic->findObjectByID(ids[i]);
		if (!obj)
			continue;
		LineUnit u;
		u.id = ids[i];
		u.along = (obj->getPosition()->x - m_linePoints.front().x) * dirX + (obj->getPosition()->y - m_linePoints.front().y) * dirY;
		units.push_back(u);
	}
	std::stable_sort(units.begin(), units.end(), lineUnitLess);

	for (size_t i = 0; i < units.size() && i < targets.size(); ++i)
	{
		GameMessage *group = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
		group->appendBooleanArgument(TRUE);
		group->appendObjectIDArgument(units[i].id);
		if (m_lineGuard)
		{
			// The same guard order as the Guard button or a double click: go there and guard it.
			GameMessage *guard = TheMessageStream->appendMessage(GameMessage::MSG_DO_GUARD_POSITION);
			guard->appendLocationArgument(targets[i]);
			guard->appendIntegerArgument(GUARDMODE_NORMAL);
		}
		else
		{
			GameMessage *move = TheMessageStream->appendMessage(GameMessage::MSG_DO_MOVETO);
			move->appendLocationArgument(targets[i]);
		}
	}

	// The game's own selection is the whole selection again.
	std::vector<ObjectID> selected;
	readSelectedIDs(&selected);
	sendLogicSelection(selected, FALSE);

	pickAndPlayUnitVoiceResponse(TheInGameUI->getAllSelectedDrawables(),
		m_lineGuard ? GameMessage::MSG_DO_GUARD_POSITION : GameMessage::MSG_DO_MOVETO);
	TheInGameUI->clearAttackMoveToMode();
	m_lastOrderMs = 0;   // the next X is a new order, not a guard double tap

	UnicodeString text;
	text.format(m_lineGuard ? L"Line guard: %d units" : L"Line move: %d units", (Int)units.size());
	m_orderFeedback = text;
	m_orderFeedbackUntilMs = timeGetTime() + FEEDBACK_MS;
}

void GameController::drawLineMove()
{
	if (m_lineState != LINE_DRAWING || m_linePoints.empty())
		return;

	// The drawn line on the ground: yellow to move, blue to guard.
	const UnsignedInt lineColor = m_lineGuard ? GameMakeColor(90, 170, 255, 230) : GameMakeColor(255, 220, 60, 220);
	ICoord2D prev;
	Bool havePrev = FALSE;
	for (size_t i = 0; i < m_linePoints.size(); ++i)
	{
		ICoord2D s;
		if (!TheTacticalView->worldToScreen(&m_linePoints[i], &s))
		{
			havePrev = FALSE;
			continue;
		}
		if (havePrev)
			TheDisplay->drawLine(prev.x, prev.y, s.x, s.y, 3.0f, lineColor);
		prev = s;
		havePrev = TRUE;
	}

	// Where each unit will go.
	std::vector<ObjectID> units;
	collectLineUnits(&units);
	std::vector<Coord3D> targets;
	if (!computeLineTargets((Int)units.size(), &targets))
		return;
	const Int r = REAL_TO_INT(5.0f * uiScale()) < 3 ? 3 : REAL_TO_INT(5.0f * uiScale());
	for (size_t i = 0; i < targets.size(); ++i)
	{
		ICoord2D s;
		if (!TheTacticalView->worldToScreen(&targets[i], &s))
			continue;
		TheDisplay->drawFillRect(s.x - r - 1, s.y - r - 1, r * 2 + 2, r * 2 + 2, GameMakeColor(0, 0, 0, 200));
		TheDisplay->drawFillRect(s.x - r, s.y - r, r * 2, r * 2, m_lineGuard ? GameMakeColor(90, 170, 255, 255) : GameMakeColor(120, 255, 120, 255));
	}
}

//-------------------------------------------------------------------------------------------------
/** What X would do right now, for the label next to the reticle. EVALUATE_ONLY never sends
	anything. The beacon command appends a hint even when evaluating, so it is skipped. */
//-------------------------------------------------------------------------------------------------
void GameController::updateOrderHint()
{
	m_orderHint = GameMessage::MSG_INVALID;
	if (m_gesture != GESTURE_IDLE || TheInGameUI->getSelectCount() == 0)
		return;

	// With a command armed from the command bar (a mouse feature) the evaluator appends
	// cursor hint messages even when only evaluating, which would fight the mouse cursor. X still
	// works then; it just shows no label.
	if (TheInGameUI->getGUICommand())
		return;
	if (!TheInGameUI->areSelectedObjectsControllable())
		return;

	Drawable *draw = nullptr;
	Coord3D pos;
	if (!resolveOrderTarget(&draw, &pos))
		return;
	m_orderHint = evaluateOrder(draw, &pos, CommandTranslator::EVALUATE_ONLY);
}

//-------------------------------------------------------------------------------------------------
const char *GameController::orderHintLabel(Int messageType)
{
	switch (messageType)
	{
		case GameMessage::MSG_DO_MOVETO:
		case GameMessage::MSG_DO_MOVETO_HINT:                    return "Move";
		case GameMessage::MSG_DO_ATTACKMOVETO:
		case GameMessage::MSG_DO_ATTACKMOVETO_HINT:              return "Attack-move";
		case GameMessage::MSG_DO_ATTACK_OBJECT:
		case GameMessage::MSG_DO_ATTACK_OBJECT_HINT:             return "Attack";
		case GameMessage::MSG_DO_ATTACK_OBJECT_AFTER_MOVING_HINT: return "Move and attack";
		case GameMessage::MSG_DO_FORCE_ATTACK_OBJECT:
		case GameMessage::MSG_DO_FORCE_ATTACK_OBJECT_HINT:       return "Force attack";
		case GameMessage::MSG_DO_FORCE_ATTACK_GROUND:
		case GameMessage::MSG_DO_FORCE_ATTACK_GROUND_HINT:       return "Attack ground";
		case GameMessage::MSG_ENTER:
		case GameMessage::MSG_ENTER_HINT:                        return "Enter";
		case GameMessage::MSG_DOCK:
		case GameMessage::MSG_DOCK_HINT:                         return "Dock";
		case GameMessage::MSG_DO_REPAIR:
		case GameMessage::MSG_DO_REPAIR_HINT:                    return "Repair";
		case GameMessage::MSG_GET_REPAIRED:
		case GameMessage::MSG_GET_REPAIRED_HINT:                 return "Get repaired";
		case GameMessage::MSG_GET_HEALED:
		case GameMessage::MSG_GET_HEALED_HINT:                   return "Get healed";
		case GameMessage::MSG_RESUME_CONSTRUCTION:
		case GameMessage::MSG_RESUME_CONSTRUCTION_HINT:          return "Resume building";
		case GameMessage::MSG_CAPTUREBUILDING_HINT:              return "Capture";
		case GameMessage::MSG_HIJACK_HINT:                       return "Hijack";
		case GameMessage::MSG_HACK_HINT:                         return "Hack";
		case GameMessage::MSG_SABOTAGE_HINT:                     return "Sabotage";
		case GameMessage::MSG_CONVERT_TO_CARBOMB:
		case GameMessage::MSG_CONVERT_TO_CARBOMB_HINT:           return "Make car bomb";
		case GameMessage::MSG_DO_SALVAGE:
		case GameMessage::MSG_DO_SALVAGE_HINT:                   return "Salvage";
		case GameMessage::MSG_SET_RALLY_POINT:
		case GameMessage::MSG_SET_RALLY_POINT_HINT:              return "Set rally point";
		case GameMessage::MSG_ADD_WAYPOINT:
		case GameMessage::MSG_ADD_WAYPOINT_HINT:                 return "Add waypoint";
		case GameMessage::MSG_IMPOSSIBLE_ATTACK_HINT:            return "Cannot attack";
		case GameMessage::MSG_DO_INVALID_HINT:                   return "Cannot go there";
		case GameMessage::MSG_VALID_GUICOMMAND_HINT:             return "Use command";
		case GameMessage::MSG_INVALID_GUICOMMAND_HINT:           return "Command not possible here";
		default:                                                 return nullptr;
	}
}

//-------------------------------------------------------------------------------------------------
// Drawing
//-------------------------------------------------------------------------------------------------
void GameController::drawBrush(Int cx, Int cy)
{
	if (m_gesture != GESTURE_BRUSH || m_brushRadius <= 0.0f)
		return;

	const UnsignedInt ring = GameMakeColor(120, 255, 120, 200);
	const Int segments = 32;
	Real px = cx + m_brushRadius;
	Real py = (Real)cy;
	for (Int i = 1; i <= segments; ++i)
	{
		const Real a = (2.0f * PI * i) / segments;
		const Real nx = cx + m_brushRadius * (Real)cos((double)a);
		const Real ny = cy + m_brushRadius * (Real)sin((double)a);
		TheDisplay->drawLine(REAL_TO_INT(px), REAL_TO_INT(py), REAL_TO_INT(nx), REAL_TO_INT(ny), 2.0f, ring);
		px = nx;
		py = ny;
	}

	// Mark the units the brush has collected so far.
	const Int m = REAL_TO_INT(5.0f * uiScale()) < 3 ? 3 : REAL_TO_INT(5.0f * uiScale());
	for (size_t i = 0; i < m_brushIDs.size(); ++i)
	{
		Drawable *draw = drawableForObject(m_brushIDs[i]);
		ICoord2D s;
		if (draw && TheTacticalView->worldToScreen(draw->getPosition(), &s))
			TheDisplay->drawOpenRect(s.x - m, s.y - m, m * 2, m * 2, 2.0f, ring);
	}
}

void GameController::drawSelectionInfo(Int cx, Int cy)
{
	const Int lineHeight = textLineHeight();
	const Int x = cx + REAL_TO_INT(30.0f * uiScale());
	Int y = cy + REAL_TO_INT(30.0f * uiScale()) + lineHeight;   // below the hover name line

	for (Int i = 0; i < 3; ++i)
	{
		if (!m_infoLines[i])
			m_infoLines[i] = makeString();
	}

	// What X would do; right after an order, a second X means guard.
	const char *hint = orderHintLabel(m_orderHint);
	if (hint && m_settings.doubleTapXGuard && m_lastOrderMs != 0 && timeGetTime() - m_lastOrderMs <= (UnsignedInt)m_settings.doubleTapMs)
		hint = "Guard (X again)";
	if (hint)
	{
		UnicodeString text = toUnicodeText("X: ");
		text.concat(toUnicodeText(hint));
		drawText(m_infoLines[0], text, x, y, GameMakeColor(255, 220, 120, 240));
		y += lineHeight;
	}

	// Selection size and the active RT type.
	const Int count = TheInGameUI->getSelectCount();
	if (count > 0)
	{
		UnicodeString text;
		if (m_typeLabel.isEmpty())
			text.format(L"%d selected", count);
		else
			text.format(L"%d selected - %s", count, m_typeLabel.str());
		drawText(m_infoLines[1], text, x, y, GameMakeColor(200, 255, 200, 230));
		y += lineHeight;
	}

	// Short feedback after an action.
	if (!m_orderFeedback.isEmpty() && timeGetTime() < m_orderFeedbackUntilMs)
		drawText(m_infoLines[2], m_orderFeedback, x, y, GameMakeColor(255, 255, 255, 230));
}
