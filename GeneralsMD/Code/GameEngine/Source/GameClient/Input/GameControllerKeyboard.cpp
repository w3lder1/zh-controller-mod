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

// GameControllerKeyboard.cpp /////////////////////////////////////////////////////////////////////
// ControllerMod @feature On-screen keyboard for the menus' text boxes: the player name, the Direct
// Connect address, the lobby chat, and in a multiplayer match the chat box.
//
// A on a text box (Y on a drop-down that can be typed into) opens it. The stick or D-pad moves
// over the keys, A types the key, X deletes, Y types a space, LB switches capitals, Start is done
// (like Enter: the chat sends), B puts the old text back. Address boxes get a number pad.
//
// The keys are typed into the box the way the keyboard types: character messages to the text
// box, which applies its own rules (length, numbers only, letters and numbers only) and tells its
// menu, so the menu reacts exactly as to a keyboard (the LAN lobby renames the player at once).
//
// The match's chat box: with the help open, A asks the game for it the way the Enter key does
// (everyone), X the way the allies chat key does, and the keyboard types into it as soon as it is
// open. Start sends the line (the chat's own Enter), B closes the box. A chat box opened with the
// keyboard's Enter key gets the on-screen keyboard with A.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameController.h"
#include "GameClient/ControllerMath.h"

#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/KeyDefs.h"

namespace
{
	// Special keys in the layouts.
	const char OSK_CAPS = '\x01';
	const char OSK_SPACE = '\x02';
	const char OSK_DELETE = '\x03';
	const char OSK_DONE = '\x04';

	const char *const LETTER_ROWS[] = { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?", "-_!@#()/:+", "\x01\x02\x03\x04" };
	const char *const NUMBER_ROWS[] = { "123", "456", "789", ".0\x03", "\x04" };
	const Int LETTER_ROW_COUNT = sizeof(LETTER_ROWS) / sizeof(LETTER_ROWS[0]);
	const Int NUMBER_ROW_COUNT = sizeof(NUMBER_ROWS) / sizeof(NUMBER_ROWS[0]);

	const UnsignedInt FEEDBACK_MS = 1500;

	UnicodeString toUnicodeText(const char *text)
	{
		UnicodeString u;
		u.translate(AsciiString(text));
		return u;
	}

	const char *const *layoutRows(Bool numeric, Int *count)
	{
		*count = numeric ? NUMBER_ROW_COUNT : LETTER_ROW_COUNT;
		return numeric ? NUMBER_ROWS : LETTER_ROWS;
	}

	UnicodeString keyLabel(char key, Bool caps)
	{
		switch (key)
		{
			case OSK_CAPS: return toUnicodeText(caps ? "abc" : "ABC");
			case OSK_SPACE: return toUnicodeText("Space");
			case OSK_DELETE: return toUnicodeText("Delete");
			case OSK_DONE: return toUnicodeText("Done");
		}
		char text[2] = { caps ? (char)toupper((unsigned char)key) : key, 0 };
		return toUnicodeText(text);
	}

	/// The menu direction (up, right, down, left) as ControllerMath::keyboardMove's (up, down, left, right).
	Int moveDirection(Int menuDir)
	{
		static const Int map[4] = { 0, 3, 1, 2 };
		return (menuDir >= 0 && menuDir < 4) ? map[menuDir] : -1;
	}
}

/// The text box inside a drop-down that can be typed into, or nullptr.
GameWindow *GameController::editableComboEntry(GameWindow *combo)
{
	if (!combo || !BitIsSet(combo->winGetStyle(), GWS_COMBO_BOX))
		return nullptr;
	ComboBoxData *data = (ComboBoxData *)combo->winGetUserData();
	if (!data || !data->isEditable || !data->editBox)
		return nullptr;
	return data->editBox;
}

void GameController::openKeyboard(GameWindow *entry, GameWindow *stop)
{
	if (!entry || !stop)
		return;
	m_keyboardOpen = TRUE;
	m_keyboardEntry = entry;
	m_keyboardStop = stop;
	m_keyboardOriginal = GadgetTextEntryGetText(entry);
	m_keyboardShown = m_keyboardOriginal;
	m_keyboardCaps = FALSE;
	m_keyboardFeedback.clear();
	m_keyboardFeedbackUntilMs = 0;
	m_keyboardMatchChat = FALSE;

	// A number pad for number-only boxes and addresses (the Direct Connect "Remote IP").
	const EntryData *data = (const EntryData *)entry->winGetUserData();
	AsciiString name = TheNameKeyGenerator ? TheNameKeyGenerator->keyToName((NameKeyType)stop->winGetWindowId()) : AsciiString::TheEmptyString;
	m_keyboardNumeric = (data && data->numericalOnly) || strstr(name.str(), "IP") != nullptr;
	m_keyboardRow = m_keyboardNumeric ? 0 : 1;
	m_keyboardCol = 0;
}

/// keep: the typed text stays (Done); otherwise the box gets its old text back (B).
void GameController::closeKeyboard(Bool keep)
{
	if (!m_keyboardOpen)
		return;
	if (keep)
	{
		// Done is Enter: the chat sends, a name or address is taken.
		TheWindowManager->winSendInputMsg(m_keyboardEntry, GWM_IME_CHAR, (WindowMsgData)VK_RETURN, 0);
		// The match's chat box closed on that Enter. The game then expects the same Enter to reach
		// its chat key as well and ignores that one press; the keyboard's Enter would, the pad's
		// does not, so the chat key hears it here (else the next real Enter would be ignored).
		if (m_keyboardMatchChat && !isMatchChatOpen())
			ToggleInGameChat();
	}
	else
	{
		// Put the old text back the way it came in, so the menu hears every change.
		for (Int guard = 0; guard < 256 && GadgetTextEntryGetText(m_keyboardEntry).getLength() > 0; ++guard)
			keyboardBackspace();
		for (Int i = 0; i < m_keyboardOriginal.getLength(); ++i)
			keyboardType(m_keyboardOriginal.getCharAt(i));
		// B in the match's chat also closes the box (it keeps the text for next time, like Esc).
		if (m_keyboardMatchChat && isMatchChatOpen())
			HideInGameChat();
	}
	m_keyboardMatchChat = FALSE;
	// The focus frame comes back on the box (found among the live gadgets next frame).
	m_menuFocus = m_keyboardStop;
	m_menuFocusShown = TRUE;
	m_keyboardOpen = FALSE;
	m_keyboardEntry = nullptr;
	m_keyboardStop = nullptr;
	m_menuFocusDrawable = FALSE;
}

void GameController::keyboardType(WideChar ch)
{
	TheWindowManager->winSendInputMsg(m_keyboardEntry, GWM_IME_CHAR, (WindowMsgData)ch, 0);
}

void GameController::keyboardBackspace()
{
	TheWindowManager->winSendInputMsg(m_keyboardEntry, GWM_CHAR, (WindowMsgData)KEY_BACKSPACE, (WindowMsgData)KEY_STATE_DOWN);
	TheWindowManager->winSendInputMsg(m_keyboardEntry, GWM_CHAR, (WindowMsgData)KEY_BACKSPACE, (WindowMsgData)KEY_STATE_UP);
}

void GameController::updateKeyboard(const std::vector<MenuTarget> &targets, UnsignedInt pressed, Int dir, UnsignedInt nowMs)
{
	// The menu with the box closed (a message box, a new screen): the keyboard goes with it.
	Bool alive = FALSE;
	for (size_t i = 0; i < targets.size(); ++i)
	{
		if (targets[i].window == m_keyboardStop)
			alive = TRUE;
	}
	if (!alive)
	{
		m_keyboardOpen = FALSE;
		m_keyboardMatchChat = FALSE;
		m_keyboardEntry = nullptr;
		m_keyboardStop = nullptr;
		return;
	}

	Int rowCount = 0;
	const char *const *rows = layoutRows(m_keyboardNumeric, &rowCount);
	Int lengths[8];
	for (Int r = 0; r < rowCount; ++r)
		lengths[r] = (Int)strlen(rows[r]);

	const Int move = moveDirection(dir);
	if (move >= 0)
		ControllerMath::keyboardMove(lengths, rowCount, &m_keyboardRow, &m_keyboardCol, move);

	const UnicodeString before = GadgetTextEntryGetText(m_keyboardEntry);
	Bool typedChar = FALSE;

	if (pressed & ControllerState::BUTTON_B)
	{
		closeKeyboard(FALSE);
		return;
	}
	if (pressed & ControllerState::BUTTON_MENU)
	{
		closeKeyboard(TRUE);
		return;
	}
	if (pressed & ControllerState::BUTTON_X)
		keyboardBackspace();
	if (pressed & ControllerState::BUTTON_Y)
	{
		keyboardType(L' ');
		typedChar = TRUE;
	}
	if (pressed & ControllerState::BUTTON_LB)
		m_keyboardCaps = !m_keyboardCaps;
	if (pressed & ControllerState::BUTTON_A)
	{
		const char key = rows[m_keyboardRow][m_keyboardCol];
		switch (key)
		{
			case OSK_CAPS: m_keyboardCaps = !m_keyboardCaps; break;
			case OSK_SPACE: keyboardType(L' '); typedChar = TRUE; break;
			case OSK_DELETE: keyboardBackspace(); break;
			case OSK_DONE: closeKeyboard(TRUE); return;
			default:
				keyboardType((WideChar)(m_keyboardCaps ? toupper((unsigned char)key) : key));
				typedChar = TRUE;
				break;
		}
	}

	// The box refused the character (full, or not allowed there): say so.
	m_keyboardShown = GadgetTextEntryGetText(m_keyboardEntry);
	if (typedChar && m_keyboardShown == before)
	{
		m_keyboardFeedback = toUnicodeText("That can't go in this box (full, or not allowed here)");
		m_keyboardFeedbackUntilMs = nowMs + FEEDBACK_MS;
	}
}

/// With the help open in a multiplayer match: ask the game for its chat box the way the chat keys
/// do (the game decides: no chat in replays, observers have no allies). The keyboard opens on it.
void GameController::openMatchChat(Bool allies, UnsignedInt nowMs)
{
	TheMessageStream->appendMessage(allies ? GameMessage::MSG_META_CHAT_ALLIES : GameMessage::MSG_META_CHAT_EVERYONE);
	m_matchChatPendingUntilMs = nowMs + 1000;
}

/// The match's chat box is open: the on-screen keyboard types into it.
void GameController::updateMatchChat(UnsignedInt pressed, Int dir, UnsignedInt nowMs)
{
	static const NameKeyType entryID = TheNameKeyGenerator->nameToKey("InGameChat.wnd:TextEntryChat");
	GameWindow *entry = TheWindowManager->winGetWindowFromId(nullptr, entryID);
	m_menuFocusDrawable = FALSE;   // no menu focus frame here: the chat box is the only stop
	if (!entry)
		return;

	if (m_keyboardOpen)
	{
		std::vector<MenuTarget> live(1);
		live[0].window = entry;
		live[0].style = entry->winGetStyle();
		updateKeyboard(live, pressed, dir, nowMs);
		return;
	}

	// Asked for by the pad (typing starts at once), or opened with the Enter key (A types).
	if (m_matchChatPendingUntilMs != 0 || (pressed & ControllerState::BUTTON_A))
	{
		m_matchChatPendingUntilMs = 0;
		openKeyboard(entry, entry);
		m_keyboardMatchChat = TRUE;
	}
	else if (pressed & ControllerState::BUTTON_B)
	{
		HideInGameChat();
	}
}

void GameController::drawKeyboard()
{
	if (!TheDisplay)
		return;
	Int rowCount = 0;
	const char *const *rows = layoutRows(m_keyboardNumeric, &rowCount);

	const Int lineHeight = textLineHeight();
	const Int keyH = lineHeight * 2;
	const Int gap = lineHeight / 3;
	const Int pad = lineHeight;
	const Int boxW = REAL_TO_INT(TheDisplay->getWidth() * (m_keyboardNumeric ? 0.26f : 0.56f));
	const Int boxH = pad * 2 + lineHeight * 3 + rowCount * (keyH + gap) + lineHeight * 2;
	const Int boxX = (TheDisplay->getWidth() - boxW) / 2;
	const Int boxY = TheDisplay->getHeight() - boxH - REAL_TO_INT(TheDisplay->getHeight() * 0.06f);

	TheDisplay->drawFillRect(boxX, boxY, boxW, boxH, GameMakeColor(0, 0, 0, 225));
	TheDisplay->drawOpenRect(boxX, boxY, boxW, boxH, 1.0f, GameMakeColor(255, 196, 40, 200));

	Int line = 0;
	DisplayString **lines = m_keyboardLines;
	for (Int i = 0; i < NUM_KEYBOARD_LINES; ++i)
	{
		if (!lines[i])
			lines[i] = makeString();
	}

	// The text so far, with a cursor.
	m_textRemapOff = TRUE;
	UnicodeString shown = m_keyboardShown;
	shown.concat(L'_');
	TheDisplay->drawFillRect(boxX + pad, boxY + pad, boxW - pad * 2, lineHeight + gap, GameMakeColor(30, 30, 30, 255));
	drawText(lines[line++], shown, boxX + pad + gap, boxY + pad + gap / 2, GameMakeColor(255, 255, 255, 255));
	Int y = boxY + pad + lineHeight * 2;

	// The keys.
	for (Int r = 0; r < rowCount && line < NUM_KEYBOARD_LINES - 3; ++r)
	{
		const Int n = (Int)strlen(rows[r]);
		const Int keyW = (boxW - pad * 2 - gap * (n - 1)) / n;
		for (Int c = 0; c < n && line < NUM_KEYBOARD_LINES - 3; ++c)
		{
			const Int x = boxX + pad + c * (keyW + gap);
			const Bool focused = r == m_keyboardRow && c == m_keyboardCol;
			TheDisplay->drawFillRect(x, y, keyW, keyH, focused ? GameMakeColor(150, 90, 10, 230) : GameMakeColor(28, 60, 100, 220));
			TheDisplay->drawOpenRect(x, y, keyW, keyH, focused ? 2.0f : 1.0f, focused ? GameMakeColor(255, 196, 40, 255) : GameMakeColor(120, 170, 220, 200));
			DisplayString *label = lines[line++];
			const UnicodeString text = keyLabel(rows[r][c], m_keyboardCaps);
			// Set the font and text first, to measure it for centring.
			ensureFont();
			if (label->getFont() != m_font)
				label->setFont(m_font);
			if (label->getText() != text)
				label->setText(text);
			Int w = 0, h = 0;
			label->getSize(&w, &h);
			drawText(label, text, x + (keyW - w) / 2, y + (keyH - h) / 2, GameMakeColor(255, 255, 255, 255));
		}
		y += keyH + gap;
	}
	m_textRemapOff = FALSE;

	// A refused key, then what the buttons do.
	y += gap;
	if (m_keyboardFeedbackUntilMs > timeGetTime() && !m_keyboardFeedback.isEmpty())
		drawText(lines[NUM_KEYBOARD_LINES - 2], m_keyboardFeedback, boxX + pad, y, GameMakeColor(255, 140, 120, 255));
	y += lineHeight;
	drawText(lines[NUM_KEYBOARD_LINES - 1],
		toUnicodeText(m_keyboardNumeric ? "A: type   X: delete   Start: done   B: cancel" : "A: type   X: delete   Y: space   LB: capitals   Start: done   B: cancel"),
		boxX + pad, y, GameMakeColor(255, 220, 120, 255));
}
