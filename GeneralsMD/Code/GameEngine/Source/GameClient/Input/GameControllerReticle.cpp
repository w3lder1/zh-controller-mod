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

// GameControllerReticle.cpp //////////////////////////////////////////////////////////////////////
// ControllerMod @feature Animated reticle.
//
// Four corner brackets around a small, fixed centre point, a short tick at the middle of each side,
// crisp horizontal and vertical strokes (whole pixels, so they stay sharp at low resolutions) on a
// thin dark outline. Idle: the side's colour, the top tick pulses slowly. On a hover candidate: its
// relationship colour at once, the side ticks turn inward, and the frame settles in from just outside
// (2 px over 0.15 s, barely visible: owner), once per candidate change. Enemies also get a second,
// inner set of corners, so the state reads by shape and not only by colour.
//
// Presentation only. Nothing here reads the pad, consumes buttons, changes modes or evaluates
// commands: it reads the hover, gesture and mode state the input code already produced, and keeps
// its own visual state (m_reticle*). The centre point is always the real aim point; with target
// assist the arcs mark the candidate where it is on screen. The setting ClassicReticle draws the
// original crosshair and corner brackets; ReducedMotion keeps every state change but replaces the
// movement with a brief brightness change.
//
// Faction style (setting FactionReticle): while idle, the arcs and gap ticks take the colour and tick
// shape of the player's main side (every general of a side looks the same): USA pale steel with
// straight ticks, China pale brass with double ticks, GLA rust-tan with V ticks. The meaning colours
// (olive own, cyan ally, amber neutral, red enemy/invalid) never change, and no side's idle colour is
// red, cyan or amber; the centre point is the same for all.
//
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameController.h"
#include "GameClient/ControllerMath.h"

#include "Common/Player.h"
#include "Common/PlayerList.h"

#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/View.h"

namespace
{
	const Real REFERENCE_HEIGHT = 1080.0f;       ///< sizes below are pixels at 1080p
	const Real FRAME_HALF = 20.0f;               ///< half the frame's width: about 44 px across with the strokes
	const Real ACQUIRE_SPREAD = 2.0f;            ///< the frame starts this much further out on a new candidate (barely)
	const UnsignedInt ACQUIRE_SETTLE_MS = 150;   ///< ... and glides in over this long
	const UnsignedInt ACQUIRE_MIN_GAP_MS = 300;  ///< no new acquisition animation sooner than this
	const UnsignedInt PULSE_PERIOD_MS = 2400;    ///< the idle accent tick
	const UnsignedInt STALE_GAP_MS = 400;        ///< not drawn for this long: animations are over

	// A unit circle in 7.5 degree steps (angle 0 = right, 90 = down on screen).
	const Int CIRCLE_STEPS = 48;
	Real s_circleX[CIRCLE_STEPS];
	Real s_circleY[CIRCLE_STEPS];
	Bool s_circleReady = FALSE;

	void prepareCircle()
	{
		if (s_circleReady)
			return;
		for (Int i = 0; i < CIRCLE_STEPS; ++i)
		{
			const double a = (2.0 * PI * i) / CIRCLE_STEPS;
			s_circleX[i] = (Real)cos(a);
			s_circleY[i] = (Real)sin(a);
		}
		s_circleReady = TRUE;
	}

	inline Int wrapStep(Int i)
	{
		return ((i % CIRCLE_STEPS) + CIRCLE_STEPS) % CIRCLE_STEPS;
	}

	void drawRadialTick(Real cx, Real cy, Int step, Real r1, Real r2, Real width, UnsignedInt color)
	{
		const Real dx = s_circleX[wrapStep(step)];
		const Real dy = s_circleY[wrapStep(step)];
		TheDisplay->drawLine(REAL_TO_INT(cx + dx * r1), REAL_TO_INT(cy + dy * r1),
			REAL_TO_INT(cx + dx * r2), REAL_TO_INT(cy + dy * r2), width, color);
	}

	enum { FACTION_NONE, FACTION_USA, FACTION_CHINA, FACTION_GLA };
	enum { TICK_STRAIGHT, TICK_DOUBLE, TICK_V };

	/// The local player's main side (a general's side counts as its base side).
	Int localFaction()
	{
		const Player *local = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		if (!local)
			return FACTION_NONE;
		AsciiString side = local->getBaseSide();
#if CONTROLLERMOD_ENABLE_TEST_INPUT
		// Automated testing only: show another side's style (test builds; player builds ignore it).
		const char *testSide = getenv("CONTROLLERMOD_TEST_FACTION");
		if (testSide && *testSide)
			side = testSide;
#endif
		if (side.isEmpty())
			side = local->getSide();
		if (side.compareNoCase("America") == 0 || side.compareNoCase("USA") == 0)
			return FACTION_USA;
		if (side.compareNoCase("China") == 0)
			return FACTION_CHINA;
		if (side.compareNoCase("GLA") == 0)
			return FACTION_GLA;
		return FACTION_NONE;
	}

	/// The four corners of a square frame of half-size h around (cx, cy); arms of length arm.
	void drawCorners(Int cx, Int cy, Int h, Int arm, Real width, UnsignedInt color)
	{
		for (Int sx = -1; sx <= 1; sx += 2)
		{
			for (Int sy = -1; sy <= 1; sy += 2)
			{
				const Int x = cx + sx * h;
				const Int y = cy + sy * h;
				TheDisplay->drawLine(x, y, x - sx * arm, y, width, color);
				TheDisplay->drawLine(x, y, x, y - sy * arm, width, color);
			}
		}
	}

	/// A side tick in the side's style, from radius r1 to r2 (r2 is the end it points to).
	void drawGapTick(Real cx, Real cy, Int step, Real r1, Real r2, Real width, UnsignedInt color, Int style, Real px)
	{
		const Real dx = s_circleX[wrapStep(step)];
		const Real dy = s_circleY[wrapStep(step)];
		if (style == TICK_DOUBLE)
		{
			// Two parallel ticks.
			const Real o = 1.6f * px;
			for (Int k = -1; k <= 1; k += 2)
			{
				const Real ox = -dy * o * k;
				const Real oy = dx * o * k;
				TheDisplay->drawLine(REAL_TO_INT(cx + dx * r1 + ox), REAL_TO_INT(cy + dy * r1 + oy),
					REAL_TO_INT(cx + dx * r2 + ox), REAL_TO_INT(cy + dy * r2 + oy), width, color);
			}
		}
		else if (style == TICK_V)
		{
			// A V whose point is at r2.
			const Real o = 2.4f * px;
			const Int tipX = REAL_TO_INT(cx + dx * r2);
			const Int tipY = REAL_TO_INT(cy + dy * r2);
			for (Int k = -1; k <= 1; k += 2)
				TheDisplay->drawLine(REAL_TO_INT(cx + dx * r1 - dy * o * k), REAL_TO_INT(cy + dy * r1 + dx * o * k), tipX, tipY, width, color);
		}
		else
		{
			TheDisplay->drawLine(REAL_TO_INT(cx + dx * r1), REAL_TO_INT(cy + dy * r1),
				REAL_TO_INT(cx + dx * r2), REAL_TO_INT(cy + dy * r2), width, color);
		}
	}

	UnsignedInt withAlpha(UnsignedByte r, UnsignedByte g, UnsignedByte b, Int alpha)
	{
		if (alpha < 0) alpha = 0;
		if (alpha > 255) alpha = 255;
		return GameMakeColor(r, g, b, (UnsignedByte)alpha);
	}
}

/// Muted, period colours: khaki idle, olive own, cyan ally, amber neutral, brick red enemy.
UnsignedInt GameController::reticleColor(Int relation, Int alpha, Real brighten)
{
	Int r = 196, g = 186, b = 140;   // khaki
	switch (relation)
	{
		case HOVER_OWN:     r = 150; g = 196; b = 90; break;    // olive
		case HOVER_ALLY:    r = 110; g = 196; b = 206; break;   // subdued cyan
		case HOVER_NEUTRAL: r = 232; g = 172; b = 56; break;    // amber
		case HOVER_ENEMY:   r = 206; g = 72; b = 52; break;     // brick red
		// Idle colours of the faction style: none of them red, cyan or amber.
		case RETICLE_IDLE_USA:   r = 206; g = 214; b = 222; break;   // pale steel
		case RETICLE_IDLE_CHINA: r = 226; g = 208; b = 150; break;   // pale brass
		case RETICLE_IDLE_GLA:   r = 188; g = 146; b = 108; break;   // rust-tan
		default: break;
	}
	if (brighten > 0.0f)
	{
		r += REAL_TO_INT((255 - r) * brighten);
		g += REAL_TO_INT((255 - g) * brighten);
		b += REAL_TO_INT((255 - b) * brighten);
	}
	return withAlpha((UnsignedByte)r, (UnsignedByte)g, (UnsignedByte)b, alpha);
}

//-------------------------------------------------------------------------------------------------
/** Forget the reticle's animation state (a new game, loading, a long pause in drawing). */
//-------------------------------------------------------------------------------------------------
void GameController::resetReticleVisual()
{
	m_reticleShownID = 0;
	m_reticleAcquireMs = 0;
	m_reticleAnimating = FALSE;
	m_reticleLastAnimMs = 0;
	m_reticleAnimatedOnce = FALSE;
	m_reticleLastDrawMs = 0;
}

//-------------------------------------------------------------------------------------------------
/** Follows the hover candidate the input code found this frame; starts the acquisition animation
	once per change. Visual state only. */
//-------------------------------------------------------------------------------------------------
void GameController::updateReticleVisual(UnsignedInt candidateID, Bool active, UnsignedInt nowMs)
{
	// Not drawn for a while (focus lost, a menu, loading, the mouse): whatever was animating is over,
	// and coming back does not replay it.
	if (m_reticleLastDrawMs == 0 || nowMs - m_reticleLastDrawMs > STALE_GAP_MS)
	{
		m_reticleAnimating = FALSE;
		m_reticleShownID = candidateID;
	}
	m_reticleLastDrawMs = nowMs;

	if (candidateID != m_reticleShownID)
	{
		m_reticleShownID = candidateID;
		m_reticleAnimating = FALSE;
		if (candidateID != 0 && active &&
			ControllerMath::reticleShouldAnimate(m_reticleAnimatedOnce != FALSE, nowMs - m_reticleLastAnimMs, ACQUIRE_MIN_GAP_MS))
		{
			m_reticleAnimating = TRUE;
			m_reticleAcquireMs = nowMs;
			m_reticleLastAnimMs = nowMs;
			m_reticleAnimatedOnce = TRUE;
		}
	}
	if (m_reticleAnimating && nowMs - m_reticleAcquireMs >= ACQUIRE_SETTLE_MS)
		m_reticleAnimating = FALSE;
}

//-------------------------------------------------------------------------------------------------
/** The Rangefinder reticle. (cx, cy) is the real aim point. */
//-------------------------------------------------------------------------------------------------
void GameController::drawRangefinder(Int cx, Int cy, Bool active)
{
	prepareCircle();
	const UnsignedInt nowMs = timeGetTime();
	const Real scale = TheDisplay->getHeight() / REFERENCE_HEIGHT;
	const Real px = scale < 0.6f ? 0.6f : scale;
	Real stroke = 2.0f * px;
	if (stroke < 1.5f) stroke = 1.5f;
	if (stroke > 3.0f) stroke = 3.0f;
	const Real outline = stroke + 2.0f;

	// The candidate, checked again here: one that died or was hidden since the hover was found
	// leaves no marker.
	Int relation = (Int)m_hoverRelation;
	UnsignedInt candidateID = m_hoverDrawableID;
	Real ax = (Real)cx;
	Real ay = (Real)cy;
	if (relation != HOVER_NONE)
	{
		Drawable *candidate = TheGameClient ? TheGameClient->findDrawableByID((DrawableID)m_hoverDrawableID) : nullptr;
		if (!candidate || candidate->isDrawableEffectivelyHidden())
		{
			relation = HOVER_NONE;
			candidateID = 0;
		}
		else if (m_hoverAssisted)
		{
			ICoord2D s;
			if (TheTacticalView->worldToScreen(candidate->getPosition(), &s))
			{
				ax = (Real)s.x;
				ay = (Real)s.y;
			}
			else
			{
				relation = HOVER_NONE;
				candidateID = 0;
			}
		}
	}

	// A building ghost or the paint brush already says what is happening: a smaller, quieter
	// reticle that does not cover it.
	const Bool quiet = isPlacementMode() || m_gesture == GESTURE_BRUSH;
	if (quiet)
	{
		relation = HOVER_NONE;
		candidateID = 0;
	}
	updateReticleVisual(candidateID, active, nowMs);

	const Bool reduced = m_settings.reducedMotion;
	const Bool animating = active && m_reticleAnimating && candidateID != 0;
	const UnsignedInt elapsed = nowMs - m_reticleAcquireMs;

	Real half = FRAME_HALF * px * (quiet ? 0.6f : 1.0f);
	Real brighten = 0.0f;
	if (animating)
	{
		if (reduced)
			brighten = 0.45f;   // no movement: a brief brighter frame instead
		else
			half += ACQUIRE_SPREAD * px * ControllerMath::reticleAcquireSpread(elapsed, ACQUIRE_SETTLE_MS);
	}

	const Int alpha = !active ? 80 : (quiet ? 150 : 230);
	const Int shadowAlpha = !active ? 50 : (quiet ? 100 : 170);
	// Faction style: the idle colour and tick shape of the player's side. Hover colours are the
	// same for every side.
	const Int faction = m_settings.factionReticle ? localFaction() : FACTION_NONE;
	Int tickStyle = TICK_STRAIGHT;
	if (faction == FACTION_CHINA)
		tickStyle = TICK_DOUBLE;
	else if (faction == FACTION_GLA)
		tickStyle = TICK_V;
	const Int idleRelation = (faction == FACTION_USA) ? RETICLE_IDLE_USA : (faction == FACTION_CHINA) ? RETICLE_IDLE_CHINA :
		(faction == FACTION_GLA) ? RETICLE_IDLE_GLA : HOVER_NONE;
	const UnsignedInt color = reticleColor(relation == HOVER_NONE ? idleRelation : relation, alpha, brighten);
	const UnsignedInt shadow = withAlpha(0, 0, 0, shadowAlpha);
	const Bool hovering = relation != HOVER_NONE;
	const Bool enemy = relation == HOVER_ENEMY;

	// The accent tick at the top gap: a slow brightness pulse while idle (steady with reduced motion).
	Int accentAlpha = alpha;
	if (active && !hovering && !quiet && !reduced)
		accentAlpha = 185 + REAL_TO_INT(45.0f * ControllerMath::reticlePulse(nowMs, PULSE_PERIOD_MS));
	const UnsignedInt accent = reticleColor(idleRelation, accentAlpha, 0.35f);

	// Whole pixels, so the straight strokes stay crisp.
	const Int fx = REAL_TO_INT(ax);
	const Int fy = REAL_TO_INT(ay);
	const Int h = REAL_TO_INT(half);
	const Int arm = REAL_TO_INT(half * 0.42f) < 3 ? 3 : REAL_TO_INT(half * 0.42f);
	const Real tickLen = 5.0f * px;
	static const Int SIDE_STEPS[4] = { 36, 0, 12, 24 };   // top, right, bottom, left
	for (Int pass = 0; pass < 2; ++pass)
	{
		const Bool dark = pass == 0;
		const Real w = dark ? outline : stroke;
		// The frame, on the candidate (the real aim point when not assisted).
		drawCorners(fx, fy, h, arm, w, dark ? shadow : color);
		// Enemy: a second, inner set of corners.
		if (enemy)
		{
			const Int inner = h - REAL_TO_INT(4.0f * px) - 1;
			drawCorners(fx, fy, inner, arm * 2 / 3 < 2 ? 2 : arm * 2 / 3, w, dark ? shadow : color);
		}
		// Side ticks: outward while idle, inward on a candidate.
		for (Int k = 0; k < 4; ++k)
		{
			const UnsignedInt c = dark ? shadow : ((k == 0 && !hovering) ? accent : color);
			if (hovering)
				drawGapTick((Real)fx, (Real)fy, SIDE_STEPS[k], (Real)h - 1.0f, (Real)h - tickLen - 1.0f, w, c, tickStyle, px);
			else
				drawGapTick((Real)fx, (Real)fy, SIDE_STEPS[k], (Real)h + 1.0f, (Real)h + tickLen, w, c, tickStyle, px);
		}
	}

	// The centre point: always the real aim point, never animated.
	const Int dot = REAL_TO_INT(2.0f * px) < 2 ? 2 : REAL_TO_INT(2.0f * px);
	const Int dotAlpha = (m_gesture == GESTURE_BRUSH) ? alpha / 2 : alpha;
	TheDisplay->drawFillRect(cx - dot / 2 - 1, cy - dot / 2 - 1, dot + 2, dot + 2, withAlpha(0, 0, 0, shadowAlpha));
	TheDisplay->drawFillRect(cx - dot / 2, cy - dot / 2, dot, dot, reticleColor(HOVER_NONE, dotAlpha, 0.5f));

	// Assisted: the arcs are on the candidate, so the aim point gets four small ticks of its own.
	if (hovering && (ax != (Real)cx || ay != (Real)cy))
	{
		const Real inner = 4.0f * px;
		const Real outer = 8.0f * px;
		for (Int k = 0; k < 4; ++k)
		{
			drawRadialTick((Real)cx, (Real)cy, k * (CIRCLE_STEPS / 4), inner, outer, outline, shadow);
			drawRadialTick((Real)cx, (Real)cy, k * (CIRCLE_STEPS / 4), inner, outer, stroke, reticleColor(HOVER_NONE, alpha, 0.0f));
		}
	}
}
