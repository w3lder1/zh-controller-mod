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

// GameControllerMenus.cpp ////////////////////////////////////////////////////////////////////////
// ControllerMod @feature The game's own menus with the controller.
//
// Everything that is not the battlefield (main menu, skirmish setup, options, save/load, the
// pause menu, message boxes, the score screen; in a multiplayer match the chat box and the
// "waiting for players" screen) is a native GUI made of windows and gadgets. The
// controller does not replace any of it. It keeps a focus on one of the gadgets the mouse could
// click right now and hands the player's choice to that gadget the way the mouse or keyboard
// would:
//
//   D-pad / left stick   move the focus to the nearest gadget in that direction; on a slider,
//                        left/right move the slider and only up/down leave it
//   up / down            in a list: previous / next entry first (the list's own arrow keys)
//   A                    click the focused button, check box or radio button (a real mouse
//                        click at a point where the mouse reaches it); open a drop-down list
//                        (a click, like the mouse), then up/down choose in it, A keeps the
//                        choice and B puts the old one back; take the selected list entry (the
//                        list's own Enter key; on a save list with NEW GAME chosen, Save); on
//                        the skirmish medals, look at them one by one (the cursor goes over
//                        each, so the game shows its tooltip) until B
//   LB / RB              the menu's tabs (radio buttons: Official / Unofficial Maps)
//   Start                outside a match: jump to Play Game (or OK)
//   B                    Back. Message boxes: No, else Cancel, else OK, and the focus starts
//                        there, so "Exit?" is never answered yes by one press too many. Other
//                        screens: their own Cancel or Back button (what their Esc key presses;
//                        the skirmish screen never gets the Esc key), else the Esc key.
//
// Which gadgets can have the focus is worked out the way the mouse finds its window: the window
// holding the mouse capture, else the modal window, else the top-most visible layer that has
// something to click; and a gadget only counts if a click at one of its points really lands on
// it. The real mouse cursor is moved onto the focused gadget, so the menus show their normal
// hover highlight and tooltips. Moving the mouse hides the controller focus until the pad is
// used again.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameController.h"
#include "GameClient/ControllerMath.h"

#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/DisconnectMenu.h"
#include "GameClient/GameClient.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/Shell.h"
#include "GameClient/DisplayString.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/Mouse.h"
#include "GameLogic/GameLogic.h"

extern HWND ApplicationHWnd;

#ifndef CONTROLLERMOD_ENABLE_TEST_INPUT
#define CONTROLLERMOD_ENABLE_TEST_INPUT 0
#endif

#if CONTROLLERMOD_ENABLE_TEST_INPUT
// Test builds only: with CONTROLLERMOD_MENU_DUMP set, every gadget of the menu layer is written
// to controllermod_menu_dump.txt with the reason it is or is not a focus stop, whenever the set
// of stops changes.
static void dumpMenuGadgets(FILE *f, GameWindow *win, Int depth)
{
	if (!win)
		return;
	const UnsignedInt style = win->winGetStyle();
	const UnsignedInt status = win->winGetStatus();
	Int x = 0, y = 0, w = 0, h = 0;
	win->winGetScreenPosition(&x, &y);
	win->winGetSize(&w, &h);
	const AsciiString name = TheNameKeyGenerator ? TheNameKeyGenerator->keyToName((NameKeyType)win->winGetWindowId()) : AsciiString();
	fprintf(f, "%*s%s style=%x status=%x rect=%d,%d %dx%d%s%s\n", depth * 2, "", name.str(), style, status, x, y, w, h,
		BitIsSet(status, WIN_STATUS_HIDDEN) ? " HIDDEN" : "", TheWindowManager->isEnabled(win) ? "" : " DISABLED");
	for (GameWindow *child = win->winGetChild(); child; child = child->winGetNext())
		dumpMenuGadgets(f, child, depth + 1);
}
#endif

namespace
{
	enum { MENU_UP = 0, MENU_RIGHT, MENU_DOWN, MENU_LEFT };

	const Real MENU_STICK_THRESHOLD = 0.6f;
	const Int MENU_ROW_TOLERANCE = 12;       ///< pixels: gadgets this close in height are one row
	const Int MENU_SLIDER_STEPS = 20;        ///< a full slider is this many presses
	const UnsignedInt MENU_SETTLE_MS = 1000; ///< buttons rest this long after skipping a movie or revealing the main menu
	const Int MENU_MEMORY_SIZE = 16;         ///< menus whose last focus is remembered

	/// The gadgets the focus stops on. A gadget's own parts (a list's scroll bar, a drop-down
	/// list's button and list) are never separate stops.
	const UnsignedInt MENU_GADGETS = GWS_PUSH_BUTTON | GWS_RADIO_BUTTON | GWS_CHECK_BOX |
		GWS_HORZ_SLIDER | GWS_SCROLL_LISTBOX | GWS_ENTRY_FIELD | GWS_COMBO_BOX;

	Bool isInside(GameWindow *ancestor, GameWindow *win)
	{
		for (; win; win = win->winGetParent())
		{
			if (win == ancestor)
				return TRUE;
		}
		return FALSE;
	}

	void screenRect(GameWindow *win, IRegion2D *rect)
	{
		Int x = 0, y = 0, w = 0, h = 0;
		win->winGetScreenPosition(&x, &y);
		win->winGetSize(&w, &h);
		rect->lo.x = x;
		rect->lo.y = y;
		rect->hi.x = x + w;
		rect->hi.y = y + h;
	}

	UnicodeString toUnicodeText(const char *text)
	{
		UnicodeString u;
		u.translate(AsciiString(text));
		return u;
	}
}

//-------------------------------------------------------------------------------------------------
// Which gadgets can be reached
//-------------------------------------------------------------------------------------------------

/// The window a mouse click at this point would go to, found the way
/// GameWindowManager::winProcessMouseEvent finds it.
GameWindow *GameController::menuWindowAt(const ICoord2D &point) const
{
	GameWindow *hit = nullptr;
	GameWindow *captor = TheWindowManager->winGetCapture();
	GameWindow *modal = TheWindowManager->winGetModalWindow();
	if (captor)
	{
		hit = captor->winPointInChild(point.x, point.y);
	}
	else if (modal)
	{
		hit = modal->winPointInChild(point.x, point.y);
	}
	else
	{
		ICoord2D pos = point;
		// A non-null tooltip window makes the search skip its tooltip lookup.
		GameWindow *tooltip = TheWindowManager->winGetWindowList();
		hit = TheWindowManager->findWindowUnderMouse(tooltip, &pos, WIN_STATUS_ABOVE, WIN_STATUS_HIDDEN);
		if (!hit)
			hit = TheWindowManager->findWindowUnderMouse(tooltip, &pos, WIN_STATUS_NONE, WIN_STATUS_ABOVE | WIN_STATUS_BELOW | WIN_STATUS_HIDDEN);
		if (!hit)
			hit = TheWindowManager->findWindowUnderMouse(tooltip, &pos, WIN_STATUS_BELOW, WIN_STATUS_HIDDEN);
	}

	if (hit && BitIsSet(hit->winGetStatus(), WIN_STATUS_NO_INPUT))
	{
		GameWindow *parent = hit->winGetParent();
		hit = (parent && BitIsSet(parent->winGetStyle(), GWS_COMBO_BOX)) ? parent : nullptr;
	}
	return hit;
}

/// A point on the screen where a mouse click reaches this gadget (it is not covered by another
/// window, and it is not outside a modal window).
Bool GameController::findMenuClickPoint(GameWindow *win, ICoord2D *point) const
{
	IRegion2D rect;
	screenRect(win, &rect);
	const Int w = rect.hi.x - rect.lo.x;
	const Int h = rect.hi.y - rect.lo.y;
	if (w < 2 || h < 2 || !TheDisplay)
		return FALSE;

	static const Real fx[5] = { 0.5f, 0.25f, 0.75f, 0.5f, 0.5f };
	static const Real fy[5] = { 0.5f, 0.5f, 0.5f, 0.25f, 0.75f };
	for (Int i = 0; i < 5; ++i)
	{
		ICoord2D p;
		p.x = rect.lo.x + REAL_TO_INT(w * fx[i]);
		p.y = rect.lo.y + REAL_TO_INT(h * fy[i]);
		if (p.x < 0 || p.y < 0 || p.x >= (Int)TheDisplay->getWidth() || p.y >= (Int)TheDisplay->getHeight())
			continue;
		if (isInside(win, menuWindowAt(p)))
		{
			*point = p;
			return TRUE;
		}
	}
	return FALSE;
}

void GameController::addMenuTargets(GameWindow *win, std::vector<MenuTarget> *out) const
{
	if (BitIsSet(win->winGetStatus(), WIN_STATUS_HIDDEN))
		return;

	const UnsignedInt style = win->winGetStyle();
	if (style & MENU_GADGETS)
	{
		MenuTarget target;
		if (TheWindowManager->isEnabled(win) && findMenuClickPoint(win, &target.clickPoint))
		{
			target.window = win;
			target.style = style;
			screenRect(win, &target.rect);
			out->push_back(target);
		}
		return;
	}

	for (GameWindow *child = win->winGetChild(); child; child = child->winGetNext())
		addMenuTargets(child, out);
}

/// The match's chat box is open (the Enter key's, or the pad's from the help).
Bool GameController::isMatchChatOpen()
{
	return IsInGameChatActive();
}

/// The "waiting for players" screen of a multiplayer match (a player stopped responding).
Bool GameController::isDisconnectScreenOpen()
{
	return TheDisconnectMenu && TheDisconnectMenu->isScreenVisible();
}

/// The top window of the match's chat box or "waiting for players" screen, while one is open.
/// The rest of the match screen (the control bar) is never part of that menu.
GameWindow *GameController::matchMenuRoot()
{
	if (!TheWindowManager || !TheNameKeyGenerator)
		return nullptr;
	const char *gadget = nullptr;
	if (isDisconnectScreenOpen())
		gadget = "DisconnectScreen.wnd:ButtonQuitGame";
	else if (isMatchChatOpen())
		gadget = "InGameChat.wnd:TextEntryChat";
	if (!gadget)
		return nullptr;
	GameWindow *win = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(gadget));
	while (win && win->winGetParent())
		win = win->winGetParent();
	return win;
}

/// The gadgets the focus can be on now, and the window they belong to (the capture or modal
/// window, the match's chat box or "waiting for players" screen, or the top-most visible layer
/// that has any).
void GameController::collectMenuTargets(std::vector<MenuTarget> *out, GameWindow **layer) const
{
	out->clear();
	*layer = nullptr;
	if (!TheWindowManager)
		return;

	GameWindow *root = TheWindowManager->winGetCapture();
	if (!root)
		root = TheWindowManager->winGetModalWindow();
	if (!root && TheGameLogic && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame())
		root = matchMenuRoot();
	if (root)
	{
		if (!TheWindowManager->isHidden(root))
			addMenuTargets(root, out);
		*layer = root;
		dumpMenuTargets(*out, root);
		return;
	}

	// Top-level windows in the order the mouse looks at them: always-on-top ones first, then
	// the normal ones, then the ones kept at the back, each in window-list (front to back) order.
	static const UnsignedInt required[3] = { WIN_STATUS_ABOVE, WIN_STATUS_NONE, WIN_STATUS_BELOW };
	static const UnsignedInt forbidden[3] = { WIN_STATUS_HIDDEN, WIN_STATUS_ABOVE | WIN_STATUS_BELOW | WIN_STATUS_HIDDEN, WIN_STATUS_HIDDEN };
	for (Int pass = 0; pass < 3; ++pass)
	{
		for (GameWindow *win = TheWindowManager->winGetWindowList(); win; win = win->winGetNext())
		{
			const UnsignedInt status = win->winGetStatus();
			if (required[pass] != WIN_STATUS_NONE && !BitIsSet(status, required[pass]))
				continue;
			if (BitIsSet(status, forbidden[pass]) || !BitIsSet(status, WIN_STATUS_ENABLED))
				continue;
			// The LAN lobby's game info panel (players and map of the selected game) only shows
			// information; the lobby under it stays the menu.
			if (TheNameKeyGenerator && strncmp(TheNameKeyGenerator->keyToName((NameKeyType)win->winGetWindowId()).str(), "GameInfoWindow.wnd", 18) == 0)
				continue;
			addMenuTargets(win, out);
			if (!out->empty())
			{
				*layer = win;
				dumpMenuTargets(*out, win);
				return;
			}
		}
	}
}

/// Test builds only: see dumpMenuGadgets.
void GameController::dumpMenuTargets(const std::vector<MenuTarget> &targets, GameWindow *layer) const
{
#if CONTROLLERMOD_ENABLE_TEST_INPUT
	static size_t s_dumpedCount = 0;
	static GameWindow *s_dumpedLayer = nullptr;
	if (getenv("CONTROLLERMOD_MENU_DUMP") && (targets.size() != s_dumpedCount || layer != s_dumpedLayer))
	{
		s_dumpedCount = targets.size();
		s_dumpedLayer = layer;
		FILE *f = fopen("controllermod_menu_dump.txt", "a");
		if (f)
		{
			fprintf(f, "=== %u stops\n", (unsigned)targets.size());
			for (size_t i = 0; i < targets.size(); ++i)
			{
				const MenuTarget &t = targets[i];
				fprintf(f, "STOP %s rect=%d,%d-%d,%d click=%d,%d\n",
					TheNameKeyGenerator->keyToName((NameKeyType)t.window->winGetWindowId()).str(),
					t.rect.lo.x, t.rect.lo.y, t.rect.hi.x, t.rect.hi.y, t.clickPoint.x, t.clickPoint.y);
			}
			dumpMenuGadgets(f, layer, 0);
			fclose(f);
		}
	}
#else
	(void)targets;
	(void)layer;
#endif
}

//-------------------------------------------------------------------------------------------------
// Moving the focus
//-------------------------------------------------------------------------------------------------

static Bool isPictureList(GameWindow *list);

/// A chat log (the LAN lobby's, the game setup's, the "waiting for players" screen's): only read,
/// never the menu's main list.
static Bool isChatLog(GameWindow *list)
{
	if (!list || !TheNameKeyGenerator)
		return FALSE;
	const AsciiString name = TheNameKeyGenerator->keyToName((NameKeyType)list->winGetWindowId());
	return strstr(name.str(), "Chat") != nullptr || strstr(name.str(), "DisconnectScreen.wnd:ListboxTextDisplay") != nullptr;
}

/// Where a menu starts: its list (the maps, the save games, the LAN games), else the first gadget
/// in reading order: the top row, then the left-most in it. Text boxes are passed over when there
/// is anything else (the skirmish screen starts with the player name), and so are the medal list
/// and chat logs (the LAN lobby's chat comes before its games list).
Int GameController::firstMenuTarget(const std::vector<MenuTarget> &targets)
{
	for (Int i = 0; i < (Int)targets.size(); ++i)
	{
		if (BitIsSet(targets[i].style, GWS_SCROLL_LISTBOX) && !isPictureList(targets[i].window) && !isChatLog(targets[i].window))
			return i;
	}
	Int best = -1;
	for (Int i = 0; i < (Int)targets.size(); ++i)
	{
		if (BitIsSet(targets[i].style, GWS_ENTRY_FIELD))
			continue;
		if (best < 0)
		{
			best = i;
			continue;
		}
		const IRegion2D &a = targets[i].rect;
		const IRegion2D &b = targets[best].rect;
		if (a.lo.y < b.lo.y - MENU_ROW_TOLERANCE)
			best = i;
		else if (abs(a.lo.y - b.lo.y) <= MENU_ROW_TOLERANCE && a.lo.x < b.lo.x)
			best = i;
	}
	return best < 0 ? 0 : best;
}

/// In a message box (the game's two layouts for them), its No button, else Cancel, else OK
/// (-1 when this is not a message box).
Int GameController::menuCancelTarget(const std::vector<MenuTarget> &targets) const
{
	static const NameKeyType ids[6] =
	{
		TheNameKeyGenerator->nameToKey("MessageBox.wnd:ButtonNo"),
		TheNameKeyGenerator->nameToKey("QuitMessageBox.wnd:ButtonNo"),
		TheNameKeyGenerator->nameToKey("MessageBox.wnd:ButtonCancel"),
		TheNameKeyGenerator->nameToKey("QuitMessageBox.wnd:ButtonCancel"),
		TheNameKeyGenerator->nameToKey("MessageBox.wnd:ButtonOk"),
		TheNameKeyGenerator->nameToKey("QuitMessageBox.wnd:ButtonOk")
	};
	for (Int k = 0; k < 6; ++k)
	{
		for (Int i = 0; i < (Int)targets.size(); ++i)
		{
			if ((NameKeyType)targets[i].window->winGetWindowId() == ids[k])
				return i;
		}
	}
	return -1;
}

/// The nearest gadget in a direction (ControllerMath::findNeighbour, which is unit tested).
Int GameController::findMenuNeighbour(const std::vector<MenuTarget> &targets, Int from, Int dir) const
{
	std::vector<Int> rects(targets.size() * 4);
	for (size_t i = 0; i < targets.size(); ++i)
	{
		rects[i * 4 + 0] = targets[i].rect.lo.x;
		rects[i * 4 + 1] = targets[i].rect.lo.y;
		rects[i * 4 + 2] = targets[i].rect.hi.x;
		rects[i * 4 + 3] = targets[i].rect.hi.y;
	}
	return ControllerMath::findNeighbour((const int (*)[4])&rects[0], (Int)targets.size(), from, dir);
}

/// D-pad or left stick direction this frame, with the panels' hold-to-repeat timing.
Int GameController::menuDirection(UnsignedInt held, const ControllerState &state, UnsignedInt nowMs)
{
	Int dir = -1;
	if (held & ControllerState::BUTTON_DPAD_UP)
		dir = MENU_UP;
	else if (held & ControllerState::BUTTON_DPAD_DOWN)
		dir = MENU_DOWN;
	else if (held & ControllerState::BUTTON_DPAD_LEFT)
		dir = MENU_LEFT;
	else if (held & ControllerState::BUTTON_DPAD_RIGHT)
		dir = MENU_RIGHT;
	else if (!m_needStickNeutral[0])
	{
		const Real x = state.leftX;
		const Real y = state.leftY;
		if (fabs(x) >= MENU_STICK_THRESHOLD || fabs(y) >= MENU_STICK_THRESHOLD)
		{
			if (fabs(y) >= fabs(x))
				dir = (y > 0.0f) ? MENU_UP : MENU_DOWN;
			else
				dir = (x > 0.0f) ? MENU_RIGHT : MENU_LEFT;
		}
	}

	if (dir < 0)
	{
		m_menuRepeatDir = -1;
		return -1;
	}
	if (dir != m_menuRepeatDir)
	{
		m_menuRepeatDir = dir;
		m_menuRepeatNextMs = nowMs + (UnsignedInt)m_settings.repeatDelayMs;
		return dir;
	}
	if (nowMs >= m_menuRepeatNextMs)
	{
		m_menuRepeatNextMs = nowMs + (UnsignedInt)m_settings.repeatRateMs;
		return dir;
	}
	return -1;
}

//-------------------------------------------------------------------------------------------------
// Acting on the focused gadget
//-------------------------------------------------------------------------------------------------

/// Put the real mouse cursor on a point of the game window, so the menus show their own hover
/// highlight and tooltip there.
void GameController::placeMenuCursor(const ICoord2D &pos)
{
	if (TheMouse)
		TheMouse->setPosition(pos.x, pos.y);
	POINT target;
	target.x = pos.x;
	target.y = pos.y;
	if (ClientToScreen(ApplicationHWnd, &target))
	{
		SetCursorPos(target.x, target.y);
		m_menuCursorScreen.x = target.x;
		m_menuCursorScreen.y = target.y;
		m_menuCursorKnown = TRUE;
	}
}

void GameController::setMenuFocus(const MenuTarget &target)
{
	m_menuFocus = target.window;
	placeMenuCursor(target.clickPoint);
}

static Bool isPictureList(GameWindow *list);

/// Copies what drawing needs from the focused gadget, which was found alive this frame. Drawing
/// never looks at the gadget itself: an action can close its menu before the frame is drawn.
void GameController::noteMenuFocus(const MenuTarget &target)
{
	m_menuFocusRect = target.rect;
	m_menuFocusStyle = target.style;
	m_menuFocusPicture = BitIsSet(target.style, GWS_SCROLL_LISTBOX) && isPictureList(target.window);
	m_menuFocusDrawable = TRUE;
}

/// A left click at the gadget, sent as the raw mouse messages the mouse itself sends, so the
/// window system handles it exactly like a real click.
void GameController::clickMenuTarget(const MenuTarget &target)
{
	placeMenuCursor(target.clickPoint);
	const UnsignedInt now = timeGetTime();
	GameMessage *down = TheMessageStream->appendMessage(GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN);
	down->appendPixelArgument(target.clickPoint);
	down->appendIntegerArgument(0);
	down->appendIntegerArgument(now);
	GameMessage *up = TheMessageStream->appendMessage(GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP);
	up->appendPixelArgument(target.clickPoint);
	up->appendIntegerArgument(0);
	up->appendIntegerArgument(now);
}

/// A key press and release as the keyboard sends them (the focused window, then its parents,
/// get it first; the menus treat Esc as Back).
void GameController::sendMenuKey(UnsignedByte key)
{
	GameMessage *down = TheMessageStream->appendMessage(GameMessage::MSG_RAW_KEY_DOWN);
	down->appendIntegerArgument(key);
	down->appendIntegerArgument(KEY_STATE_DOWN);
	GameMessage *up = TheMessageStream->appendMessage(GameMessage::MSG_RAW_KEY_UP);
	up->appendIntegerArgument(key);
	up->appendIntegerArgument(KEY_STATE_UP);
}

/// A key straight to one gadget's own keyboard handling (lists use it for up, down and Enter).
static void sendGadgetKey(GameWindow *win, UnsignedByte key)
{
	TheWindowManager->winSendInputMsg(win, GWM_CHAR, key, KEY_STATE_DOWN);
	TheWindowManager->winSendInputMsg(win, GWM_CHAR, key, KEY_STATE_UP);
}

/// Left/right on a slider: the slider's own arrow keys are unusable (they run backwards), so the
/// value is set the way dragging the thumb sets it: move the slider, then tell its owner.
static void changeSlider(GameWindow *win, Int step)
{
	Int minVal = 0, maxVal = 0;
	GadgetSliderGetMinMax(win, &minVal, &maxVal);
	if (maxVal <= minVal)
		return;
	Int stepSize = (maxVal - minVal) / MENU_SLIDER_STEPS;
	if (stepSize < 1)
		stepSize = 1;
	Int pos = GadgetSliderGetPosition(win) + step * stepSize;
	if (pos < minVal)
		pos = minVal;
	if (pos > maxVal)
		pos = maxVal;
	GadgetSliderSetPosition(win, pos);
	TheWindowManager->winSendSystemMsg(win->winGetOwner(), GSM_SLIDER_TRACK, (WindowMsgData)win, pos);
}

/// Up/down inside a list while it has somewhere to go. Returns FALSE at its ends, so the focus
/// can leave the list.
static Bool moveInList(GameWindow *list, Int dir)
{
	const Int count = GadgetListBoxGetNumEntries(list);
	if (count <= 0)
		return FALSE;
	Int selected = -1;
	GadgetListBoxGetSelected(list, &selected);
	if (dir == MENU_UP && selected <= 0)
		return FALSE;
	if (dir == MENU_DOWN && selected >= count - 1)
		return FALSE;
	sendGadgetKey(list, dir == MENU_UP ? KEY_UP : KEY_DOWN);
	return TRUE;
}

/// A list whose entries are pictures in several columns and no text (the skirmish medals): A looks
/// at them one by one instead of choosing a row. A list with text beside its pictures is an
/// ordinary list (the map list shows a star next to maps already beaten).
static Bool isPictureList(GameWindow *list)
{
	const ListboxData *data = (const ListboxData *)list->winGetUserData();
	if (!data || data->columns < 2 || !data->listData)
		return FALSE;
	Bool pictures = FALSE;
	for (Int row = 0; row < data->endPos; ++row)
	{
		const ListEntryCell *cells = data->listData[row].cell;
		for (Int col = 0; cells && col < data->columns; ++col)
		{
			if (cells[col].cellType == LISTBOX_IMAGE && cells[col].data)
				pictures = TRUE;
			if (cells[col].cellType == LISTBOX_TEXT && cells[col].data &&
				!((DisplayString *)cells[col].data)->getText().isEmpty())
				return FALSE;
		}
	}
	return pictures;
}

/// The screen rectangles of a picture list's filled cells, in reading order, found with the list's
/// own "which cell is at this point" lookup (the one its tooltip uses).
static void collectPictureCells(GameWindow *list, std::vector<IRegion2D> *out)
{
	out->clear();
	IRegion2D rect;
	screenRect(list, &rect);

	struct Cell { Int row, col; IRegion2D r; };
	std::vector<Cell> cells;
	const Int step = 4;
	for (Int y = rect.lo.y + 1; y < rect.hi.y; y += step)
	{
		for (Int x = rect.lo.x + 1; x < rect.hi.x; x += step)
		{
			Int row = -1, col = -1;
			GadgetListBoxGetEntryBasedOnXY(list, x, y, row, col);
			if (row < 0 || col < 0 || GadgetListBoxGetItemData(list, row, col) == nullptr)
				continue;
			size_t i = 0;
			while (i < cells.size() && (cells[i].row != row || cells[i].col != col))
				++i;
			if (i == cells.size())
			{
				Cell c;
				c.row = row;
				c.col = col;
				c.r.lo.x = c.r.hi.x = x;
				c.r.lo.y = c.r.hi.y = y;
				cells.push_back(c);
				continue;
			}
			IRegion2D &r = cells[i].r;
			if (x < r.lo.x) r.lo.x = x;
			if (x > r.hi.x) r.hi.x = x;
			if (y < r.lo.y) r.lo.y = y;
			if (y > r.hi.y) r.hi.y = y;
		}
	}

	// Reading order: row, then column.
	for (size_t i = 1; i < cells.size(); ++i)
	{
		for (size_t j = i; j > 0 && (cells[j].row < cells[j - 1].row ||
			(cells[j].row == cells[j - 1].row && cells[j].col < cells[j - 1].col)); --j)
		{
			const Cell t = cells[j];
			cells[j] = cells[j - 1];
			cells[j - 1] = t;
		}
	}
	for (size_t i = 0; i < cells.size(); ++i)
		out->push_back(cells[i].r);
}

static ICoord2D rectMiddle(const IRegion2D &r)
{
	ICoord2D p;
	p.x = (r.lo.x + r.hi.x) / 2;
	p.y = (r.lo.y + r.hi.y) / 2;
	return p;
}

//-------------------------------------------------------------------------------------------------
// Names
//-------------------------------------------------------------------------------------------------

static AsciiString menuWindowName(GameWindow *win)
{
	return TheNameKeyGenerator ? TheNameKeyGenerator->keyToName((NameKeyType)win->winGetWindowId()) : AsciiString();
}

/// The first stop whose window name ends with this text, or -1.
Int GameController::findMenuTargetNamed(const std::vector<MenuTarget> &targets, const char *suffix)
{
	const size_t m = strlen(suffix);
	for (Int i = 0; i < (Int)targets.size(); ++i)
	{
		const AsciiString name = menuWindowName(targets[i].window);
		const size_t n = strlen(name.str());
		if (n >= m && strcmp(name.str() + n - m, suffix) == 0)
			return i;
	}
	return -1;
}

//-------------------------------------------------------------------------------------------------
// Acting on the focused gadget
//-------------------------------------------------------------------------------------------------

void GameController::activateMenuTarget(const std::vector<MenuTarget> &targets, Int focus)
{
	const MenuTarget &target = targets[focus];
	if (BitIsSet(target.style, GWS_SCROLL_LISTBOX))
	{
		// The medals: look at them one by one.
		if (isPictureList(target.window))
		{
			collectPictureCells(target.window, &m_menuCells);
			if (!m_menuCells.empty())
			{
				m_menuMode = MENU_MODE_CELLS;
				m_menuModeWindow = target.window;
				m_menuCell = 0;
				placeMenuCursor(rectMiddle(m_menuCells[0]));
			}
			return;
		}

		// A save list with "NEW GAME" chosen: only Save applies (Load is not offered), so A saves.
		const Int save = findMenuTargetNamed(targets, ":ButtonSave");
		if (save >= 0 && findMenuTargetNamed(targets, ":ButtonLoad") < 0)
		{
			clickMenuTarget(targets[save]);
			placeMenuCursor(target.clickPoint);
			return;
		}

		// Nothing chosen yet: choose the first entry. Otherwise the list's Enter, which the menus
		// treat like a double click on the entry.
		Int selected = -1;
		GadgetListBoxGetSelected(target.window, &selected);
		sendGadgetKey(target.window, selected < 0 ? KEY_DOWN : KEY_ENTER);
		return;
	}
	if (BitIsSet(target.style, GWS_HORZ_SLIDER))
		return;
	// A text box (player name, chat): the on-screen keyboard types into it.
	if (BitIsSet(target.style, GWS_ENTRY_FIELD))
	{
		openKeyboard(target.window, target.window);
		return;
	}
	if (BitIsSet(target.style, GWS_COMBO_BOX))
	{
		// Open the list with a click, like the mouse. Remember the choice, so B can put it back.
		m_menuDropdownOriginal = -1;
		GadgetComboBoxGetSelectedPos(target.window, &m_menuDropdownOriginal);
		m_menuModeWindow = target.window;
	}
	clickMenuTarget(target);
}

/// Up/down in an open drop-down list: the list's own selection moves (and the menu hears it, like
/// the map list), the list stays open and scrolls to keep the choice in view.
static void moveInDropdown(GameWindow *combo, Int step)
{
	const Int count = GadgetComboBoxGetLength(combo);
	if (count <= 0)
		return;
	Int selected = -1;
	GadgetComboBoxGetSelectedPos(combo, &selected);
	Int next = (selected < 0) ? 0 : selected + step;
	if (next < 0)
		next = 0;
	if (next > count - 1)
		next = count - 1;
	if (next == selected)
		return;
	GadgetComboBoxSetSelectedPos(combo, next, TRUE);

	GameWindow *list = GadgetComboBoxGetListBox(combo);
	if (list)
	{
		const Int top = GadgetListBoxGetTopVisibleEntry(list);
		const Int bottom = GadgetListBoxGetBottomVisibleEntry(list);
		if (next < top)
			GadgetListBoxSetTopVisibleEntry(list, next);
		else if (next > bottom)
			GadgetListBoxSetTopVisibleEntry(list, top + (next - bottom));
	}
}

/// Close an open drop-down list, the way a click elsewhere closes it.
static void closeDropdown(GameWindow *combo)
{
	GadgetComboBoxHideList(combo);
	TheWindowManager->winSetLoneWindow(nullptr);
}

/// LB/RB: the next or previous tab of the menu (radio buttons, such as Official / Unofficial Maps).
void GameController::switchMenuTab(const std::vector<MenuTarget> &targets, Int step)
{
	std::vector<Int> tabs;
	for (Int i = 0; i < (Int)targets.size(); ++i)
	{
		if (BitIsSet(targets[i].style, GWS_RADIO_BUTTON))
			tabs.push_back(i);
	}
	if (tabs.size() < 2)
		return;
	// Left to right (then top to bottom).
	for (size_t i = 1; i < tabs.size(); ++i)
	{
		for (size_t j = i; j > 0; --j)
		{
			const IRegion2D &a = targets[tabs[j]].rect;
			const IRegion2D &b = targets[tabs[j - 1]].rect;
			if (a.lo.x < b.lo.x || (a.lo.x == b.lo.x && a.lo.y < b.lo.y))
			{
				const Int t = tabs[j];
				tabs[j] = tabs[j - 1];
				tabs[j - 1] = t;
			}
		}
	}
	Int current = -1;
	for (Int i = 0; i < (Int)tabs.size(); ++i)
	{
		if (BitIsSet(targets[tabs[i]].window->winGetInstanceData()->getState(), WIN_STATE_SELECTED))
			current = i;
	}
	Int next = (current < 0) ? (step > 0 ? 0 : (Int)tabs.size() - 1) : current + step;
	if (next < 0 || next >= (Int)tabs.size() || next == current)
		return;
	clickMenuTarget(targets[tabs[next]]);
}

//-------------------------------------------------------------------------------------------------
// Per frame
//-------------------------------------------------------------------------------------------------

/// While the start-up videos or another full-screen movie play, A, B and Start all do what Esc
/// does there (skip the stage or the movie), so they can simply be pressed repeatedly to get to
/// the menus. Only then: in a menu that is still sliding in, Esc would mean Back.
Bool GameController::skipMovieWithPad(UnsignedInt pressed)
{
	const Bool moviePlaying = (TheGameClient && TheGameClient->isIntroPlaying()) || (TheDisplay && TheDisplay->isMoviePlaying());
	if (!moviePlaying)
		return FALSE;
	if (pressed & (ControllerState::BUTTON_A | ControllerState::BUTTON_B | ControllerState::BUTTON_MENU))
	{
		sendMenuKey(KEY_ESC);
		m_menuSettleUntilMs = timeGetTime() + MENU_SETTLE_MS;
	}
	return TRUE;
}

/// The real mouse moved (or clicked) since the controller last placed it.
Bool GameController::menuMouseMoved()
{
	Bool moved = FALSE;
	POINT current;
	if (GetCursorPos(&current))
	{
		if (m_menuCursorKnown &&
			abs(current.x - m_menuCursorScreen.x) + abs(current.y - m_menuCursorScreen.y) >= m_settings.mouseTakeoverPixels)
			moved = TRUE;
		m_menuCursorScreen.x = current.x;
		m_menuCursorScreen.y = current.y;
		m_menuCursorKnown = TRUE;
	}
	if (TheMouse)
	{
		const UnsignedInt events = TheMouse->getButtonOrWheelEventCount();
		if (events != m_menuMouseEvents)
			moved = TRUE;
		m_menuMouseEvents = events;
	}
#if CONTROLLERMOD_ENABLE_TEST_INPUT
	// Automated testing only: in scripted runs the mouse is not the player's. With two game copies
	// on one PC, each copy moves the one real cursor onto its focused button, and the other copy
	// would take that for the player picking up the mouse.
	if (!m_testSteps.empty())
		moved = FALSE;
#endif
	return moved;
}

void GameController::rememberMenuFocus(GameWindow *layer, GameWindow *focus)
{
	for (size_t i = 0; i < m_menuMemory.size(); ++i)
	{
		if (m_menuMemory[i].first == layer)
		{
			m_menuMemory[i].second = focus;
			return;
		}
	}
	if ((Int)m_menuMemory.size() >= MENU_MEMORY_SIZE)
		m_menuMemory.erase(m_menuMemory.begin());
	m_menuMemory.push_back(std::make_pair(layer, focus));
}

/// Where the focus was when this menu was last used, if that gadget is still there.
Int GameController::rememberedMenuFocus(GameWindow *layer, const std::vector<MenuTarget> &targets) const
{
	for (size_t i = 0; i < m_menuMemory.size(); ++i)
	{
		if (m_menuMemory[i].first != layer)
			continue;
		for (size_t t = 0; t < targets.size(); ++t)
		{
			if (targets[t].window == m_menuMemory[i].second)
				return (Int)t;
		}
	}
	return -1;
}

void GameController::updateMenuNavigation(UnsignedInt pressed, UnsignedInt held, const ControllerState &state, UnsignedInt nowMs)
{
	if (m_needStickNeutral[0] && isStickNeutral(state, 0))
		m_needStickNeutral[0] = FALSE;

	const Bool mouseMoved = menuMouseMoved();
	const Int dir = menuDirection(held, state, nowMs);

	// After skipping the start-up videos (or revealing the main menu), A, B and Start rest until
	// they have not been pressed for a moment: presses still being mashed to skip the intro must
	// not choose Solo Play, a campaign or the Exit box. Each mashed press extends the rest.
	const UnsignedInt settleButtons = ControllerState::BUTTON_A | ControllerState::BUTTON_B | ControllerState::BUTTON_MENU;
	if (nowMs < m_menuSettleUntilMs)
	{
		if (pressed & settleButtons)
			m_menuSettleUntilMs = nowMs + MENU_SETTLE_MS;
		pressed &= ~settleButtons;
	}
	const UnsignedInt menuButtons = ControllerState::BUTTON_A | ControllerState::BUTTON_B |
		ControllerState::BUTTON_LB | ControllerState::BUTTON_RB | ControllerState::BUTTON_MENU;
	const Bool padUsed = dir >= 0 || (pressed & menuButtons) != 0;

	std::vector<MenuTarget> targets;
	GameWindow *layer = nullptr;
	collectMenuTargets(&targets, &layer);

#if CONTROLLERMOD_ENABLE_TEST_INPUT
	// Automated testing only: an @Name step uses that gadget like A (a button is clicked; a list's
	// first A picks its first entry, the next one takes it) as soon as it is on screen.
	if (!m_testPendingClick.isEmpty())
	{
		const Int named = findMenuTargetNamed(targets, m_testPendingClick.str());
		if (named >= 0)
		{
			activateMenuTarget(targets, named);
			m_testPendingClick.clear();
			return;
		}
	}
#endif

	// The on-screen keyboard owns the pad while it is open.
	if (m_keyboardOpen)
	{
		updateKeyboard(targets, pressed, dir, nowMs);
		return;
	}

	// A save/load step appeared with a Confirm button (the save name box, "overwrite?"): the focus
	// goes there, so NEW GAME, A, A saves. Only stops that just appeared are looked at.
	Int appeared = -1;
	for (Int i = 0; i < (Int)targets.size(); ++i)
	{
		Bool known = FALSE;
		for (size_t k = 0; k < m_menuPrevStops.size() && !known; ++k)
			known = m_menuPrevStops[k] == targets[i].window;
		if (!known && appeared < 0 && strstr(menuWindowName(targets[i].window).str(), "Confirm"))
			appeared = i;
	}
	Int radios = 0;
	for (size_t i = 0; i < targets.size(); ++i)
		radios += BitIsSet(targets[i].style, GWS_RADIO_BUTTON) ? 1 : 0;
	m_menuHasTabs = radios >= 2;
	m_menuPrevStops.clear();
	for (size_t i = 0; i < targets.size(); ++i)
		m_menuPrevStops.push_back(targets[i].window);

	// Nothing to focus. The main menu hides its buttons until any key or a mouse move; A or Start
	// is that key here (Space, which the hidden menu only uses to show itself). B is Esc.
	if (targets.empty())
	{
		m_menuFocus = nullptr;
		m_menuFocusStyle = 0;
		m_menuFocusDrawable = FALSE;
		m_menuMode = MENU_MODE_NORMAL;
		if ((pressed & (ControllerState::BUTTON_A | ControllerState::BUTTON_MENU)) && TheShell && TheShell->isShellActive())
		{
			sendMenuKey(KEY_SPACE);
			m_menuSettleUntilMs = nowMs + MENU_SETTLE_MS;
		}
		else if (pressed & ControllerState::BUTTON_B)
		{
			sendMenuKey(KEY_ESC);
		}
		return;
	}

	if (mouseMoved && !padUsed)
		m_menuFocusShown = FALSE;

	Int focus = -1;
	for (Int i = 0; i < (Int)targets.size(); ++i)
	{
		if (targets[i].window == m_menuFocus)
			focus = i;
	}

	if (!m_menuFocusShown)
	{
		m_menuMode = MENU_MODE_NORMAL;
		if (!padUsed)
		{
			m_menuFocus = nullptr;
			return;
		}
		// The pad takes over from the mouse: start on the gadget under the cursor, if any. This
		// press only shows the focus (B still goes back).
		m_menuFocusShown = TRUE;
		focus = -1;
		Bool useCursor = TheMouse != nullptr;
#if CONTROLLERMOD_ENABLE_TEST_INPUT
		// Automated testing only: scripted runs ignore the real cursor, which is wherever it was left
		// on the desktop (possibly over Exit in a background test window).
		if (!m_testSteps.empty())
			useCursor = FALSE;
#endif
		if (useCursor)
		{
			const ICoord2D mouse = TheMouse->getMouseStatus()->pos;
			for (Int i = 0; i < (Int)targets.size(); ++i)
			{
				const IRegion2D &r = targets[i].rect;
				if (mouse.x >= r.lo.x && mouse.x < r.hi.x && mouse.y >= r.lo.y && mouse.y < r.hi.y)
					focus = i;
			}
		}
		if (focus < 0)
			focus = rememberedMenuFocus(layer, targets);
		if (focus < 0)
			focus = menuCancelTarget(targets);
		if (focus < 0)
			focus = firstMenuTarget(targets);
		// Start jumps to Play Game (Accept) on this first press as well: it only moves the focus.
		if ((pressed & ControllerState::BUTTON_MENU) && menuStartTarget(targets) >= 0)
			focus = menuStartTarget(targets);
		setMenuFocus(targets[focus]);
		noteMenuFocus(targets[focus]);
		rememberMenuFocus(layer, targets[focus].window);
		if (pressed & ControllerState::BUTTON_B)
		{
			menuBack(targets);
			m_menuFocusDrawable = FALSE;
		}
		return;
	}

	if (appeared >= 0)
	{
		focus = appeared;
		setMenuFocus(targets[focus]);
	}
	else if (focus < 0)
	{
		// A new menu, or the focused gadget went away: where this menu was left, else a message
		// box's No, else the first gadget.
		focus = rememberedMenuFocus(layer, targets);
		if (focus < 0)
			focus = menuCancelTarget(targets);
		if (focus < 0)
			focus = firstMenuTarget(targets);
		setMenuFocus(targets[focus]);
	}

	noteMenuFocus(targets[focus]);
	rememberMenuFocus(layer, targets[focus].window);
	GameWindow *focusWindow = targets[focus].window;

	// --- An open drop-down list owns the pad until it closes ------------------------------------
	GameWindow *dropList = BitIsSet(targets[focus].style, GWS_COMBO_BOX) ? GadgetComboBoxGetListBox(focusWindow) : nullptr;
	if (dropList && !dropList->winIsHidden())
	{
		if (m_menuMode != MENU_MODE_DROPDOWN || m_menuModeWindow != focusWindow)
		{
			// Opened just now (by A, or by the mouse).
			if (m_menuModeWindow != focusWindow)
			{
				m_menuDropdownOriginal = -1;
				GadgetComboBoxGetSelectedPos(focusWindow, &m_menuDropdownOriginal);
			}
			m_menuMode = MENU_MODE_DROPDOWN;
			m_menuModeWindow = focusWindow;
		}
		if (dir == MENU_UP || dir == MENU_DOWN)
			moveInDropdown(focusWindow, dir == MENU_DOWN ? 1 : -1);
		if (pressed & ControllerState::BUTTON_A)
		{
			closeDropdown(focusWindow);
			m_menuMode = MENU_MODE_NORMAL;
		}
		else if (pressed & ControllerState::BUTTON_B)
		{
			Int selected = -1;
			GadgetComboBoxGetSelectedPos(focusWindow, &selected);
			if (m_menuDropdownOriginal >= 0 && selected != m_menuDropdownOriginal)
				GadgetComboBoxSetSelectedPos(focusWindow, m_menuDropdownOriginal, TRUE);
			closeDropdown(focusWindow);
			m_menuMode = MENU_MODE_NORMAL;
		}
		return;
	}
	if (m_menuMode == MENU_MODE_DROPDOWN)
		m_menuMode = MENU_MODE_NORMAL;   // closed some other way (the mouse)

	// --- Looking at the medals one by one --------------------------------------------------------
	if (m_menuMode == MENU_MODE_CELLS)
	{
		if (m_menuModeWindow != focusWindow || m_menuCells.empty())
		{
			m_menuMode = MENU_MODE_NORMAL;
		}
		else
		{
			if (m_menuCell < 0 || m_menuCell >= (Int)m_menuCells.size())
				m_menuCell = 0;
			// The medal's description shows at once, without the mouse's wait for the cursor to
			// rest (each step moves the cursor, which would restart that wait).
			if (TheMouse)
				TheMouse->skipTooltipDelay();
			if (dir >= 0)
			{
				std::vector<Int> rects(m_menuCells.size() * 4);
				for (size_t i = 0; i < m_menuCells.size(); ++i)
				{
					rects[i * 4 + 0] = m_menuCells[i].lo.x;
					rects[i * 4 + 1] = m_menuCells[i].lo.y;
					rects[i * 4 + 2] = m_menuCells[i].hi.x;
					rects[i * 4 + 3] = m_menuCells[i].hi.y;
				}
				const Int next = ControllerMath::findNeighbour((const int (*)[4])&rects[0], (Int)m_menuCells.size(), m_menuCell, dir);
				if (next >= 0)
				{
					m_menuCell = next;
					placeMenuCursor(rectMiddle(m_menuCells[m_menuCell]));
				}
			}
			if (pressed & (ControllerState::BUTTON_A | ControllerState::BUTTON_B))
			{
				m_menuMode = MENU_MODE_NORMAL;
				placeMenuCursor(targets[focus].clickPoint);
			}
			return;
		}
	}

	// --- Normal movement --------------------------------------------------------------------------
	if (dir >= 0)
	{
		const MenuTarget &current = targets[focus];
		Bool used = FALSE;
		if ((dir == MENU_LEFT || dir == MENU_RIGHT) && BitIsSet(current.style, GWS_HORZ_SLIDER))
		{
			changeSlider(current.window, dir == MENU_RIGHT ? 1 : -1);
			used = TRUE;
		}
		else if ((dir == MENU_UP || dir == MENU_DOWN) && BitIsSet(current.style, GWS_SCROLL_LISTBOX) && !isPictureList(current.window))
		{
			used = moveInList(current.window, dir);
		}

		if (!used)
		{
			const Int next = findMenuNeighbour(targets, focus, dir);
			if (next >= 0)
			{
				focus = next;
				setMenuFocus(targets[focus]);
				noteMenuFocus(targets[focus]);
				rememberMenuFocus(layer, targets[focus].window);
			}
		}
	}

	// LB/RB: the menu's tabs (Official / Unofficial Maps).
	if (pressed & (ControllerState::BUTTON_LB | ControllerState::BUTTON_RB))
	{
		switchMenuTab(targets, (pressed & ControllerState::BUTTON_RB) ? 1 : -1);
		placeMenuCursor(targets[focus].clickPoint);
		m_menuFocusDrawable = FALSE;
	}

	// Start (Menu) outside a match: jump to the button that starts it (Play Game), or the OK of
	// a screen that has no start button. It only moves the focus.
	if ((pressed & ControllerState::BUTTON_MENU) && TheGameLogic &&
		(!TheGameLogic->isInGame() || TheGameLogic->isInShellGame()))
	{
		const Int start = menuStartTarget(targets);
		if (start >= 0)
		{
			focus = start;
			setMenuFocus(targets[focus]);
			noteMenuFocus(targets[focus]);
			rememberMenuFocus(layer, targets[focus].window);
		}
	}

	// Y on a drop-down that can also be typed into (the Direct Connect address): the on-screen
	// keyboard types into its box.
	if ((pressed & ControllerState::BUTTON_Y) && focus >= 0 && focus < (Int)targets.size() &&
		BitIsSet(targets[focus].style, GWS_COMBO_BOX))
	{
		GameWindow *edit = editableComboEntry(targets[focus].window);
		if (edit)
		{
			openKeyboard(edit, targets[focus].window);
			return;
		}
	}

	// A and B last: they can close this menu and open another, even before this frame is drawn (a
	// list's Enter accepts a map at once). The frame waits for the next update to find the focus
	// among the live gadgets again.
	if (pressed & ControllerState::BUTTON_A)
	{
		activateMenuTarget(targets, focus);
		m_menuFocusDrawable = FALSE;
	}
	else if (pressed & ControllerState::BUTTON_B)
	{
		menuBack(targets);
		m_menuFocusDrawable = FALSE;
	}
}

/// What Start jumps to outside a match: the button that starts it (Play Game; Accept for a player
/// who joined), or the OK of a screen without one. -1 when there is none.
Int GameController::menuStartTarget(const std::vector<MenuTarget> &targets)
{
	if (!TheGameLogic || (TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()))
		return -1;
	Int start = findMenuTargetNamed(targets, ":ButtonStart");
	if (start < 0)
		start = findMenuTargetNamed(targets, ":ButtonOK");
	if (start < 0)
		start = findMenuTargetNamed(targets, ":ButtonOk");   // the score screen after a match
	if (start < 0)
		start = findMenuTargetNamed(targets, ":ButtonAccept");
	return start;
}

/// B: a message box's No / Cancel / OK; else the screen's own Cancel or Back button (what its Esc
/// key would press, where the Esc key reaches it); otherwise Esc. The "waiting for players" screen
/// has no way back (it goes when the players respond, or by Quit Game): B does nothing there.
void GameController::menuBack(const std::vector<MenuTarget> &targets)
{
	Int back = menuCancelTarget(targets);
	if (back < 0 && isDisconnectScreenOpen())
		return;
	if (back < 0)
		back = findMenuTargetNamed(targets, "Cancel");
	if (back < 0)
		back = findMenuTargetNamed(targets, "Back");
	if (back >= 0)
		clickMenuTarget(targets[back]);
	else
		sendMenuKey(KEY_ESC);
}

//-------------------------------------------------------------------------------------------------
// Drawing
//-------------------------------------------------------------------------------------------------
void GameController::drawMenuFocus()
{
	if (!m_menuFocusShown || !m_menuFocus || !m_menuFocusDrawable || !TheDisplay)
		return;

	// A gold frame around the focused gadget, or around the medal being looked at.
	const Bool cells = m_menuMode == MENU_MODE_CELLS && m_menuCell >= 0 && m_menuCell < (Int)m_menuCells.size();
	const IRegion2D &r = cells ? m_menuCells[m_menuCell] : m_menuFocusRect;
	const Int pad = 3;
	TheDisplay->drawOpenRect(r.lo.x - pad, r.lo.y - pad, (r.hi.x - r.lo.x) + 2 * pad, (r.hi.y - r.lo.y) + 2 * pad,
		3.0f, GameMakeColor(255, 196, 40, 255));

	// What the buttons do here, at the bottom left.
	const char *text;
	if (cells)
		text = "D-pad: next medal   B: done";
	else if (m_menuMode == MENU_MODE_DROPDOWN)
		text = "Up/Down: choose   A: keep it   B: cancel";
	else if (BitIsSet(m_menuFocusStyle, GWS_HORZ_SLIDER))
		text = "Left/Right: adjust   Up/Down: move   B: back";
	else if (BitIsSet(m_menuFocusStyle, GWS_COMBO_BOX))
		text = editableComboEntry(m_menuFocus) ? "A: open the list   Y: type   D-pad: move   B: back" : "A: open the list   D-pad: move   B: back";
	else if (m_menuFocusPicture)
		text = "A: look at the medals   D-pad: move   B: back";
	else if (BitIsSet(m_menuFocusStyle, GWS_SCROLL_LISTBOX))
		text = m_menuHasTabs ? "Up/Down: choose   A: take this one   LB/RB: tabs   B: back" : "Up/Down: choose   A: take this one   B: back";
	else if (BitIsSet(m_menuFocusStyle, GWS_ENTRY_FIELD))
		text = "A: type   D-pad: move   B: back";
	else
		text = "A: select   D-pad / left stick: move   B: back";

	if (!m_menuLegend)
		m_menuLegend = makeString();
	const Int lineHeight = textLineHeight();
	drawText(m_menuLegend, toUnicodeText(text), 8, (Int)TheDisplay->getHeight() - lineHeight - 4, GameMakeColor(255, 220, 120, 255));
}
