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

// GameControllerTestState.cpp ////////////////////////////////////////////////////////////////////
// ControllerMod @test Automated testing only (builds configured with CONTROLLERMOD_TEST_INPUT=ON;
// player builds compile none of this).
//
// A test script cannot see the screen the way a player does, so the controller writes what it
// knows as plain text:
// - CONTROLLERMOD_TEST_STATE set: controllermod_state.txt, rewritten twice a second.
// - a SNAP:label test step: the same block appended to controllermod_snaps.txt at that moment.
// A second game copy on the same PC writes controllermod_state_Instance02.txt and so on.
// The text names the screen and menu focus, money and power, camera, what the reticle is on, the
// open wheel with every slice (angle, name, internal name, available), placement or targeting,
// the selection, the player's objects (with build progress) and the control groups.
//
// AIM:Name|Name test steps push the stick of the open wheel towards the slice whose name or
// internal name contains one of the Names, so a script can pick a slice on any faction's wheel.
// The controller's own stick-to-slice code still does the choosing.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameController.h"

#if CONTROLLERMOD_ENABLE_TEST_INPUT

#include "Common/Energy.h"
#include "Common/Money.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingTemplate.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Drawable.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameWindow.h"
#include "GameClient/InGameUI.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Squad.h"

#include <map>
#include <string>

namespace
{
	const UnsignedInt STATE_INTERVAL_MS = 500;

	std::string lowerCase(const char *text)
	{
		std::string s(text ? text : "");
		for (size_t i = 0; i < s.size(); ++i)
			s[i] = (char)tolower((unsigned char)s[i]);
		return s;
	}

	std::string ascii(const UnicodeString &text)
	{
		AsciiString a;
		a.translate(text);
		return std::string(a.str());
	}

	/// True when one of the '|' separated names (lower case) is part of text (any case).
	Bool matchesAny(const char *names, const std::string &text)
	{
		const std::string lower = lowerCase(text.c_str());
		const std::string all = lowerCase(names);
		size_t start = 0;
		while (start <= all.size())
		{
			size_t end = all.find('|', start);
			if (end == std::string::npos)
				end = all.size();
			const std::string name = all.substr(start, end - start);
			if (!name.empty() && lower.find(name) != std::string::npos)
				return TRUE;
			start = end + 1;
		}
		return FALSE;
	}

	/// controllermod_<base>.txt, or controllermod_<base>_InstanceNN.txt for a second game copy.
	std::string instanceFileName(const char *base)
	{
		char name[96];
		if (rts::ClientInstance::getInstanceId() > 1u)
			snprintf(name, sizeof(name), "controllermod_%s_Instance%.2u.txt", base, rts::ClientInstance::getInstanceId());
		else
			snprintf(name, sizeof(name), "controllermod_%s.txt", base);
		return std::string(name);
	}

	const char *windowName(const GameWindow *win)
	{
		if (!win || !TheNameKeyGenerator)
			return "-";
		static AsciiString s_name;
		s_name = TheNameKeyGenerator->keyToName((NameKeyType)const_cast<GameWindow *>(win)->winGetWindowId());
		return s_name.isEmpty() ? "?" : s_name.str();
	}

	const char *relationName(Int relation)
	{
		switch (relation)
		{
			case 0: return "own";
			case 1: return "ally";
			case 2: return "neutral";
			case 3: return "enemy";
			default: return "none";
		}
	}

	const char *panelName(Int kind, Int region)
	{
		if (kind == 0)
			return region == 0 ? "commands" : (region == 1 ? "build-queue" : "orders");
		if (kind == 1)
			return region == 0 ? "powers" : "promotions";
		if (kind == 2)
			return "groups";
		return "none";
	}

	/// The nearest stick token (LSN .. LSNW) to a slice centred at degrees, if it lands well
	/// inside the slice; "-" otherwise.
	const char *compassFor(Real degrees, Real width)
	{
		static const char *const names[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
		Int best = -1;
		Real bestDelta = 360.0f;
		for (Int d = 0; d < 8; ++d)
		{
			Real delta = d * 45.0f - degrees;
			while (delta > 180.0f)
				delta -= 360.0f;
			while (delta <= -180.0f)
				delta += 360.0f;
			if (fabs(delta) < bestDelta)
			{
				bestDelta = (Real)fabs(delta);
				best = d;
			}
		}
		return (best >= 0 && bestDelta < width * 0.5f - 4.0f) ? names[best] : "-";
	}

	struct ObjectTally
	{
		std::map<std::string, Int> count;
		std::map<std::string, std::string> building;   ///< template -> build progress of unfinished ones
	};

	void tallyObject(Object *obj, void *userData)
	{
		ObjectTally *tally = (ObjectTally *)userData;
		if (!obj || obj->isEffectivelyDead() || !obj->getTemplate())
			return;
		const std::string name = obj->getTemplate()->getName().str();
		tally->count[name] += 1;
		if (obj->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION))
		{
			char pct[16];
			snprintf(pct, sizeof(pct), "%s%d%%", tally->building[name].empty() ? "" : ",", (Int)obj->getConstructionPercent());
			tally->building[name] += pct;
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** Everything a test script needs to know about the controller and the game right now. */
//-------------------------------------------------------------------------------------------------
void GameController::writeTestState(FILE *f, const char *label, UnsignedInt nowMs) const
{
	const Int testMs = m_testStartMs ? (Int)(nowMs - m_testStartMs) : -1;
	const Int battleMs = m_testBattleStartMs ? (Int)(nowMs - m_testBattleStartMs) : -1;
	const Bool inGame = TheGameLogic && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame();
	const Bool loading = TheGameLogic && TheGameLogic->isLoadingMap();
	const Bool battlefield = isBattlefieldContext();

	fprintf(f, "== %s test_ms=%d battle_ms=%d frame=%u\n", label, testMs, battleMs, TheGameLogic ? TheGameLogic->getFrame() : 0);
	fprintf(f, "context: %s%s%s%s%s%s%s\n",
		loading ? "loading" : (inGame ? (battlefield ? "battle" : "battle-menu") : "shell"),
		m_settingsOpen ? " settings-screen" : "", m_helpVisible ? " help" : "",
		(inGame && isMatchChatOpen()) ? " match-chat" : "",
		(inGame && isDisconnectScreenOpen()) ? " disconnect-screen" : "",
		(inGame && TheInGameUI && TheInGameUI->isQuitMenuVisible()) ? " quit-menu" : "",
		m_needNeutral ? " waiting-for-neutral-pad" : "");
	fprintf(f, "pad: connected=%d active=%d suspended=%d\n", (Int)m_state.connected, (Int)m_active, (Int)m_suspended);

	if (!battlefield && !loading)
	{
		std::vector<MenuTarget> targets;
		GameWindow *layer = nullptr;
		collectMenuTargets(&targets, &layer);
		const char *mode = m_menuMode == MENU_MODE_DROPDOWN ? "dropdown" : (m_menuMode == MENU_MODE_CELLS ? "medals" : "normal");
		std::string focus = "-";
		for (size_t i = 0; i < targets.size(); ++i)
		{
			if (targets[i].window == m_menuFocus)
				focus = windowName(targets[i].window);
		}
		fprintf(f, "menu: layer=%s focus=%s mode=%s\n", windowName(layer), focus.c_str(), mode);
		fprintf(f, "menu-stops:");
		for (size_t i = 0; i < targets.size(); ++i)
			fprintf(f, " %s", windowName(targets[i].window));
		fprintf(f, "\n");
		// The newest line of each chat box on screen.
		for (size_t i = 0; i < targets.size(); ++i)
		{
			if (!BitIsSet(targets[i].style, GWS_SCROLL_LISTBOX) || !strstr(windowName(targets[i].window), "Chat"))
				continue;
			const Int count = GadgetListBoxGetNumEntries(targets[i].window);
			if (count > 0)
				fprintf(f, "chat: %s \"%s\"\n", windowName(targets[i].window), ascii(GadgetListBoxGetText(targets[i].window, count - 1, 0)).c_str());
		}
		if (m_keyboardOpen)
		{
			fprintf(f, "keyboard: %s row=%d col=%d caps=%d box=%s text=\"%s\"\n", m_keyboardNumeric ? "numbers" : "letters",
				m_keyboardRow, m_keyboardCol, (Int)m_keyboardCaps, windowName(m_keyboardStop), ascii(m_keyboardShown).c_str());
		}
	}

	Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	if (inGame && player)
	{
		fprintf(f, "player: name=\"%s\" side=%s money=%u power=%d/%d rank=%d points=%d\n",
			ascii(player->getPlayerDisplayName()).c_str(), player->getSide().str(),
			player->getMoney()->countMoney(), player->getEnergy()->getProduction(),
			player->getEnergy()->getConsumption(), player->getRankLevel(), player->getSciencePurchasePoints());
	}
	if (inGame && TheTacticalView)
	{
		const Coord3D &pos = TheTacticalView->getPosition();
		fprintf(f, "camera: x=%.0f y=%.0f zoom=%.2f angle=%.2f\n", pos.x, pos.y, TheTacticalView->getZoom(), TheTacticalView->getAngle());
	}

	if (battlefield)
	{
		std::string over = "-";
		Drawable *hover = (m_hoverDrawableID && TheGameClient) ? TheGameClient->findDrawableByID((DrawableID)m_hoverDrawableID) : nullptr;
		if (hover && hover->getTemplate())
			over = hover->getTemplate()->getName().str();
		const char *order = orderHintLabel(m_orderHint);
		fprintf(f, "reticle: over=%s relation=%s assisted=%d order=\"%s\"\n", over.c_str(), relationName(m_hoverRelation),
			(Int)m_hoverAssisted, order ? order : "-");

		// Mode: what the next A / X / B does.
		std::string mode = "free";
		if (isPlacementMode())
			mode = std::string("placing ") + TheInGameUI->getPendingPlaceType()->getName().str();
		else if (isTargetingMode())
			mode = std::string("aiming ") + TheInGameUI->getGUICommand()->getName().str();
		else if (m_orderMode == ORDERMODE_FORCE_ATTACK)
			mode = "order-mode force-attack";
		else if (m_orderMode == ORDERMODE_WAYPOINTS)
			mode = "order-mode waypoints";
		else if (m_panelOpen)
			mode = std::string("wheel ") + panelName(m_panelKind, m_panelRegion);
		if (m_lineState == LINE_DRAWING)
			mode += m_lineGuard ? " +drawing-guard-line" : " +drawing-line";
		if (m_gesture == GESTURE_BRUSH)
			mode += " +painting";
		fprintf(f, "mode: %s\n", mode.c_str());

		if (m_panelOpen && m_panelKind >= 0)
		{
			Int wheels = 1;
			std::vector<PanelEntry> first;
			collectWheelEntries(0, &first, &wheels);
			for (Int side = 0; side < wheels; ++side)
			{
				std::vector<PanelEntry> entries;
				if (side == 0)
					entries = first;
				else
					collectWheelEntries(1, &entries, nullptr);
				const Int n = (Int)entries.size();
				const Real width = n > 0 ? 360.0f / n : 360.0f;
				fprintf(f, "wheel %d (%s stick, %d slices):\n", side, side == 0 ? "left" : "right", n);
				for (Int i = 0; i < n; ++i)
				{
					const PanelEntry &e = entries[i];
					const CommandButton *command = e.window ? (const CommandButton *)GadgetButtonGetData(e.window) : nullptr;
					const Bool enabled = !e.window || BitIsSet(e.window->winGetStatus(), WIN_STATUS_ENABLED);
					Bool focused = FALSE;
					if (wheels == 1)
						focused = m_wheelHighlight && e.slot == m_panelSlot[m_panelKind][m_panelRegion];
					else
						focused = m_dualActive[side] && e.slot == m_dualSlot[side];
					const Int queued = command ? queuedCount(command) : 0;
					fprintf(f, "  [%d] %3.0fdeg %-3s %s \"%s\" (%s)%s%s\n", i, i * width, compassFor(i * width, width),
						enabled ? "ok " : "off", ascii(panelEntryName(e)).c_str(),
						command ? command->getName().str() : "-",
						queued > 0 ? " queued" : "", focused ? (wheels == 1 || m_dualCurrent == side ? " <FOCUS" : " <focus(other wheel)") : "");
				}
			}
		}

		std::map<std::string, Int> selected;
		const DrawableList *list = TheInGameUI ? TheInGameUI->getAllSelectedDrawables() : nullptr;
		Int total = 0;
		if (list)
		{
			for (DrawableList::const_iterator it = list->begin(); it != list->end(); ++it)
			{
				if (*it && (*it)->getTemplate())
				{
					selected[(*it)->getTemplate()->getName().str()] += 1;
					++total;
				}
			}
		}
		fprintf(f, "selection: %d", total);
		for (std::map<std::string, Int>::const_iterator it = selected.begin(); it != selected.end(); ++it)
			fprintf(f, " %s x%d", it->first.c_str(), it->second);
		if (m_typeShown)
			fprintf(f, " (type cycle: %s)", m_typeShown->getName().str());
		fprintf(f, "\n");
	}

	if (inGame && player)
	{
		ObjectTally tally;
		player->iterateObjects(tallyObject, &tally);
		fprintf(f, "own:");
		for (std::map<std::string, Int>::const_iterator it = tally.count.begin(); it != tally.count.end(); ++it)
		{
			fprintf(f, " %s x%d", it->first.c_str(), it->second);
			std::map<std::string, std::string>::const_iterator b = tally.building.find(it->first);
			if (b != tally.building.end())
				fprintf(f, " (building %s)", b->second.c_str());
		}
		fprintf(f, "\n");

		fprintf(f, "groups:");
		for (Int g = 0; g < NUM_HOTKEY_SQUADS; ++g)
		{
			Squad *squad = player->getHotkeySquad(g);
			const Int size = squad ? (Int)squad->getLiveObjects().size() : 0;
			if (size > 0)
				fprintf(f, " %d:%d", g, size);
		}
		fprintf(f, "\n");
	}

	if (!m_orderFeedback.isEmpty() && nowMs < m_orderFeedbackUntilMs)
		fprintf(f, "feedback: \"%s\"\n", ascii(m_orderFeedback).c_str());
	fprintf(f, "\n");
}

//-------------------------------------------------------------------------------------------------
/** CONTROLLERMOD_TEST_STATE: rewrite the state file twice a second. Written to a temporary file
	and moved over the old one, so a reader never sees half a file. */
//-------------------------------------------------------------------------------------------------
void GameController::updateTestState(UnsignedInt nowMs)
{
	if (!m_testStateEnabled || nowMs < m_testStateNextMs)
		return;
	m_testStateNextMs = nowMs + STATE_INTERVAL_MS;
	const std::string name = instanceFileName("state");
	const std::string temp = name + ".tmp";
	FILE *f = fopen(temp.c_str(), "w");
	if (!f)
		return;
	writeTestState(f, "STATE", nowMs);
	fclose(f);
	MoveFileExA(temp.c_str(), name.c_str(), MOVEFILE_REPLACE_EXISTING);
}

/// SNAP:label: append the state, labelled, to the snapshot file.
void GameController::writeTestSnapshot(const char *label, UnsignedInt nowMs) const
{
	FILE *f = fopen(instanceFileName("snaps").c_str(), "a");
	if (!f)
		return;
	char heading[80];
	snprintf(heading, sizeof(heading), "SNAP %s", label);
	writeTestState(f, heading, nowMs);
	fclose(f);
}

/// Start of a run: the snapshot file starts empty, and the state file is written if asked for.
void GameController::beginTestState()
{
	m_testStateEnabled = getenv("CONTROLLERMOD_TEST_STATE") != nullptr;
	m_testStateNextMs = 0;
	if (!m_testSteps.empty())
		remove(instanceFileName("snaps").c_str());
}

//-------------------------------------------------------------------------------------------------
/** AIM:Name|Name: the stick (0 left, 1 right) and direction (degrees, 0 = up, clockwise) that
	point at the open wheel's slice whose name or internal command name contains one of the
	Names. False when no wheel is open or no slice matches. */
//-------------------------------------------------------------------------------------------------
Bool GameController::testAimAt(const char *names, Int *side, Real *degrees) const
{
	if (!m_panelOpen || !m_settings.wheelMenus || m_panelKind < 0)
		return FALSE;
	Int wheels = 1;
	std::vector<PanelEntry> first;
	collectWheelEntries(0, &first, &wheels);
	for (Int s = 0; s < wheels; ++s)
	{
		std::vector<PanelEntry> entries;
		if (s == 0)
			entries = first;
		else
			collectWheelEntries(1, &entries, nullptr);
		const Int n = (Int)entries.size();
		for (Int i = 0; i < n; ++i)
		{
			const CommandButton *command = entries[i].window ? (const CommandButton *)GadgetButtonGetData(entries[i].window) : nullptr;
			if (matchesAny(names, ascii(panelEntryName(entries[i]))) ||
				(command && matchesAny(names, std::string(command->getName().str()))))
			{
				*side = s;
				*degrees = i * 360.0f / n;
				return TRUE;
			}
		}
	}
	return FALSE;
}

#endif // CONTROLLERMOD_ENABLE_TEST_INPUT
