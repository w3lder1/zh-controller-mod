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

// ControllerMath.h ///////////////////////////////////////////////////////////////////////////////
// ControllerMod @feature Pure input math for the game controller: axis normalization, radial
// deadzones, response curves and trigger hysteresis. Has no engine dependencies so it can be
// unit tested on its own (see ControllerMod/tests). Keep it C++98 compatible for VC6 builds.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <math.h>

namespace ControllerMath
{

/// Converts a signed 16 bit stick axis to [-1, 1] using the correct divisor for each sign,
/// so that both -32768 and +32767 map exactly to the ends of the range.
inline float normalizeAxis(short value)
{
	if (value < 0)
		return (float)value / 32768.0f;
	return (float)value / 32767.0f;
}

/// Converts an 8 bit trigger value to [0, 1].
inline float normalizeTrigger(unsigned char value)
{
	return (float)value / 255.0f;
}

inline float clampf(float lo, float v, float hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

/// Applies a radial (circular) deadzone and a response curve to a stick.
///
/// Magnitudes at or below innerDeadzone produce (0, 0). Magnitudes at or above outerLimit produce
/// full deflection. In between, the magnitude is rescaled to [0, 1] and raised to responseExponent.
/// The direction of the stick is preserved, so diagonals are never faster than straight pushes.
/// Invalid settings (outer <= inner, exponent <= 0) fall back to safe values instead of dividing by 0.
inline void processStick(float x, float y, float innerDeadzone, float outerLimit, float responseExponent,
	float *outX, float *outY)
{
	*outX = 0.0f;
	*outY = 0.0f;

	innerDeadzone = clampf(0.0f, innerDeadzone, 0.95f);
	if (outerLimit <= innerDeadzone + 0.01f)
		outerLimit = innerDeadzone + 0.01f;
	if (outerLimit > 1.0f)
		outerLimit = 1.0f;
	if (responseExponent <= 0.0f)
		responseExponent = 1.0f;

	const float magnitude = (float)sqrt((double)(x * x + y * y));
	if (magnitude <= innerDeadzone)
		return;

	const float t = clampf(0.0f, (magnitude - innerDeadzone) / (outerLimit - innerDeadzone), 1.0f);
	const float scaled = (float)pow((double)t, (double)responseExponent);

	*outX = (x / magnitude) * scaled;
	*outY = (y / magnitude) * scaled;
}

/// Rescales a trigger so values at or below deadzone read as 0 and full pull reads as 1.
inline float processTrigger(float value, float deadzone)
{
	deadzone = clampf(0.0f, deadzone, 0.95f);
	if (value <= deadzone)
		return 0.0f;
	return clampf(0.0f, (value - deadzone) / (1.0f - deadzone), 1.0f);
}

/// Trigger treated as a button with hysteresis: it turns on above pressThreshold and only turns
/// off again below releaseThreshold, so a trigger resting near one threshold cannot flicker.
inline bool updateTriggerButton(bool wasPressed, float value, float pressThreshold, float releaseThreshold)
{
	if (wasPressed)
		return value > releaseThreshold;
	return value >= pressThreshold;
}

/// Frame-rate independent exponential smoothing factor for a time constant in seconds.
/// Returns 1 (no smoothing) when the time constant is zero or negative.
inline float smoothingAlpha(float dtSeconds, float timeConstantSeconds)
{
	if (timeConstantSeconds <= 0.0f || dtSeconds <= 0.0f)
		return 1.0f;
	return 1.0f - (float)exp((double)(-dtSeconds / timeConstantSeconds));
}

/// Moves value toward zero by amount without crossing zero. Returns what was consumed.
inline float consumeTowardZero(float *value, float amount)
{
	if (amount <= 0.0f)
		return 0.0f;
	if (*value > 0.0f)
	{
		const float used = (*value < amount) ? *value : amount;
		*value -= used;
		return used;
	}
	if (*value < 0.0f)
	{
		const float used = (-*value < amount) ? -*value : amount;
		*value += used;
		return used;
	}
	return 0.0f;
}

inline float signf(float v)
{
	return (v > 0.0f) ? 1.0f : ((v < 0.0f) ? -1.0f : 0.0f);
}

inline float minAbs(float a, float b)
{
	const float fa = (float)fabs((double)a);
	const float fb = (float)fabs((double)b);
	return fa < fb ? fa : fb;
}

/// Solves (wx, wy) = a * (rx, ry) + b * (dx, dy). Returns false for a degenerate basis.
inline bool solveInBasis(float wx, float wy, float rx, float ry, float dx, float dy, float *a, float *b)
{
	const float det = rx * dy - ry * dx;
	if ((float)fabs((double)det) < 1e-12f)
		return false;
	*a = (wx * dy - wy * dx) / det;
	*b = (rx * wy - ry * wx) / det;
	return true;
}

/// Screen-space pan request and reticle offset at the map border.
///
/// Inputs are in screen pixels at the reticle: req is this frame's pan request, offset is how far
/// the reticle currently sits from the view centre. (rightX, rightY) and (downX, downY) are the world
/// displacement for one pixel of screen movement to the right and down. The camera pivot at
/// (posX, posY) is clipped by the view to the axis aligned area [lo, hi].
///
/// 1. A request opposite to the offset first walks the reticle back toward the centre.
/// 2. If hasArea, offset the camera no longer needs is handed back to the camera, at most
///    handBackPx per call: camera and reticle move together, so the aimed world point stays put.
/// 3. If hasArea, the part of the request that the area would clip becomes reticle offset instead,
///    predicted from the area itself, so unrelated camera movement can never be mistaken for it.
inline void resolveEdgeOffset(float *reqX, float *reqY, float *offsetX, float *offsetY,
	float posX, float posY, float rightX, float rightY, float downX, float downY,
	bool hasArea, float areaLoX, float areaLoY, float areaHiX, float areaHiY, float handBackPx)
{
	float *req[2] = { reqX, reqY };
	float *offset[2] = { offsetX, offsetY };

	for (int axis = 0; axis < 2; ++axis)
	{
		if (*req[axis] != 0.0f && signf(*req[axis]) == -signf(*offset[axis]))
		{
			const float used = consumeTowardZero(offset[axis], (float)fabs((double)*req[axis]));
			*req[axis] -= used * signf(*req[axis]);
		}
	}

	if (!hasArea)
		return;

	for (int axis = 0; axis < 2; ++axis)
	{
		if (*offset[axis] != 0.0f && (*req[axis] == 0.0f || signf(*req[axis]) == signf(*offset[axis])))
		{
			const float give = minAbs(*offset[axis], handBackPx) * signf(*offset[axis]);
			*offset[axis] -= give;
			*req[axis] += give;
		}
	}

	const float startX = clampf(areaLoX, posX, areaHiX);
	const float startY = clampf(areaLoY, posY, areaHiY);
	const float targetX = startX + *reqX * rightX + *reqY * downX;
	const float targetY = startY + *reqX * rightY + *reqY * downY;
	const float blockedWorldX = targetX - clampf(areaLoX, targetX, areaHiX);
	const float blockedWorldY = targetY - clampf(areaLoY, targetY, areaHiY);
	if (blockedWorldX == 0.0f && blockedWorldY == 0.0f)
		return;

	float blocked[2];
	if (!solveInBasis(blockedWorldX, blockedWorldY, rightX, rightY, downX, downY, &blocked[0], &blocked[1]))
		return;

	for (int axis = 0; axis < 2; ++axis)
	{
		// Only the part pushing in the requested direction moves the reticle; the rest slides
		// along the border and is clipped by the view as usual.
		if (*req[axis] != 0.0f && signf(blocked[axis]) == signf(*req[axis]))
		{
			const float amount = minAbs(blocked[axis], *req[axis]) * signf(*req[axis]);
			*offset[axis] += amount;
			*req[axis] -= amount;
		}
	}
}

/// Length of a polyline through count points.
inline float pathLength(const float *xs, const float *ys, int count)
{
	float total = 0.0f;
	for (int i = 1; i < count; ++i)
	{
		const float dx = xs[i] - xs[i - 1];
		const float dy = ys[i] - ys[i - 1];
		total += (float)sqrt((double)(dx * dx + dy * dy));
	}
	return total;
}

/// n points spaced evenly along a polyline (by distance), from its start to its end inclusive.
/// n == 1 gives the end. zs is interpolated the same way. Returns false for an empty path.
inline bool spacePointsAlongPath(const float *xs, const float *ys, const float *zs, int count, int n,
	float *outX, float *outY, float *outZ)
{
	if (count < 1 || n < 1)
		return false;
	const float total = pathLength(xs, ys, count);
	int seg = 1;
	float segStart = 0.0f;   // distance along the path at the start of segment seg
	for (int k = 0; k < n; ++k)
	{
		const float want = (n == 1) ? total : total * (float)k / (float)(n - 1);
		if (count == 1 || total <= 0.0f)
		{
			outX[k] = xs[count - 1];
			outY[k] = ys[count - 1];
			outZ[k] = zs[count - 1];
			continue;
		}
		// Advance to the segment that contains 'want' (the targets only increase).
		float segLen = 0.0f;
		for (;;)
		{
			const float dx = xs[seg] - xs[seg - 1];
			const float dy = ys[seg] - ys[seg - 1];
			segLen = (float)sqrt((double)(dx * dx + dy * dy));
			if (want <= segStart + segLen || seg == count - 1)
				break;
			segStart += segLen;
			++seg;
		}
		float t = segLen > 0.0f ? (want - segStart) / segLen : 1.0f;
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
		outX[k] = xs[seg - 1] + (xs[seg] - xs[seg - 1]) * t;
		outY[k] = ys[seg - 1] + (ys[seg] - ys[seg - 1]) * t;
		outZ[k] = zs[seg - 1] + (zs[seg] - zs[seg - 1]) * t;
	}
	return true;
}

//-------------------------------------------------------------------------------------------------
// Menu focus: which rectangle is next in a direction. Rectangles are lo.x, lo.y, hi.x, hi.y
// in screen pixels (y down). dir: 0 up, 1 right, 2 down, 3 left.
//-------------------------------------------------------------------------------------------------

/// How far apart two ranges on one axis are: 0 when they really overlap (one row or column),
/// else the distance between their middles. Ranges that only touch (a row ending where the next
/// starts) are not the same row.
inline int crossDistance(int lo1, int hi1, int lo2, int hi2)
{
	const int overlap = ((hi1 < hi2) ? hi1 : hi2) - ((lo1 > lo2) ? lo1 : lo2);
	if (overlap > 4)
		return 0;
	const int d = (lo1 + hi1) / 2 - (lo2 + hi2) / 2;
	return d < 0 ? -d : d;
}

/// The index of the nearest rectangle ahead of rects[from] in the direction, or -1. Mostly ahead
/// and preferably in the same row or column; equal scores go to the one whose middle is closest.
/// Rectangles in the same row or column, or whose middle lies within about 60 degrees of the
/// direction, come first; only when there are none is anything further off considered (so Right
/// from a button goes to the button beside it, not to one mostly above it).
inline int findNeighbourIn(const int (*rects)[4], int count, int from, int dir, bool coneOnly);
inline int findNeighbour(const int (*rects)[4], int count, int from, int dir)
{
	const int inCone = findNeighbourIn(rects, count, from, dir, true);
	return inCone >= 0 ? inCone : findNeighbourIn(rects, count, from, dir, false);
}

inline int findNeighbourIn(const int (*rects)[4], int count, int from, int dir, bool coneOnly)
{
	const int *a = rects[from];
	const int ax = (a[0] + a[2]) / 2;
	const int ay = (a[1] + a[3]) / 2;
	const bool horizontal = dir == 1 || dir == 3;

	int best = -1;
	int bestScore = 0;
	int bestOffset = 0;
	for (int i = 0; i < count; ++i)
	{
		if (i == from)
			continue;
		const int *b = rects[i];
		const int bx = (b[0] + b[2]) / 2;
		const int by = (b[1] + b[3]) / 2;

		int ahead;
		if (dir == 1)
			ahead = bx - ax;
		else if (dir == 3)
			ahead = ax - bx;
		else if (dir == 2)
			ahead = by - ay;
		else
			ahead = ay - by;
		if (ahead <= 0)
			continue;

		const int side = horizontal ? crossDistance(a[1], a[3], b[1], b[3]) : crossDistance(a[0], a[2], b[0], b[2]);
		int offset = horizontal ? by - ay : bx - ax;
		if (offset < 0)
			offset = -offset;
		if (coneOnly && side > 0 && offset * 4 > ahead * 7)   // tan(60) is about 1.73
			continue;
		const int score = ahead + side * 3;
		if (best < 0 || score < bestScore || (score == bestScore && offset < bestOffset))
		{
			best = i;
			bestScore = score;
			bestOffset = offset;
		}
	}
	return best;
}

//-------------------------------------------------------------------------------------------------
// Button layout: the face buttons and bumpers can be swapped around. They are the six lowest
// button bits (A, B, X, Y, LB, RB). map[f] is the physical button that does function f. A layout
// is always a permutation, so every function stays on exactly one button.
//-------------------------------------------------------------------------------------------------
const int NUM_REMAPPABLE_BUTTONS = 6;
const unsigned REMAPPABLE_BUTTON_BITS = 0x3Fu;

inline void setDefaultButtonMap(int *map)
{
	for (int f = 0; f < NUM_REMAPPABLE_BUTTONS; ++f)
		map[f] = f;
}

/// Each physical button used exactly once.
inline bool isValidButtonMap(const int *map)
{
	unsigned seen = 0;
	for (int f = 0; f < NUM_REMAPPABLE_BUTTONS; ++f)
	{
		if (map[f] < 0 || map[f] >= NUM_REMAPPABLE_BUTTONS || (seen & (1u << map[f])))
			return false;
		seen |= 1u << map[f];
	}
	return true;
}

inline bool isDefaultButtonMap(const int *map)
{
	for (int f = 0; f < NUM_REMAPPABLE_BUTTONS; ++f)
	{
		if (map[f] != f)
			return false;
	}
	return true;
}

/// Physical buttons -> the functions they do. Other bits pass through.
inline unsigned remapButtons(unsigned physical, const int *map)
{
	unsigned logical = physical & ~REMAPPABLE_BUTTON_BITS;
	for (int f = 0; f < NUM_REMAPPABLE_BUTTONS; ++f)
	{
		if (physical & (1u << map[f]))
			logical |= 1u << f;
	}
	return logical;
}

/// Put function f on a physical button. The function that had that button takes f's old one
/// (a swap), so nothing is ever left without a button. Returns the function that moved, or -1.
inline int assignButton(int *map, int function, int physical)
{
	if (function < 0 || function >= NUM_REMAPPABLE_BUTTONS || physical < 0 || physical >= NUM_REMAPPABLE_BUTTONS)
		return -1;
	if (map[function] == physical)
		return -1;
	for (int g = 0; g < NUM_REMAPPABLE_BUTTONS; ++g)
	{
		if (g != function && map[g] == physical)
		{
			map[g] = map[function];
			map[function] = physical;
			return g;
		}
	}
	map[function] = physical;
	return -1;
}

/// A button hold that fires once when it has lasted holdMs, and not again until it is released
/// (the emergency reset). Times are timeGetTime() milliseconds; the elapsed time is unsigned, so
/// the timer wrapping around does not matter.
struct HoldLatch
{
	bool holding;
	bool fired;
	unsigned startMs;
};

inline void resetHoldLatch(HoldLatch *latch)
{
	latch->holding = false;
	latch->fired = false;
	latch->startMs = 0;
}

/// Returns true on the one update where the hold reaches holdMs.
inline bool updateHoldLatch(HoldLatch *latch, bool held, unsigned nowMs, unsigned holdMs)
{
	if (!held)
	{
		resetHoldLatch(latch);
		return false;
	}
	if (!latch->holding)
	{
		latch->holding = true;
		latch->fired = false;
		latch->startMs = nowMs;
	}
	if (latch->fired || nowMs - latch->startMs < holdMs)
		return false;
	latch->fired = true;
	return true;
}

/// RT's type cycle, All -> each type -> All. shown: 0 = All is selected, 1..typeCount = that type
/// is, -1 = the type that was selected is gone (all of it died or left). everyoneSelected: the
/// selection is already every surviving member. Returns the next step (0 = All, k = type k), or
/// -1 when there is nothing to change (a vanished type goes back to All survivors).
inline int nextTypeCycleIndex(int shown, int typeCount, bool everyoneSelected)
{
	if (typeCount <= 0)
		return -1;
	if (shown < 0)
		return 0;
	if (typeCount < 2)
		return everyoneSelected ? -1 : 0;
	return (shown + 1) % (typeCount + 1);
}

/// Animated reticle: how far the frame still stands out after the hover candidate changed, 1 = the
/// full spread, 0 = settled. It glides in, easing out (fast first, slow at the end), and is settled
/// after durationMs (elapsed from timeGetTime(), unsigned, so frame rate and timer wrap do not matter).
inline float reticleAcquireSpread(unsigned elapsedMs, unsigned durationMs)
{
	if (durationMs == 0 || elapsedMs >= durationMs)
		return 0.0f;
	const float left = 1.0f - (float)elapsedMs / (float)durationMs;
	return left * left;
}

/// Animated reticle: the idle accent tick's brightness, 0..1..0 over periodMs.
inline float reticlePulse(unsigned nowMs, unsigned periodMs)
{
	if (periodMs == 0)
		return 0.0f;
	const float phase = (float)(nowMs % periodMs) / (float)periodMs;
	return phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f;
}

/// Animated reticle: a new candidate animates, unless the last animation started less than
/// minGapMs ago (the reticle sweeping across a crowd must not flash continuously).
inline bool reticleShouldAnimate(bool animatedBefore, unsigned sinceLastAnimMs, unsigned minGapMs)
{
	return !animatedBefore || sinceLastAnimMs >= minGapMs;
}

} // namespace ControllerMath
