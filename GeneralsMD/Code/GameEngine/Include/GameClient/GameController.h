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

// GameController.h ///////////////////////////////////////////////////////////////////////////////
// ControllerMod @feature Halo Wars style game controller support.
//
// Camera and reticle. The left stick moves the camera under a reticle anchored at the centre of
// the tactical view, LT boosts, the right stick rotates and zooms, the right stick click resets.
// Selection and orders. A tap / double tap / hold-brush selects, X gives the native context
// order at the reticle, B cancels or deselects, LB/RB select the army across the map / on screen,
// RT cycles unit types within the selected group. Mouse and keyboard remain fully usable.
//
// Everything here is client side. Selection uses the same InGameUI calls and selected-group
// messages as the mouse, and orders go through GameClient::evaluateContextCommand exactly like a
// mouse click, so the simulation, CRCs, saves, replays and the network see only native messages.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "Common/GameType.h"
#include "Common/UnicodeString.h"
#include "GameClient/ControllerMath.h"

#include <vector>

class CommandButton;
class DisplayString;
class GameWindow;
class Drawable;
class GameFont;
class Object;
class Player;
class ThingTemplate;
class UserPreferences;

//-------------------------------------------------------------------------------------------------
/** Raw state of one physical controller, already normalized. Stick +Y means pushed away from
	the player. Triggers are [0, 1]. */
//-------------------------------------------------------------------------------------------------
struct ControllerState
{
	enum Button
	{
		BUTTON_A          = 0x0001,
		BUTTON_B          = 0x0002,
		BUTTON_X          = 0x0004,
		BUTTON_Y          = 0x0008,
		BUTTON_LB         = 0x0010,
		BUTTON_RB         = 0x0020,
		BUTTON_VIEW       = 0x0040,
		BUTTON_MENU       = 0x0080,
		BUTTON_LS         = 0x0100,
		BUTTON_RS         = 0x0200,
		BUTTON_DPAD_UP    = 0x0400,
		BUTTON_DPAD_DOWN  = 0x0800,
		BUTTON_DPAD_LEFT  = 0x1000,
		BUTTON_DPAD_RIGHT = 0x2000,
		BUTTON_RT         = 0x4000   ///< right trigger as a button (hysteresis applied)
	};

	Bool connected;
	Real leftX, leftY;
	Real rightX, rightY;
	Real leftTrigger, rightTrigger;
	UnsignedInt buttons;

	void clear()
	{
		connected = FALSE;
		leftX = leftY = rightX = rightY = 0.0f;
		leftTrigger = rightTrigger = 0.0f;
		buttons = 0;
	}
};

//-------------------------------------------------------------------------------------------------
/** A replaceable controller backend. The first one is XInput; SDL can follow later behind the
	same interface. Backends only read hardware; they never touch the game. */
//-------------------------------------------------------------------------------------------------
class ControllerDevice
{
public:
	virtual ~ControllerDevice() {}
	virtual Bool init() = 0;                          ///< false when the backend is unavailable on this PC
	virtual Bool poll(ControllerState *state) = 0;   ///< fills state; returns whether a controller is connected
	virtual const char *getBackendName() const = 0;
	virtual Int getActiveSlot() const = 0;            ///< -1 when no controller is bound
};

ControllerDevice *createXInputControllerDevice();

//-------------------------------------------------------------------------------------------------
/** Tunable settings, stored in ControllerMod.ini in the user data folder. These are the starting
	values from the development guide, not measured Halo Wars constants. */
//-------------------------------------------------------------------------------------------------
struct ControllerSettings
{
	Int profileVersion;
	Bool enabled;
	Real innerDeadzoneLeft;
	Real innerDeadzoneRight;
	Real outerLimit;
	Real responseExponent;
	Real triggerDeadzone;
	Real panRate;              ///< viewport widths per second at full deflection
	Real boostMultiplier;      ///< pan multiplier with LT fully pulled
	Real yawRateDegrees;       ///< degrees per second at full deflection
	Real zoomRate;             ///< fraction of the native zoom range per second at full deflection
	Bool invertZoom;
	Bool invertRotate;
	Real smoothingMs;
	Bool edgeOffset;           ///< let the reticle leave the centre when the camera hits the map edge
	Int mouseTakeoverPixels;
	Bool showDiagnostics;

	// Selection
	Int doubleTapMs;           ///< second release within this time after the first = same-type select
	Int holdSelectMs;          ///< holding A this long starts the paint brush
	Real brushRadiusMin;       ///< logical pixels at 1080p
	Real brushRadiusMax;
	Int brushGrowMs;
	Real assistRadius;         ///< target acquisition radius, logical pixels at 1080p (0 = off)
	Real assistRetention;      ///< keep the current target while within this radius
	Real triggerPress;         ///< RT counts as pressed above this
	Real triggerRelease;       ///< ... and released below this
	Bool doubleTapXGuard;      ///< a quick second X turns the order into guard at that spot
	Real brushUnitPadding;     ///< extra hit area around each unit for the brush, logical pixels

	// Command bar
	Real precisionMultiplier;  ///< pan speed with LT fully held while targeting or placing
	Int repeatDelayMs;         ///< held bumper / D-pad: first repeat after this long
	Int repeatRateMs;          ///< ... then every this long
	Bool lockMouseToReticle;   ///< while the controller is in use, the hidden mouse cursor sits on the reticle
	Bool wheelMenus;           ///< panels are shown as radial wheels (left stick points, A uses)
	Bool classicReticle;       ///< the original crosshair and corner brackets instead of the animated reticle
	Bool reducedMotion;        ///< the animated reticle changes state without movement or pulsing
	Bool factionReticle;       ///< the idle reticle takes the colour and tick style of the player's side
	Bool lineMove;             ///< hold X and pan to draw a line; the selected units spread along it
	Bool openWheelOnBuilding;  ///< tapping A on one of our buildings opens its command wheel at once
	Int lineMoveHoldMs;        ///< how long X must be held before the line starts
	Int buttonMap[6];          ///< the physical button (0 A ... 5 RB) that does each function; see ControllerMath

	void setDefaults();
	void validate();
};

//-------------------------------------------------------------------------------------------------
/** The controller front end: polling, stick processing, camera control, reticle and overlays. */
//-------------------------------------------------------------------------------------------------
class GameController
{
public:
	GameController();
	~GameController();

	void init();
	void reset();                  ///< new game or load: clears all transient state
	void update();                 ///< once per client frame, after the mouse update
	void drawWorldOverlay();       ///< reticle; drawn before the HUD windows
	void drawScreenOverlay();      ///< help and diagnostics; drawn after the HUD windows

	/// True while the controller is the input the player is using. Used to suppress screen edge
	/// scrolling so a mouse cursor left at the screen border does not fight the stick.
	Bool isActiveInput() const;

	const ControllerSettings &getSettings() const { return m_settings; }

	/// While the controller places a building, the native placement ghost follows the reticle.
	Bool getPlacementCursor(ICoord2D *pixel) const;
	/// True while the controller is placing a building (the mouse must not drag the anchor).
	Bool isDrivingPlacement() const;

private:
	enum HoverRelation { HOVER_NONE = -1, HOVER_OWN = 0, HOVER_ALLY, HOVER_NEUTRAL, HOVER_ENEMY };
	enum { RETICLE_IDLE_USA = 100, RETICLE_IDLE_CHINA, RETICLE_IDLE_GLA };   ///< reticleColor: faction idle colours

	/// Controller panels. Commands (Y) and Powers (D-pad up) focus real game windows; Groups
	/// (D-pad down) is drawn by the controller.
	enum PanelKind { PANEL_NONE = -1, PANEL_COMMANDS = 0, PANEL_POWERS, PANEL_GROUPS, NUM_PANEL_KINDS };
	enum { MAX_PANEL_REGIONS = 3, NUM_GROUP_LINES = 12, NUM_WHEEL_LINES = 64, MAX_WHEEL_SECTORS = 10 };
	enum PanelRegion
	{
		REGION_COMMANDS = 0, REGION_QUEUE = 1, REGION_ORDERS = 2,   // PANEL_COMMANDS
		REGION_POWERS_USE = 0, REGION_POWERS_PROMOTE = 1,            // PANEL_POWERS
		REGION_GROUPS = 0                                            // PANEL_GROUPS
	};

	/// Explicit order modes entered from the command panel's Orders region.
	enum OrderMode { ORDERMODE_NONE = 0, ORDERMODE_FORCE_ATTACK, ORDERMODE_WAYPOINTS };

	void loadSettings();
	void saveSettings();
	void loadTestInput();
	void applyTestInput(ControllerState *state, Bool inBattle);
	void updateInput();
	void updateCursorLock();
	void applyCursorHiding();
	void onConnected();
	void onDisconnected();
	void clearMotion();
	void beginInputContext();
	void resetTransientInput();
	Bool isNeutral(const ControllerState &state) const;
	Bool isStickNeutral(const ControllerState &state, Int side) const;
	Bool isBattlefieldContext() const;
	Bool mouseActedThisFrame();
	void handleButtons(UnsignedInt pressed, UnsignedInt released, UnsignedInt held, UnsignedInt nowMs);
	void updateCamera(Real dt);
	Bool computeReticleBasis(Real reticleX, Real reticleY, Coord2D *worldPerPxRight, Coord2D *worldPerPxDown) const;
	void getReticleScreenPos(Real *x, Real *y) const;
	static void getApparentIdentity(const Object *obj, const Player *local,
		const Player **apparentPlayer, const ThingTemplate **apparentTemplate);
	void updateHover();
	Real uiScale() const;

	// Selection and orders (GameControllerSelection.cpp)
	void handleWorldButtons(UnsignedInt pressed, UnsignedInt released, UnsignedInt held, UnsignedInt nowMs);
	void updateSelectionGesture(UnsignedInt nowMs);
	void cancelGesture();
	void cancelOrDeselect();
	void onTapReleased(UnsignedInt nowMs);
	void selectSingle(Drawable *draw, Bool playSound);
	void selectArmy(Bool acrossMap);
	void commitBrush();
	void collectBrush();
	void cycleTypeSubset();
	void captureCohortFromSelection();
	void readSelectedIDs(std::vector<ObjectID> *out) const;
	void applySelection(const std::vector<ObjectID> &ids);
	static void sendLogicSelection(const std::vector<ObjectID> &ids, Bool playSound);
	Bool resolveOrderTarget(Drawable **target, Coord3D *pos);
	Int issueOrder();              ///< returns the GameMessage::Type sent, or MSG_INVALID
	void updateOrderHint();
	static const char *orderHintLabel(Int messageType);
	Drawable *findHoverDrawable(UnsignedInt previousID, Bool *assisted) const;
	void drawBrush(Int cx, Int cy);
	void drawSelectionInfo(Int cx, Int cy);
	void issueGuard();
	void evacuateSelectedVehicles();

	// Line move: hold X and pan to draw a line on the ground; the selected units spread along it.
	enum LineState { LINE_IDLE, LINE_PRESSED, LINE_DRAWING };
	void beginLineCandidate(UnsignedInt nowMs);
	void updateLineMove(UnsignedInt nowMs, UnsignedInt held);
	void finishLineMove();
	void collectLineUnits(std::vector<ObjectID> *out) const;
	Bool computeLineTargets(Int count, std::vector<Coord3D> *out) const;
	Bool reticleGround(Coord3D *pos) const;
	void drawLineMove();
	void updatePendingBuildingWheel(UnsignedInt nowMs);

	// Native menus (GameControllerMenus.cpp)
	struct MenuTarget
	{
		GameWindow *window;
		IRegion2D rect;           ///< screen pixels
		ICoord2D clickPoint;      ///< a point where a mouse click reaches this gadget
		UnsignedInt style;        ///< GWS_ gadget style
	};
	void updateMenuNavigation(UnsignedInt pressed, UnsignedInt held, const ControllerState &state, UnsignedInt nowMs);
	void collectMenuTargets(std::vector<MenuTarget> *out, GameWindow **layer) const;
	void addMenuTargets(GameWindow *win, std::vector<MenuTarget> *out) const;
	GameWindow *menuWindowAt(const ICoord2D &point) const;
	Bool findMenuClickPoint(GameWindow *win, ICoord2D *point) const;
	static Int firstMenuTarget(const std::vector<MenuTarget> &targets);
	Int menuCancelTarget(const std::vector<MenuTarget> &targets) const;
	void menuBack(const std::vector<MenuTarget> &targets);
	void dumpMenuTargets(const std::vector<MenuTarget> &targets, GameWindow *layer) const;
	Int findMenuNeighbour(const std::vector<MenuTarget> &targets, Int from, Int dir) const;
	Int menuDirection(UnsignedInt held, const ControllerState &state, UnsignedInt nowMs);
	void placeMenuCursor(const ICoord2D &pos);
	void setMenuFocus(const MenuTarget &target);
	void noteMenuFocus(const MenuTarget &target);
	void clickMenuTarget(const MenuTarget &target);
	void sendMenuKey(UnsignedByte key);
	void activateMenuTarget(const std::vector<MenuTarget> &targets, Int focus);
	void switchMenuTab(const std::vector<MenuTarget> &targets, Int step);
	static Int findMenuTargetNamed(const std::vector<MenuTarget> &targets, const char *suffix);
	Bool menuMouseMoved();
	Bool skipMovieWithPad(UnsignedInt pressed);
	void rememberMenuFocus(GameWindow *layer, GameWindow *focus);
	Int rememberedMenuFocus(GameWindow *layer, const std::vector<MenuTarget> &targets) const;
	void drawMenuFocus();

	// Controller settings screen and button layout (GameControllerSettings.cpp)
	enum { NUM_SETTINGS_LINES = 40 };
	static const char *physicalButtonName(Int physical);
	static Int physicalButtonFromName(const char *name);
	UnicodeString buttonNamesFor(const UnicodeString &text) const;
	void afterButtonMapChange();
	void showSettingsMessage(const char *text);
	void updateEmergencyReset(UnsignedInt nowMs, Bool appActive);
	void loadButtonMap(const UserPreferences &prefs);
	void saveButtonMap(UserPreferences *prefs) const;
	void openSettingsScreen();
	void closeSettingsScreen();
	void changeSetting(Int item, Int steps);
	void assignButtonFor(Int function, Int physical);
	void updateSettingsScreen(UnsignedInt pressed, UnsignedInt held, const ControllerState &state, UnsignedInt nowMs);
	DisplayString *settingsLine(Int index);
	void formatSettingValue(Int item, char *buf, Int size) const;
	void drawSettingsScreen();
	void drawSettingsMessage();
	// Command bar, targeting and placement (GameControllerCommandPanel.cpp)
	struct PanelEntry
	{
		GameWindow *window;   ///< the real game button, or nullptr for an entry drawn by the controller
		Int slot;             ///< native slot for windows; the order or group number otherwise
	};
	Bool isPlacementMode() const;
	Bool isTargetingMode() const;
	void sendClickAtReticle();
	void showFeedback(const char *text);
	void showFeedback(const UnicodeString &text);
	static GameWindow *findGameWindow(const char *parentName, const char *name);
	static Bool isWindowShown(GameWindow *win);
	static Bool isUsableCommandButton(GameWindow *win);
	static Int panelRegionCount(Int kind);
	Bool isPanelRegionAvailable(Int kind, Int region) const;
	void collectPanelEntries(Int kind, Int region, std::vector<PanelEntry> *out) const;
	Bool focusedPanelEntry(PanelEntry *entry, Int *indexOut) const;
	void openPanel(Int kind);
	void closePanel();
	Int panelStepFromButtons(UnsignedInt pressed, UnsignedInt held, UnsignedInt nowMs);
	void stepPanelFocus(Int delta);
	void switchPanelRegion(Int delta);
	void activatePanelWindow(GameWindow *win);
	void activatePanelEntry(const PanelEntry &entry);
	void useFocusedPanelEntry(const PanelEntry &entry, UnsignedInt nowMs);
	void noteFocusMovedByPlayer();
	Bool isFocusedCommandSeen(const PanelEntry &entry, UnsignedInt nowMs);
	void handlePanelButtons(UnsignedInt pressed, UnsignedInt held, UnsignedInt nowMs);
	void updatePanel();
	void handleTargetButtons(UnsignedInt pressed);
	void confirmTarget();
	void handlePlacementButtons(UnsignedInt pressed);
	void confirmPlacement();
	void returnToPanelIfPossible();
	void cancelPowerToBattlefield();
	void drawPanelFocus();
	void drawModeHint(Int cx, Int cy);

	// Powers, groups, jumps and explicit orders (GameControllerPanels.cpp)
	void collectPowerEntries(Int region, std::vector<PanelEntry> *out) const;
	void collectOrderEntries(std::vector<PanelEntry> *out) const;
	void setScienceWindowShown(Bool show);
	Bool isScienceWindowShown() const;
	Bool closeScienceWindow();
	void adjustCursorParkPoint(ICoord2D *pos) const;
	void beginOrderMode(Int mode);
	void endOrderMode(Bool backToPanel);
	void updateOrderMode();
	void handleOrderModeButtons(UnsignedInt pressed);
	void handleGroupPanelButtons(UnsignedInt pressed, UnsignedInt held, UnsignedInt nowMs);
	void recallGroup(Int group);
	void assignGroup(Int group);
	void clearGroup(Int group);
	void focusGroup(Int group);
	void jumpToBase();
	void jumpToAlert();
	void drawGroupPanel();
	void drawOrderEntries(Int x, Int bottomY);
	static const char *orderEntryLabel(Int order);
	void refreshPromotionButtons();

	// Radial wheels over the same panels (GameControllerWheel.cpp). Up to ten entries use one
	// wheel on the left stick; more are split over two wheels, left stick and right stick.
	static Int wheelCountFor(Int total);
	void collectWheelEntries(Int side, std::vector<PanelEntry> *out, Int *wheelCountOut) const;
	Bool wheelFocusedEntry(PanelEntry *entry, Int *indexOut) const;
	void resetWheel();
	void updateWheelSticks(Real lx, Real ly, Real rx, Real ry);
	void handleWheelButtons(UnsignedInt pressed, UnsignedInt held, UnsignedInt nowMs);
	void drawWheel();
	void drawOneWheel(Int side, const std::vector<PanelEntry> &entries, Int firstIndex, Int cx, Int cy, Real outerR, Bool dual);
	UnicodeString panelEntryName(const PanelEntry &entry) const;
	Object *singleProducer() const;
	Int queuedCount(const CommandButton *command) const;
	Int cancelQueued(const CommandButton *command, Bool all);
	DisplayString *makeString();
	void ensureFont();
	Int textLineHeight();
	void drawText(DisplayString *str, const UnicodeString &text, Int x, Int y, UnsignedInt color);
	void drawReticle(Int cx, Int cy, Bool active);
	void drawClassicReticle(Int cx, Int cy, Bool active);
	// Animated reticle (GameControllerReticle.cpp): presentation only
	void drawRangefinder(Int cx, Int cy, Bool active);
	void updateReticleVisual(UnsignedInt candidateID, Bool active, UnsignedInt nowMs);
	void resetReticleVisual();
	static UnsignedInt reticleColor(Int relation, Int alpha, Real brighten);
	void drawHelp();
	void drawDiagnostics();

	ControllerDevice *m_device;
	ControllerSettings m_settings;

	ControllerState m_state;        ///< latest raw state
	UnsignedInt m_prevButtons;
	Bool m_wasConnected;
	Int m_lastSlot;                 ///< the controller slot in use last frame (a different pad is a reconnect)
	Bool m_needNeutral;             ///< ignore input until the pad returns to rest (after connect, focus, load)
	Bool m_suspended;               ///< game window does not have focus
	Bool m_active;                  ///< controller is the input in use
	ICoord2D m_lastMousePos;
	UnsignedInt m_lastMouseEventCount; ///< Mouse::getButtonOrWheelEventCount() at the last check
	Bool m_haveMousePos;
	Int m_mouseTravel;
	Bool m_cursorLocked;            ///< the mouse cursor is parked on the reticle this frame
	Bool m_cursorHidden;            ///< we replaced the cursor image with none for drawing
	Int m_hiddenCursor;             ///< Mouse::MouseCursor to put back when the lock ends
	Bool m_lockPlaced;              ///< m_lockScreenPos holds where the lock last put the real cursor
	ICoord2D m_lockScreenPos;       ///< screen coordinates

	// processed input
	Real m_panX, m_panY;            ///< smoothed left stick after deadzone and curve
	Real m_rotX, m_rotY;            ///< right stick after deadzone and curve
	Real m_boost;                   ///< processed LT
	Real m_rtAnalog;                ///< processed RT: faster movement while aiming or placing

	// camera
	Bool m_wasInBattle;
	Coord2D m_reticleOffset;        ///< pixels from the view centre; non-zero only at map edges
	Real m_worldPerScrollX;         ///< measured world units moved per View::scrollBy unit
	Real m_worldPerScrollY;
	Bool m_scrollScaleValid;
	Bool m_haveLastCameraPos;
	Coord3D m_lastCameraPos;        ///< camera position after our last update, to detect camera jumps
	Bool m_needStickNeutral[2];     ///< after a context change, each stick (0 left, 1 right) must return to rest before it acts again
	Real m_lastDt;

	// hover under the reticle
	UnsignedInt m_hoverDrawableID;
	Int m_hoverRelation;            ///< -1 none, 0 own, 1 ally, 2 neutral, 3 enemy
	// Animated reticle: visual state only, never read by the input code
	UnsignedInt m_reticleShownID;   ///< the candidate the reticle shows (drawable ID, 0 = none)
	UnsignedInt m_reticleAcquireMs; ///< when its acquisition animation started
	Bool m_reticleAnimating;
	UnsignedInt m_reticleLastAnimMs;
	Bool m_reticleAnimatedOnce;
	UnsignedInt m_reticleLastDrawMs; ///< 0 = not drawn yet
	UnicodeString m_hoverName;
	Bool m_hoverHasGround;
	Coord3D m_hoverGround;
	Bool m_hoverAssisted;           ///< found by target assist, not directly under the reticle

	// A-button gesture (tap / double tap / hold brush)
	enum GestureState { GESTURE_IDLE, GESTURE_PRESSED, GESTURE_BRUSH };
	Int m_gesture;
	UnsignedInt m_gestureDownMs;
	UnsignedInt m_contextGeneration;   ///< bumped by beginInputContext(); a gesture only commits in its own context
	UnsignedInt m_gestureGeneration;
	ObjectID m_tapCandidate;           ///< captured when A goes down (the visible bracket)
	UnsignedInt m_lastTapReleaseMs;
	ObjectID m_lastTapObject;
	UnsignedInt m_lastTapGeneration;
	std::vector<ObjectID> m_brushIDs;
	Real m_brushRadius;

	// Selection cohort for RT type cycling
	std::vector<ObjectID> m_cohort;
	std::vector<ObjectID> m_appliedSelection;  ///< sorted; what our last selection change produced
	Int m_typeIndex;                           ///< 0 = All, 1..n = one type
	const ThingTemplate *m_typeShown;          ///< the one type selected now (nullptr = All)
	UnicodeString m_typeLabel;
	Bool m_rtDown;
	UnsignedInt m_bumperPending;    ///< LB or RB waiting briefly for its partner (LB+RB = unload)
	UnsignedInt m_bumperDownMs;
	UnsignedInt m_bumperGeneration;

	// Order under the reticle
	Int m_orderHint;                ///< GameMessage::Type the context evaluator would produce, or MSG_INVALID
	UnsignedInt m_orderFeedbackUntilMs;
	UnicodeString m_orderFeedback;
	UnsignedInt m_lastOrderMs;      ///< for double-tap X = guard
	UnsignedInt m_lastOrderGeneration;

	// Panel focus and native modes
	Bool m_panelOpen;
	Int m_panelKind;                ///< PanelKind of the open panel
	Int m_panelRegion;              ///< region within the panel (Commands/Queue/Orders, Use/Promote)
	Int m_panelSlot[NUM_PANEL_KINDS][MAX_PANEL_REGIONS]; ///< focused slot per panel and region
	Int m_returnPanel;              ///< PanelKind that B from targeting/placement/order mode goes back to
	const CommandButton *m_focusCommand; ///< command on the focused button, to notice when it changes
	UnsignedInt m_focusChangedMs;
	Int m_waypointCount;            ///< waypoints added in the current waypoint mode
	Bool m_wheelHighlight;          ///< single wheel: a sector is highlighted (m_panelSlot holds it)
	Bool m_dualActive[2];           ///< two wheels: that side has a highlight (it stays when the stick is let go)
	Int m_dualSlot[2];              ///< ... and which slot
	Int m_dualCurrent;              ///< the wheel pointed at last (A / left-stick click use it), or -1
	Bool m_stickPushed[2];          ///< each stick was pushed last frame
	UnsignedInt m_xDownMs;          ///< X held on a queued item: holding cancels all of that item
	Int m_xHoldSlot;
	ObjectID m_xHoldProducer;       ///< the building the held X started on; the hold ends if that changes
	UnsignedInt m_xHoldButton;      ///< X or LB, whichever started the hold
	Bool m_xHoldDone;
	UnsignedInt m_promotionRefreshMs;

	// A tap on a building opens its wheel once the command bar shows that building
	ObjectID m_pendingWheelObject;
	UnsignedInt m_pendingWheelUntilMs;

	// line move
	Int m_lineState;
	UnsignedInt m_lineDownMs;
	UnsignedInt m_lineGeneration;
	std::vector<Coord3D> m_linePoints;
	Bool m_lineGuard;               ///< clicking the left stick while drawing: the line guards instead of moving
	UnsignedInt m_sellConfirmUntilMs; ///< selling several objects needs a second A before this time
	Bool m_scienceShownByUs;        ///< we opened the promotion window and close it again
	Int m_orderMode;                ///< OrderMode
	Int m_groupClearPending;        ///< group waiting for the clear confirmation, or -1
	Int m_baseJumpIndex;
	Int m_alertJumpIndex;
	UnsignedInt m_alertNewestFrame; ///< newest alert when the alert cycle started
	UnsignedInt m_lastAlertJumpMs;
	UnsignedInt m_repeatButton;
	UnsignedInt m_repeatNextMs;
	Bool m_wasNativeMode;
	Bool m_checkTargetResult;       ///< a target click was sent; check next frame whether it was taken
	const CommandButton *m_checkTargetCommand;

	// overlays
	Bool m_helpVisible;
	GameFont *m_font;
	Int m_fontSize;
	enum { MAX_TEXT_LINES = 12 };
	DisplayString *m_lines[MAX_TEXT_LINES];
	DisplayString *m_hoverString;
	DisplayString *m_infoLines[3];
	DisplayString *m_panelLines[2];
	DisplayString *m_groupLines[NUM_GROUP_LINES];
	DisplayString *m_wheelLines[NUM_WHEEL_LINES];

	// Menus. m_menuFocus is only ever compared with the gadgets found this frame, never used
	// on its own, so a closed menu cannot leave a dangling window behind. Drawing uses only the
	// copies below, taken while the gadget was found alive.
	GameWindow *m_menuFocus;
	IRegion2D m_menuFocusRect;
	UnsignedInt m_menuFocusStyle;
	Bool m_menuFocusPicture;        ///< the focused gadget is a picture list (the medals)
	Bool m_menuFocusDrawable;       ///< the copies are from this frame's live gadgets (not after A/B/tabs, which can close the menu)
	Bool m_menuFocusShown;          ///< the pad drives the menu (the mouse hides it until the pad is used)
	Int m_menuRepeatDir;
	UnsignedInt m_menuRepeatNextMs;
	enum { MENU_MODE_NORMAL, MENU_MODE_DROPDOWN, MENU_MODE_CELLS };
	Int m_menuMode;                 ///< normal, a drop-down list open, or looking at the medals
	GameWindow *m_menuModeWindow;   ///< the drop-down list or medal list of that mode (compared only)
	Int m_menuDropdownOriginal;     ///< the drop-down's choice when it opened (B puts it back)
	std::vector<IRegion2D> m_menuCells;  ///< medal cells, screen pixels
	Int m_menuCell;
	std::vector<GameWindow *> m_menuPrevStops;  ///< last frame's stops (compared only)
	Bool m_menuHasTabs;             ///< the menu has tabs for LB/RB
	UnsignedInt m_menuSettleUntilMs; ///< after skipping a movie or revealing the main menu, A/B/Start rest until then
	ICoord2D m_menuCursorScreen;    ///< real cursor position last seen or placed, screen coordinates
	Bool m_menuCursorKnown;
	UnsignedInt m_menuMouseEvents;
	std::vector< std::pair<GameWindow *, GameWindow *> > m_menuMemory;  ///< last focus per menu
	DisplayString *m_menuLegend;

	// Settings screen and button layout
	Bool m_settingsOpen;
	Int m_settingsPage;
	Int m_settingsRow;
	ControllerSettings m_settingsBackup;   ///< the settings when the screen opened (X undoes to them)
	Int m_settingsCapture;                 ///< function waiting for its new button, or -1
	UnsignedInt m_settingsCaptureUntilMs;
	UnsignedInt m_settingsDefaultsAskMs;   ///< first Y of "page defaults"
	UnicodeString m_settingsMessage;
	UnsignedInt m_settingsMessageUntilMs;
	DisplayString *m_settingsLines[NUM_SETTINGS_LINES];
	UnsignedInt m_rawButtons;              ///< physical buttons this frame, before the layout
	UnsignedInt m_rawPrevButtons;
	ControllerMath::HoldLatch m_resetHold; ///< both stick clicks held (emergency default buttons)
	Bool m_resetKeysDown;
	Bool m_textRemapOff;                   ///< drawText shows the text as it is (real button names)

	// Automated test support only (CONTROLLERMOD_TEST_INPUT, see loadTestInput)
	struct TestStep
	{
		UnsignedInt atMs;
		UnsignedInt untilMs;
		UnsignedInt buttons;
		Real stickX, stickY;   ///< left stick, used when hasStick
		Bool hasStick;
		Real rightX, rightY;   ///< right stick, used when hasRight
		Bool hasRight;
		Bool holdLT;           ///< left trigger fully pulled
		Bool disconnect;       ///< DISC: the pad reads as unplugged
		Bool swapSlot;         ///< SWAP: another pad (another slot) is the one read
	};
	void applyTestConnection(ControllerState *state, Int *slot);
	std::vector<TestStep> m_testSteps;
	UnsignedInt m_testStartMs;
	Bool m_testFromStart;      ///< "shell;" prefix: timed from the first frame (menus), not the first battle frame
};

extern GameController *TheGameController;
