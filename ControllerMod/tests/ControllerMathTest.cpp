// ControllerMathTest.cpp
// Standalone checks for the pure controller input math (ControllerMath.h).
// Build and run with ControllerMod\scripts\test.cmd. Exit code 0 means all checks passed.

#include "../../GeneralsMD/Code/GameEngine/Include/GameClient/ControllerMath.h"

#include <math.h>
#include <stdio.h>

using namespace ControllerMath;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool condition, const char *what)
{
	++g_checks;
	if (!condition)
	{
		++g_failures;
		printf("FAIL: %s\n", what);
	}
}

static bool near(float a, float b, float eps = 1e-4f)
{
	return fabs(a - b) <= eps;
}

static float len(float x, float y)
{
	return (float)sqrt((double)(x * x + y * y));
}

static void testNormalizeAxis()
{
	check(near(normalizeAxis(0), 0.0f), "axis: 0 -> 0");
	check(near(normalizeAxis(32767), 1.0f), "axis: +32767 -> +1 exactly");
	check(near(normalizeAxis(-32768), -1.0f), "axis: -32768 -> -1 exactly");
	check(normalizeAxis(-1) < 0.0f && normalizeAxis(-1) > -0.001f, "axis: -1 is tiny negative");
	check(near(normalizeAxis(16384), 16384.0f / 32767.0f), "axis: positive half");
	check(near(normalizeAxis(-16384), -0.5f), "axis: negative half");
	check(near(normalizeTrigger(0), 0.0f) && near(normalizeTrigger(255), 1.0f), "trigger: 0..255 -> 0..1");
}

static void testDeadzone()
{
	float x, y;

	processStick(0.0f, 0.0f, 0.18f, 0.98f, 1.6f, &x, &y);
	check(x == 0.0f && y == 0.0f, "deadzone: zero stays zero (no divide by zero)");

	processStick(0.17f, 0.0f, 0.18f, 0.98f, 1.6f, &x, &y);
	check(x == 0.0f && y == 0.0f, "deadzone: inside inner deadzone is zero");

	// Radial, not per-axis: a diagonal whose components are each inside the deadzone but whose
	// length is outside must produce output.
	processStick(0.15f, 0.15f, 0.18f, 0.98f, 1.0f, &x, &y);
	check(x > 0.0f && y > 0.0f, "deadzone: radial, diagonal 0.15/0.15 is outside 0.18");

	processStick(0.18f + 1e-4f, 0.0f, 0.18f, 0.98f, 1.6f, &x, &y);
	check(x >= 0.0f && x < 0.001f, "deadzone: output is continuous at the inner edge");

	processStick(1.0f, 0.0f, 0.18f, 0.98f, 1.6f, &x, &y);
	check(near(x, 1.0f) && near(y, 0.0f), "saturation: full push -> 1");

	processStick(0.98f, 0.0f, 0.18f, 0.98f, 1.6f, &x, &y);
	check(near(x, 1.0f), "saturation: outer limit already gives full output");

	processStick(-1.0f, 0.0f, 0.18f, 0.98f, 1.6f, &x, &y);
	check(near(x, -1.0f), "saturation: negative extreme -> -1");
}

static void testDiagonalsNotFaster()
{
	float x, y;
	processStick(1.0f, 1.0f, 0.18f, 0.98f, 1.6f, &x, &y);
	check(near(len(x, y), 1.0f), "diagonal: full diagonal has length 1, not sqrt(2)");
	check(near(x, y), "diagonal: direction preserved");

	float sx, sy;
	processStick(0.6f, 0.0f, 0.18f, 0.98f, 1.6f, &sx, &sy);
	processStick(0.6f / 1.41421356f, 0.6f / 1.41421356f, 0.18f, 0.98f, 1.6f, &x, &y);
	check(near(len(x, y), len(sx, sy)), "diagonal: same deflection -> same speed as straight");
}

static void testResponseCurve()
{
	float linearX, curvedX, y;
	processStick(0.58f, 0.0f, 0.18f, 0.98f, 1.0f, &linearX, &y);
	processStick(0.58f, 0.0f, 0.18f, 0.98f, 1.6f, &curvedX, &y);
	check(near(linearX, 0.5f), "curve: exponent 1 is linear between inner and outer");
	check(curvedX < linearX, "curve: exponent 1.6 gives more precision near centre");

	float prev = 0.0f;
	bool monotonic = true;
	for (int i = 0; i <= 100; ++i)
	{
		processStick(i / 100.0f, 0.0f, 0.18f, 0.98f, 1.6f, &curvedX, &y);
		if (curvedX < prev - 1e-6f)
			monotonic = false;
		prev = curvedX;
	}
	check(monotonic, "curve: output never decreases as the stick is pushed further");
}

static void testInvalidSettings()
{
	float x, y;
	processStick(0.5f, 0.0f, 0.5f, 0.3f, 1.6f, &x, &y);  // outer below inner
	check(x == x && y == y, "invalid: outer<inner gives a number, not NaN");
	processStick(0.9f, 0.0f, 0.2f, 0.98f, 0.0f, &x, &y);  // zero exponent
	check(x > 0.0f && x <= 1.0f, "invalid: exponent 0 falls back to linear");
	processStick(0.5f, 0.0f, 2.0f, 3.0f, 1.0f, &x, &y);   // absurd deadzone
	check(x == x && x >= 0.0f && x <= 1.0f, "invalid: absurd deadzone stays in range");
}

static void testTriggers()
{
	check(processTrigger(0.05f, 0.1f) == 0.0f, "trigger: below deadzone is 0");
	check(near(processTrigger(1.0f, 0.1f), 1.0f), "trigger: full pull is 1");
	check(near(processTrigger(0.55f, 0.1f), 0.5f), "trigger: rescaled linearly");

	bool pressed = false;
	pressed = updateTriggerButton(pressed, 0.50f, 0.55f, 0.35f);
	check(!pressed, "hysteresis: 0.50 does not press");
	pressed = updateTriggerButton(pressed, 0.56f, 0.55f, 0.35f);
	check(pressed, "hysteresis: 0.56 presses");
	pressed = updateTriggerButton(pressed, 0.40f, 0.55f, 0.35f);
	check(pressed, "hysteresis: dropping to 0.40 stays pressed");
	pressed = updateTriggerButton(pressed, 0.30f, 0.55f, 0.35f);
	check(!pressed, "hysteresis: below 0.35 releases");
}

static void testSmoothing()
{
	check(smoothingAlpha(0.016f, 0.0f) == 1.0f, "smoothing: 0 ms means no smoothing");
	const float a60 = smoothingAlpha(1.0f / 60.0f, 0.035f);
	const float a30 = smoothingAlpha(1.0f / 30.0f, 0.035f);
	check(a60 > 0.0f && a60 < 1.0f, "smoothing: alpha in (0,1)");
	// Two 60 Hz steps must equal one 30 Hz step, i.e. frame rate independent.
	const float twoSteps = 1.0f - (1.0f - a60) * (1.0f - a60);
	check(near(twoSteps, a30, 1e-5f), "smoothing: frame rate independent");
}

static void testConsumeTowardZero()
{
	float v = 10.0f;
	check(near(consumeTowardZero(&v, 4.0f), 4.0f) && near(v, 6.0f), "consume: partial");
	check(near(consumeTowardZero(&v, 20.0f), 6.0f) && v == 0.0f, "consume: stops at zero");
	v = -5.0f;
	check(near(consumeTowardZero(&v, 2.0f), 2.0f) && near(v, -3.0f), "consume: negative side");
	check(consumeTowardZero(&v, -1.0f) == 0.0f && near(v, -3.0f), "consume: negative amount ignored");
}

// Camera looking north (unrotated): one screen pixel right = +0.5 world X, one pixel down = -0.5 world Y.
// The camera pivot is clipped to [0,1000] x [0,1000].
struct EdgeCase
{
	float reqX, reqY, offX, offY;
	void run(float posX, float posY, bool hasArea = true, float handBack = 8.0f,
		float rx = 0.5f, float ry = 0.0f, float dx = 0.0f, float dy = -0.5f)
	{
		resolveEdgeOffset(&reqX, &reqY, &offX, &offY, posX, posY, rx, ry, dx, dy,
			hasArea, 0.0f, 0.0f, 1000.0f, 1000.0f, handBack);
	}
};

static void testEdgeOffset()
{
	EdgeCase c;

	c.reqX = 10; c.reqY = 0; c.offX = 0; c.offY = 0;
	c.run(500, 500);
	check(near(c.reqX, 10) && c.offX == 0 && c.offY == 0, "edge: free pan is untouched");

	c.reqX = 10; c.reqY = 0; c.offX = 0; c.offY = 0;
	c.run(1000, 500);
	check(near(c.reqX, 0) && near(c.offX, 10), "edge: push into east border moves the reticle instead");

	c.reqX = 10; c.reqY = 0; c.offX = 0; c.offY = 0;
	c.run(998, 500);   // 2 world units = 4 px of room left
	check(near(c.reqX, 4) && near(c.offX, 6), "edge: partial room is used, the rest becomes offset");

	c.reqX = 0; c.reqY = -10; c.offX = 0; c.offY = 0;
	c.run(500, 1000);  // screen up = north = +Y world, blocked at the north border
	check(near(c.reqY, 0) && near(c.offY, -10), "edge: push into north border offsets upward");

	c.reqX = 10; c.reqY = -10; c.offX = 0; c.offY = 0;
	c.run(1000, 500);
	check(near(c.offX, 10) && near(c.reqX, 0) && near(c.reqY, -10) && c.offY == 0,
		"edge: diagonal into east border only offsets the blocked axis");

	c.reqX = -8; c.reqY = 0; c.offX = 20; c.offY = 0;
	c.run(1000, 500);
	check(near(c.offX, 12) && near(c.reqX, 0), "edge: reversing walks the reticle back first");

	c.reqX = -8; c.reqY = 0; c.offX = 5; c.offY = 0;
	c.run(1000, 500);
	check(c.offX == 0 && near(c.reqX, -3), "edge: reversal past the centre pans the camera with the rest");

	c.reqX = 0; c.reqY = 0; c.offX = 20; c.offY = 0;
	c.run(1000, 500);
	check(near(c.offX, 20) && near(c.reqX, 0), "edge: offset is kept while the camera is still at the border");

	c.reqX = 0; c.reqY = 0; c.offX = 20; c.offY = 0;
	c.run(900, 500);
	check(near(c.offX, 12) && near(c.reqX, 8), "edge: offset no longer needed is handed back at pan speed");

	c.reqX = 0; c.reqY = 0; c.offX = 20; c.offY = 0;
	c.run(999, 500);   // 1 world unit = 2 px of room
	check(near(c.offX, 18) && near(c.reqX, 2), "edge: hand-back stops at the border");

	// The review's false-offset case: far from any border, nothing can create an offset,
	// however the camera got there.
	c.reqX = -10; c.reqY = 0; c.offX = 0; c.offY = 0;
	c.run(500, 500);
	check(c.offX == 0 && near(c.reqX, -10), "edge: no offset away from the border (camera jumps cannot fake one)");

	c.reqX = 10; c.reqY = 0; c.offX = 0; c.offY = 0;
	c.run(1000, 500, false);
	check(c.offX == 0 && near(c.reqX, 10), "edge: unknown constraint area never creates an offset");

	c.reqX = -8; c.reqY = 0; c.offX = 20; c.offY = 0;
	c.run(1000, 500, false);
	check(near(c.offX, 12) && near(c.reqX, 0), "edge: reversal still works when the area is unknown");

	// Rotated 90 degrees: screen right = +Y world, screen down = +X world.
	c.reqX = 10; c.reqY = 0; c.offX = 0; c.offY = 0;
	c.run(500, 1000, true, 8.0f, 0.0f, 0.5f, 0.5f, 0.0f);
	check(near(c.offX, 10) && near(c.reqX, 0), "edge: rotated camera, screen right into the north border");

	// Camera outside the area (the area shrank after a zoom): only our own push counts.
	c.reqX = 0; c.reqY = 0; c.offX = 0; c.offY = 0;
	c.run(1010, 500);
	check(c.offX == 0 && c.reqX == 0, "edge: being outside the area by itself creates no offset");
}

static void testSpacePointsAlongPath()
{
	float ox[8], oy[8], oz[8];

	// Straight line: evenly spaced including both ends.
	const float sx[2] = { 0, 100 }, sy[2] = { 0, 0 }, sz[2] = { 0, 10 };
	check(spacePointsAlongPath(sx, sy, sz, 2, 5, ox, oy, oz), "line: straight path accepted");
	check(near(ox[0], 0) && near(ox[1], 25) && near(ox[2], 50) && near(ox[3], 75) && near(ox[4], 100), "line: straight path spacing");
	check(near(oz[2], 5), "line: height interpolated");

	// L-shaped path: the middle point of three lands exactly on the corner.
	const float lx[3] = { 0, 100, 100 }, ly[3] = { 0, 0, 100 }, lz[3] = { 0, 0, 0 };
	spacePointsAlongPath(lx, ly, lz, 3, 3, ox, oy, oz);
	check(near(ox[0], 0) && near(oy[0], 0) && near(ox[1], 100) && near(oy[1], 0) && near(ox[2], 100) && near(oy[2], 100), "line: corner of an L path");

	// Many samples along a line: spacing is by distance, not by sample.
	const float mx[4] = { 0, 10, 20, 100 }, my[4] = { 0, 0, 0, 0 }, mz[4] = { 0, 0, 0, 0 };
	spacePointsAlongPath(mx, my, mz, 4, 3, ox, oy, oz);
	check(near(ox[1], 50), "line: uneven samples still space evenly");
	check(near(pathLength(mx, my, 4), 100), "line: path length");

	// Degenerate paths.
	const float px[1] = { 7 }, py[1] = { 8 }, pz[1] = { 9 };
	spacePointsAlongPath(px, py, pz, 1, 3, ox, oy, oz);
	check(near(ox[0], 7) && near(ox[2], 7) && near(oy[1], 8), "line: single point path");
	spacePointsAlongPath(sx, sy, sz, 2, 1, ox, oy, oz);
	check(near(ox[0], 100), "line: one unit goes to the end");
	check(!spacePointsAlongPath(sx, sy, sz, 0, 3, ox, oy, oz), "line: empty path refused");
}

static void testMenuNeighbour()
{
	// The skirmish player grid: rows touch (one ends at y=204, the next starts there).
	// 0 Blue, 1 Army0 (row 1); 2 Red, 3 Army1 (row 2); 4 a button far below.
	const int grid[5][4] =
	{
		{ 392, 168, 560, 204 }, { 566, 168, 982, 204 },
		{ 392, 204, 560, 240 }, { 566, 204, 982, 240 },
		{ 188, 769, 536, 823 }
	};
	check(findNeighbour(grid, 5, 3, 3) == 2, "menu: left stays in a touching row");
	check(findNeighbour(grid, 5, 1, 2) == 3, "menu: down goes to the row below");
	check(findNeighbour(grid, 5, 2, 0) == 0, "menu: up goes to the row above");
	check(findNeighbour(grid, 4, 0, 3) == -1, "menu: nothing to the left");
	check(findNeighbour(grid, 5, 2, 2) == 4, "menu: down reaches the far button");
	check(crossDistance(168, 204, 204, 240) == 36, "menu: touching rows are not one row");
	check(crossDistance(168, 204, 170, 206) == 0, "menu: overlapping rows are one row");

	// A column of main menu buttons: down steps one at a time.
	const int column[3][4] = { { 1090, 205, 1500, 255 }, { 1090, 265, 1500, 315 }, { 1090, 325, 1500, 375 } };
	check(findNeighbour(column, 3, 0, 2) == 1 && findNeighbour(column, 3, 1, 2) == 2, "menu: column steps");
	check(findNeighbour(column, 3, 2, 2) == -1, "menu: no wrap at the bottom");

	// Skirmish bottom: Right from Play Game goes to Main Menu, not to the Game Speed slider that
	// is a little to the right but mostly above.
	const int bottom[3][4] = { { 188, 769, 536, 823 }, { 1060, 769, 1402, 823 }, { 352, 516, 532, 556 } };
	check(findNeighbour(bottom, 3, 0, 1) == 1, "menu: right prefers the same row");
	check(findNeighbour(bottom, 3, 0, 0) == 2, "menu: up still reaches the slider");
	// Nothing in the cone: the nearest one further off is still reachable.
	const int lonely[2][4] = { { 0, 0, 100, 40 }, { 120, 400, 220, 440 } };
	check(findNeighbour(lonely, 2, 0, 1) == 1, "menu: falls back outside the cone");
}

static void testButtonMap()
{
	int map[NUM_REMAPPABLE_BUTTONS];
	setDefaultButtonMap(map);
	check(isValidButtonMap(map) && isDefaultButtonMap(map), "buttons: default layout");
	check(remapButtons(0x1 | 0x400, map) == (0x1 | 0x400), "buttons: default passes through");

	// Swap A (0) and B (1): pressing physical B now does A.
	check(assignButton(map, 0, 1) == 1, "buttons: A onto B moves B");
	check(map[0] == 1 && map[1] == 0 && isValidButtonMap(map), "buttons: swapped");
	check(remapButtons(0x2, map) == 0x1, "buttons: physical B does A");
	check(remapButtons(0x1, map) == 0x2, "buttons: physical A does B");
	check(remapButtons(0x3 | 0x100, map) == (0x3 | 0x100), "buttons: both held, others kept");
	check(!isDefaultButtonMap(map), "buttons: not default any more");

	// Assigning a button a function already has changes nothing.
	check(assignButton(map, 0, 1) == -1 && map[0] == 1, "buttons: same button no change");
	check(assignButton(map, 7, 1) == -1 && assignButton(map, 0, 9) == -1, "buttons: out of range refused");

	// Invalid layouts are recognised.
	int bad[NUM_REMAPPABLE_BUTTONS] = { 0, 0, 2, 3, 4, 5 };
	check(!isValidButtonMap(bad), "buttons: duplicate refused");
	int bad2[NUM_REMAPPABLE_BUTTONS] = { 0, 1, 2, 3, 4, 6 };
	check(!isValidButtonMap(bad2), "buttons: out of range refused in map");
}

// Review F11: the emergency reset fires once per hold, again after a release, across a wrap.
static void testHoldLatch()
{
	HoldLatch latch;
	resetHoldLatch(&latch);
	int fired = 0;
	for (unsigned t = 100000; t <= 106000; t += 16)   // six seconds held, 3 s threshold
		fired += updateHoldLatch(&latch, true, t, 3000) ? 1 : 0;
	check(fired == 1, "hold latch: one reset for a six-second hold");
	check(!updateHoldLatch(&latch, true, 103016, 3000), "hold latch: no second reset at 103016 ms (old sentinel bug)");
	updateHoldLatch(&latch, false, 106100, 3000);
	fired = 0;
	for (unsigned t = 106200; t <= 112000; t += 16)
		fired += updateHoldLatch(&latch, true, t, 3000) ? 1 : 0;
	check(fired == 1, "hold latch: one more after release and hold again");
	updateHoldLatch(&latch, false, 0, 3000);
	check(!updateHoldLatch(&latch, true, 0xFFFFFC00u, 3000), "hold latch: not at the start of a hold 1 s before the wrap");
	check(!updateHoldLatch(&latch, true, 0x00000700u, 3000), "hold latch: not at 2.8 s, across the wrap");
	check(updateHoldLatch(&latch, true, 0x00000800u, 3000), "hold latch: fires at 3.07 s, across the timer wrap");
	check(!updateHoldLatch(&latch, true, 0x00001000u, 3000), "hold latch: once across the wrap");
	check(!updateHoldLatch(&latch, false, 0x00001100u, 3000) && !latch.holding, "hold latch: release clears it");
	check(!updateHoldLatch(&latch, true, 0x00001200u, 3000), "hold latch: a short hold does nothing");
}

// Review F06: RT's cycle after members vanish.
static void testTypeCycle()
{
	check(nextTypeCycleIndex(0, 2, true) == 1, "type cycle: All -> first type");
	check(nextTypeCycleIndex(1, 2, false) == 2, "type cycle: first -> second");
	check(nextTypeCycleIndex(2, 2, false) == 0, "type cycle: last -> All");
	check(nextTypeCycleIndex(-1, 1, false) == 0, "type cycle: the infantry all died -> All survivors (the tank)");
	check(nextTypeCycleIndex(-1, 3, false) == 0, "type cycle: a vanished type -> All");
	check(nextTypeCycleIndex(1, 1, false) == 0, "type cycle: one type left but not all selected -> All");
	check(nextTypeCycleIndex(0, 1, true) == -1, "type cycle: one type, everyone selected -> nothing to do");
	check(nextTypeCycleIndex(0, 0, true) == -1, "type cycle: nobody left -> nothing to do");
}

// Animated reticle timing.
static void testReticleTiming()
{
	check(reticleAcquireSpread(0, 240) == 1.0f, "reticle: full spread at the start");
	check(near(reticleAcquireSpread(120, 240), 0.25f), "reticle: eases out (a quarter left half-way)");
	check(reticleAcquireSpread(60, 240) > reticleAcquireSpread(120, 240) && reticleAcquireSpread(120, 240) > reticleAcquireSpread(180, 240), "reticle: glides in steadily");
	check(reticleAcquireSpread(240, 240) == 0.0f && reticleAcquireSpread(100000, 240) == 0.0f, "reticle: settled after the duration");
	check(reticleAcquireSpread(0xFFFFFFFFu, 240) == 0.0f && reticleAcquireSpread(5, 0) == 0.0f, "reticle: huge elapsed or zero duration is settled");
	check(reticlePulse(0, 2400) == 0.0f && near(reticlePulse(1200, 2400), 1.0f) && near(reticlePulse(600, 2400), 0.5f), "reticle: pulse 0 -> 1 -> 0 over 2.4 s");
	check(near(reticlePulse(2400 + 600, 2400), 0.5f) && reticlePulse(5, 0) == 0.0f, "reticle: pulse repeats; zero period is off");
	check(reticleShouldAnimate(false, 0, 250), "reticle: the first candidate animates");
	check(!reticleShouldAnimate(true, 100, 250), "reticle: rapid candidate changes do not re-animate");
	check(reticleShouldAnimate(true, 250, 250), "reticle: animates again after the gap");
	check(reticleShouldAnimate(true, 0x80000000u, 250), "reticle: a wrapped timer does not block it");
}

static void testKeyboardMove()
{
	const int rows[6] = { 10, 10, 10, 10, 10, 4 };
	int r = 0, c = 0;
	keyboardMove(rows, 6, &r, &c, 2);
	check(r == 0 && c == 9, "keyboard: left from the first key wraps to the row's end");
	keyboardMove(rows, 6, &r, &c, 3);
	check(r == 0 && c == 0, "keyboard: right from the row's end wraps to its start");
	r = 4; c = 9;
	keyboardMove(rows, 6, &r, &c, 1);
	check(r == 5 && c == 3, "keyboard: down from the right end lands on the last wide key");
	keyboardMove(rows, 6, &r, &c, 1);
	check(r == 0 && c == 8, "keyboard: down from the last row wraps to the top, same side");
	r = 5; c = 0;
	keyboardMove(rows, 6, &r, &c, 0);
	check(r == 4 && c == 1, "keyboard: up from a wide key lands under it");
	r = 2; c = 5;
	keyboardMove(rows, 6, &r, &c, 1);
	check(r == 3 && c == 5, "keyboard: rows of the same length keep the column");
	const int pad[5] = { 3, 3, 3, 3, 1 };
	r = 3; c = 2;
	keyboardMove(pad, 5, &r, &c, 1);
	check(r == 4 && c == 0, "keyboard: number pad, down to the single Done key");
	keyboardMove(pad, 5, &r, &c, 0);
	check(r == 3 && c == 1, "keyboard: number pad, up from Done to the middle");
	r = 7; c = 3;
	keyboardMove(pad, 5, &r, &c, 1);
	check(r == 0 && c == 0, "keyboard: an invalid position resets to the first key");
}

int main()
{
	testNormalizeAxis();
	testDeadzone();
	testDiagonalsNotFaster();
	testResponseCurve();
	testInvalidSettings();
	testTriggers();
	testSmoothing();
	testConsumeTowardZero();
	testEdgeOffset();
	testSpacePointsAlongPath();
	testMenuNeighbour();
	testButtonMap();
	testHoldLatch();
	testTypeCycle();
	testReticleTiming();
	testKeyboardMove();

	printf("%d checks, %d failed\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
