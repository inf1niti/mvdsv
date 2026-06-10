/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

	
*/

#ifdef SERVERONLY
#include "qwsvdef.h"
#else
#include "quakedef.h"
#include "pmove.h"
#endif

movevars_t      movevars;
playermove_t    pmove;

static float	pm_frametime;

static vec3_t	pm_forward, pm_right;

static vec3_t   groundnormal;

vec3_t	player_mins = {-16, -16, -24};
vec3_t	player_maxs = {16, 16, 32};

#define STEPSIZE        18

#define pm_flyfriction	4

#define BLOCKED_FLOOR	1
#define BLOCKED_STEP	2
#define BLOCKED_OTHER	4
#define BLOCKED_ANY		7

#define MAX_JUMPFIX_DOTPRODUCT -0.1

#define HOOK_PULL_SPEED      684
#define HOOK_INIT_PULL_SPEED 360
#define HOOK_ACCEL_TIME      0.598f
#define HOOK_EPSILON         0.000001f

#define HOOK_GROUND_DETACH_SPEED          360
#define HOOK_GROUND_DETACH_MIN_UP         0.02f
#define HOOK_GROUND_MIN_LIFT_SPEED        120
#define HOOK_GROUND_MAX_LIFT_SPEED        260
#define HOOK_GROUND_FULL_LIFT_UP          0.25f
#define HOOK_GROUND_TANGENTIAL_SCALE      0.35f
#define HOOK_GROUND_FAST_TANGENTIAL_SCALE 0.95f

#define HOOK_SLACK_DELAY     0.325f
#define HOOK_SLACK_DURATION  0.85f

#define HOOK_PULL_ACCEL      4800
#define HOOK_PULL_DECEL      2400
#define HOOK_PULL_RECOVER    7200
#define HOOK_VERTICAL_PULL_BOOST 0.35f

#define HOOK_MIN_GRAVITY     0.36f
#define HOOK_MAX_GRAVITY     0.78f

#define HOOK_MIN_INERTIA     0.078f
#define HOOK_MAX_INERTIA     0.38f

#define HOOK_INPUT_TANGENTIAL_ACCEL 290
#define HOOK_INPUT_TANGENTIAL_BACK_BIAS 0.25f
#define HOOK_INPUT_BACK_PULL_SCALE 0.55f
#define HOOK_INPUT_BACK_RESIST_SCALE 0.85f
#define HOOK_INPUT_BACK_GRAVITY_FACTOR 0.65f
#define HOOK_INPUT_REEL_RADIAL_BOOST 0.42f
#define HOOK_INPUT_REEL_TANGENTIAL_SCALE 0.95f
#define HOOK_INPUT_REEL_TANGENTIAL_DAMPING 0.99f
#define HOOK_INPUT_REEL_GRAVITY_SCALE 1.0f
#define HOOK_INPUT_REEL_MAX_PULL_SCALE 1.14f
#define HOOK_INPUT_NEUTRAL_REEL_SCALE 0.48f
#define HOOK_INPUT_HOLD_REEL_DECAY 1.25f
#define HOOK_INPUT_HOLD_EASE_TIME 0.60f
#define HOOK_INPUT_EASE_OUT_TIME 0.36f
#define HOOK_INPUT_REEL_PULL_EASE_TIME 0.15f
#define HOOK_INPUT_REEL_SCALE 1.45f
#define HOOK_INPUT_HOLD_GROUND_CHECK 56
#define HOOK_INPUT_HOLD_GROUND_MIN_REEL 0.22f

#define HOOK_MIN_PULL_SCALE 0.85f
#define HOOK_MAX_PULL_SCALE 0.94f

#define HOOK_TENSION_INPUT_GAIN     320
#define HOOK_TENSION_AWAY_GAIN      0.65f
#define HOOK_TENSION_DECAY_RATE     180
#define HOOK_TENSION_RELEASE_RATE   840
#define HOOK_TENSION_MAX            0.35f

#define HOOK_RADIAL_SPEED_CAP       1.14f
#define HOOK_RADIAL_AWAY_CAP        0.85f
#define HOOK_TANGENTIAL_SPEED_CAP   1.035f
#define HOOK_TOTAL_SPEED_CAP        1.26f
#define HOOK_SPEED_PRESERVE_TIME    0.22f
#define HOOK_SPEED_PRESERVE_BUFFER  0.99f
#define HOOK_OSCILLATION_DAMPING    0.92f
#define HOOK_OSCILLATION_TANGENTIAL_DAMPING 0.985f
#define HOOK_OSCILLATION_THRESHOLD_SCALE     0.33f
#define HOOK_OSCILLATION_DAMPING_DELAY       0.18f

#define HOOK_INPUT_NONE 0
#define HOOK_INPUT_HOLD 1
#define HOOK_INPUT_REEL 2

// Add an entity to touch list, discarding duplicates
static void PM_AddTouchedEnt (int num)
{
	int i;

	if (pmove.numtouch == sizeof(pmove.touchindex)/sizeof(pmove.touchindex[0]))
		return;

	for (i = 0; i < pmove.numtouch; i++)
		if (pmove.touchindex[i] == num)
			return; // already added

	pmove.touchindex[pmove.numtouch] = num;
	pmove.numtouch++;
}

//Slide off of the impacting object
//returns the blocked flags (1 = floor, 2 = step / wall)
#define STOP_EPSILON 0.1
static void PM_ClipVelocity (vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
	float backoff, change;
	int i;

	backoff = DotProduct (in, normal) * overbounce;

	for (i = 0; i < 3; i++) {
		change = normal[i] * backoff;
		out[i] = in[i] - change;
		if (out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
			out[i] = 0;
	}
}

static float PM_HookOscillationFactor(float length, float threshold, float vRad)
{
	float x, k, b;

	x = threshold - length;
	x = x < 0 ? 0 : x;

	k = 0.186f;
	b = 1.62f * sqrt(k);

	return k * x - b * vRad;
}

static float PM_HookVectorAlignment(vec3_t vector1, vec3_t vector2)
{
	vec3_t uv_1, uv_2;
	float ln1, ln2;

	VectorCopy(vector1, uv_1);
	ln1 = VectorNormalize(uv_1);

	VectorCopy(vector2, uv_2);
	ln2 = VectorNormalize(uv_2);

	return (ln1 < HOOK_EPSILON || ln2 < HOOK_EPSILON) ? 0 : bound(-1.0f, DotProduct(uv_1, uv_2), 1.0f);
}

static void PM_HookDecomposeVelocity(vec3_t velocity, vec3_t uv_hook, vec3_t radialVel, vec3_t tangentialVel, float *radialSpeed)
{
	*radialSpeed = DotProduct(velocity, uv_hook);
	VectorScale(uv_hook, *radialSpeed, radialVel);
	VectorSubtract(velocity, radialVel, tangentialVel);
}

static float PM_HookApproach(float current, float target, float accel, float decel)
{
	float delta;
	float step;

	delta = target - current;
	step = (delta > 0) ? accel : decel;

	if (fabs(delta) <= step) {
		return target;
	}

	return current + ((delta > 0) ? step : -step);
}

static float PM_HookPreserveFactor(void)
{
	return bound(0, 1.0f - (pmove.hook_time / HOOK_SPEED_PRESERVE_TIME), 1.0f);
}

static float PM_HookPreservedCap(float normalCap, float initialSpeed, float preserveFactor)
{
	float preservedCap;

	preservedCap = initialSpeed * HOOK_SPEED_PRESERVE_BUFFER;
	if (preservedCap <= normalCap) {
		return normalCap;
	}

	return normalCap + (preservedCap - normalCap) * preserveFactor;
}

static void PM_HookSetMinimumRadialSpeed(vec3_t uv_hook, float minSpeed)
{
	vec3_t radialVel, tangentialVel;
	float radialSpeed;

	PM_HookDecomposeVelocity(pmove.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);
	if (radialSpeed >= minSpeed) {
		return;
	}

	VectorScale(uv_hook, minSpeed, radialVel);
	VectorAdd(radialVel, tangentialVel, pmove.velocity);
}

static void PM_HookDampenTangentialVelocity(vec3_t uv_hook, float scale)
{
	vec3_t radialVel, tangentialVel;
	float radialSpeed;

	PM_HookDecomposeVelocity(pmove.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);
	VectorScale(tangentialVel, scale, tangentialVel);
	VectorAdd(radialVel, tangentialVel, pmove.velocity);
}

static void PM_HookSetMinimumGroundLift(vec3_t uv_hook)
{
	float liftSpeed;

	if (uv_hook[2] <= HOOK_GROUND_DETACH_MIN_UP) {
		return;
	}

	liftSpeed = HOOK_GROUND_MAX_LIFT_SPEED * bound(0, uv_hook[2] / HOOK_GROUND_FULL_LIFT_UP, 1);
	liftSpeed = max(HOOK_GROUND_MIN_LIFT_SPEED, liftSpeed);

	if (pmove.velocity[2] < liftSpeed) {
		pmove.velocity[2] = liftSpeed;
	}
}

static void PM_HookGetPullVector(vec3_t uv_hook, qbool wasOnGround, vec3_t uv_pull)
{
	VectorCopy(uv_hook, uv_pull);

	if (wasOnGround && (uv_pull[2] <= HOOK_GROUND_DETACH_MIN_UP)) {
		uv_pull[2] = 0;
		if (VectorNormalize(uv_pull) < HOOK_EPSILON) {
			VectorCopy(uv_hook, uv_pull);
		}
	}
}

static float PM_HookTargetPullSpeed(float minPull, float maxPull)
{
	float lerpFactor;

	lerpFactor = bound(0, pmove.hook_pull_time / HOOK_ACCEL_TIME, 1);
	return minPull + lerpFactor * (maxPull - minPull);
}

static float PM_HookApproachBlend(float current, qbool active, float inTime, float outTime)
{
	float step;

	step = pm_frametime / (active ? inTime : outTime);
	if (active) {
		return min(current + step, 1.0f);
	}

	return max(current - step, 0);
}

static float PM_HookEaseBlend(float blend)
{
	blend = bound(0, blend, 1);
	return blend * blend * (3.0f - 2.0f * blend);
}

static void PM_HookUpdateInputMode(qbool holdHeld, qbool reelHeld)
{
	if (holdHeld && !pmove.hook_hold_washeld) {
		pmove.hook_input_mode = HOOK_INPUT_HOLD;
	}
	if (reelHeld && !pmove.hook_reel_washeld) {
		pmove.hook_input_mode = HOOK_INPUT_REEL;
	}

	if (pmove.hook_input_mode == HOOK_INPUT_HOLD && !holdHeld) {
		pmove.hook_input_mode = reelHeld ? HOOK_INPUT_REEL : HOOK_INPUT_NONE;
	}
	else if (pmove.hook_input_mode == HOOK_INPUT_REEL && !reelHeld) {
		pmove.hook_input_mode = holdHeld ? HOOK_INPUT_HOLD : HOOK_INPUT_NONE;
	}
	else if (pmove.hook_input_mode == HOOK_INPUT_NONE) {
		if (reelHeld) {
			pmove.hook_input_mode = HOOK_INPUT_REEL;
		}
		else if (holdHeld) {
			pmove.hook_input_mode = HOOK_INPUT_HOLD;
		}
	}

	pmove.hook_hold_washeld = holdHeld;
	pmove.hook_reel_washeld = reelHeld;
}

static void PM_HookUpdateInputBlends(qbool holdHeld, qbool reelHeld)
{
	PM_HookUpdateInputMode(holdHeld, reelHeld);
	holdHeld = pmove.hook_input_mode == HOOK_INPUT_HOLD;
	reelHeld = pmove.hook_input_mode == HOOK_INPUT_REEL;

	pmove.hook_hold_blend = PM_HookApproachBlend(pmove.hook_hold_blend, holdHeld,
			HOOK_INPUT_HOLD_EASE_TIME, HOOK_INPUT_EASE_OUT_TIME);
	pmove.hook_reel_blend = PM_HookApproachBlend(pmove.hook_reel_blend, reelHeld,
			HOOK_ACCEL_TIME, HOOK_INPUT_EASE_OUT_TIME);
	pmove.hook_reel_pull_blend = PM_HookApproachBlend(pmove.hook_reel_pull_blend, reelHeld,
			HOOK_INPUT_REEL_PULL_EASE_TIME, HOOK_INPUT_EASE_OUT_TIME);
}

static void PM_HookUpdatePullTime(float holdBlend, float reelPullBlend)
{
	float scale;
	float holdEffect, reelPullEffect;

	holdEffect = PM_HookEaseBlend(holdBlend);
	reelPullEffect = PM_HookEaseBlend(reelPullBlend);
	scale = HOOK_INPUT_NEUTRAL_REEL_SCALE + reelPullEffect * (HOOK_INPUT_REEL_SCALE - HOOK_INPUT_NEUTRAL_REEL_SCALE);
	pmove.hook_pull_time += pm_frametime * scale * (1.0f - holdEffect);
	pmove.hook_pull_time -= pm_frametime * HOOK_INPUT_HOLD_REEL_DECAY * holdEffect;
	pmove.hook_pull_time = bound(0, pmove.hook_pull_time, HOOK_ACCEL_TIME);
}

static float PM_HookHoldReelScale(qbool wasOnGround)
{
	vec3_t end;
	trace_t trace;
	float groundScale, reelScale;

	reelScale = bound(0, pmove.hook_pull_time / HOOK_ACCEL_TIME, 1);
	if (wasOnGround) {
		return max(reelScale, HOOK_INPUT_HOLD_GROUND_MIN_REEL);
	}

	VectorCopy(pmove.origin, end);
	end[2] -= HOOK_INPUT_HOLD_GROUND_CHECK;
	trace = PM_PlayerTrace(pmove.origin, end);
	if (trace.fraction == 1 || trace.plane.normal[2] < MIN_STEP_NORMAL) {
		return reelScale;
	}

	groundScale = (1.0f - trace.fraction) * HOOK_INPUT_HOLD_GROUND_MIN_REEL;
	return max(reelScale, groundScale);
}

static float PM_HookHoldBrakeFactor(void)
{
	return bound(0, pmove.hook_time / HOOK_INPUT_HOLD_EASE_TIME, 1);
}

static float PM_HookAdjustedWishAlign(float wishAlign, float reelBlend)
{
	float reelEffect;

	if (wishAlign >= 0 || reelBlend <= 0) {
		return wishAlign;
	}

	reelEffect = PM_HookEaseBlend(reelBlend);
	return wishAlign * (1.0f - reelEffect);
}

static float PM_HookMovementInfluence(vec3_t uv_hook, vec3_t wishDir, vec3_t tangentDir)
{
	vec3_t controlDir;
	float wishAlign, tangentLen;

	VectorClear(wishDir);
	VectorClear(tangentDir);
	VectorClear(controlDir);

	VectorMA(wishDir, pmove.cmd.sidemove, pm_right, wishDir);

	if (VectorNormalize(wishDir) < HOOK_EPSILON) {
		return 0;
	}

	VectorMA(controlDir, pmove.cmd.sidemove, pm_right, controlDir);

	wishAlign = bound(-1.0f, DotProduct(wishDir, uv_hook), 1.0f);
	if (VectorNormalize(controlDir) < HOOK_EPSILON) {
		return wishAlign;
	}

	VectorMA(controlDir, -DotProduct(controlDir, uv_hook), uv_hook, tangentDir);
	tangentLen = VectorNormalize(tangentDir);

	if (tangentLen < HOOK_EPSILON) {
		VectorClear(tangentDir);
	}

	return wishAlign;
}

static void PM_HookUpdateSlack(float wishAlign, float distanceToHook)
{
	if ((wishAlign < -0.25f) && (distanceToHook > (pmove.hook_initial_length * 0.5f))) {
		pmove.hook_awaytime += pm_frametime;
	}
	else {
		pmove.hook_awaytime = 0;
	}
}

static float PM_HookSlackFactor(void)
{
	float slackFraction;

	if (pmove.hook_awaytime <= HOOK_SLACK_DELAY) {
		return 0;
	}

	slackFraction = bound(0, (pmove.hook_awaytime - HOOK_SLACK_DELAY) / HOOK_SLACK_DURATION, 1);
	return slackFraction * slackFraction * (3.0f - 2.0f * slackFraction);
}

static float PM_HookUpdateTension(float wishAlign, float radialSpeed, float maxPull)
{
	float tensionCap, inputTension, velocityTension, tensionBoost;

	tensionCap = maxPull * HOOK_TENSION_MAX;
	inputTension = 0;
	velocityTension = 0;
	tensionBoost = 0;

	if (wishAlign < -0.25f) {
		inputTension = fabs(wishAlign) * HOOK_TENSION_INPUT_GAIN * pm_frametime;
	}

	if (radialSpeed < 0) {
		velocityTension = min(-radialSpeed, maxPull) * HOOK_TENSION_AWAY_GAIN * pm_frametime;
	}

	pmove.hook_tension = min(pmove.hook_tension + inputTension + velocityTension, tensionCap);

	if ((wishAlign >= -0.1f) && (radialSpeed >= -25) && (pmove.hook_tension > 0)) {
		tensionBoost = pmove.hook_tension;
		pmove.hook_tension = max(0, pmove.hook_tension - HOOK_TENSION_RELEASE_RATE * pm_frametime);
	}
	else if (!inputTension && !velocityTension && (pmove.hook_tension > 0)) {
		pmove.hook_tension = max(0, pmove.hook_tension - HOOK_TENSION_DECAY_RATE * pm_frametime);
	}

	return tensionBoost;
}

static float PM_HookDownwardPullTarget(vec3_t uv_hook, vec3_t velocity, float targetSpeed)
{
	float fallingPull;

	if (uv_hook[2] >= -HOOK_GROUND_DETACH_MIN_UP) {
		return targetSpeed;
	}

	fallingPull = max(0, -velocity[2]) * -uv_hook[2];
	if (fallingPull > targetSpeed) {
		return fallingPull;
	}

	return targetSpeed;
}

static float PM_HookPreservedRadialPullTarget(float radialSpeed, float targetSpeed, float maxPull)
{
	float preserveFactor, preservedCap;

	if (radialSpeed <= targetSpeed) {
		return targetSpeed;
	}

	preserveFactor = PM_HookPreserveFactor();
	if (preserveFactor <= 0) {
		return targetSpeed;
	}

	preservedCap = PM_HookPreservedCap(maxPull * HOOK_RADIAL_SPEED_CAP, pmove.hook_initial_radial_speed, preserveFactor);
	if (radialSpeed <= preservedCap) {
		return radialSpeed;
	}

	return max(targetSpeed, preservedCap);
}

static void PM_HookApplyRadialPull(vec3_t uv_hook, float distanceToHook, float minPull, float maxPull,
		float wishAlign, float holdBlend, float reelBlend, float reelPullBlend, qbool wasOnGround)
{
	vec3_t radialVel, tangentialVel;
	float targetSpeed, radialSpeed, accel, slackFraction, slackScale, tensionBoost, pullWishAlign;
	float holdEffect, reelPullEffect, brakeFactor, holdScale, normalTarget;

	PM_HookDecomposeVelocity(pmove.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);
	pullWishAlign = PM_HookAdjustedWishAlign(wishAlign, reelBlend);
	pullWishAlign = (pullWishAlign < -0.15f) ? pullWishAlign * HOOK_INPUT_BACK_RESIST_SCALE : pullWishAlign;
	holdEffect = PM_HookEaseBlend(holdBlend);
	reelPullEffect = PM_HookEaseBlend(reelPullBlend);
	PM_HookUpdatePullTime(holdBlend, reelPullBlend);

	targetSpeed = PM_HookTargetPullSpeed(minPull, maxPull);
	if (holdEffect > 0) {
		brakeFactor = PM_HookHoldBrakeFactor() * holdEffect;
		holdScale = PM_HookHoldReelScale(wasOnGround);
		targetSpeed *= 1.0f - brakeFactor * (1.0f - holdScale);
	}
	targetSpeed += (1.0f - holdEffect) * bound(0, uv_hook[2], 1) * HOOK_VERTICAL_PULL_BOOST * (maxPull - targetSpeed);

	if (reelPullEffect > 0) {
		targetSpeed += reelPullEffect * HOOK_INPUT_REEL_RADIAL_BOOST * (maxPull - targetSpeed);
	}

	if (pullWishAlign < -0.15f) {
		targetSpeed *= 1.0f + ((1.0f - holdEffect) * pullWishAlign * HOOK_INPUT_BACK_PULL_SCALE);
	}

	PM_HookUpdateSlack(pullWishAlign, distanceToHook);
	tensionBoost = PM_HookUpdateTension(pullWishAlign, radialSpeed, maxPull);
	slackFraction = PM_HookSlackFactor();
	if (slackFraction > 0) {
		slackScale = HOOK_MIN_INERTIA + fabs(pullWishAlign) * (HOOK_MAX_INERTIA - HOOK_MIN_INERTIA);
		targetSpeed *= 1.0f - (slackFraction * slackScale);
	}

	targetSpeed += tensionBoost;
	if (holdEffect < 1.0f) {
		normalTarget = PM_HookDownwardPullTarget(uv_hook, pmove.velocity, targetSpeed);
		normalTarget = bound(minPull * 0.25f, normalTarget, maxPull * HOOK_RADIAL_SPEED_CAP);
		normalTarget = PM_HookPreservedRadialPullTarget(radialSpeed, normalTarget, maxPull);
		targetSpeed = targetSpeed * holdEffect + normalTarget * (1.0f - holdEffect);
	}
	accel = ((radialSpeed < 0) && (targetSpeed > radialSpeed)) ? HOOK_PULL_RECOVER : HOOK_PULL_ACCEL;
	radialSpeed = PM_HookApproach(radialSpeed, targetSpeed, accel * pm_frametime, HOOK_PULL_DECEL * pm_frametime);

	VectorScale(uv_hook, radialSpeed, radialVel);
	VectorAdd(radialVel, tangentialVel, pmove.velocity);
	pmove.hook_time = min(pmove.hook_time + pm_frametime, HOOK_ACCEL_TIME);
}

static void PM_HookApplyInputControl(vec3_t tangentDir, float wishAlign, float reelBlend)
{
	float accel, reelEffect, suppressEffect;

	if (VectorLength(tangentDir) < HOOK_EPSILON) {
		return;
	}

	if (wishAlign < -0.15f) {
		return;
	}

	reelEffect = PM_HookEaseBlend(reelBlend);
	suppressEffect = reelEffect * reelEffect;
	accel = HOOK_INPUT_TANGENTIAL_ACCEL;
	if (wishAlign < 0) {
		accel *= 1.0f + fabs(wishAlign) * HOOK_INPUT_TANGENTIAL_BACK_BIAS;
	}
	if (suppressEffect > 0) {
		accel *= 1.0f + suppressEffect * (HOOK_INPUT_REEL_TANGENTIAL_SCALE - 1.0f);
	}

	VectorMA(pmove.velocity, accel * pm_frametime, tangentDir, pmove.velocity);
}

static void PM_HookApplyGravityInfluence(vec3_t uv_hook, float maxPull, float wishAlign, float reelBlend)
{
	vec3_t transVector, uv_gravity;
	float radialSpeed, radialFactor, gravityInfluence, gravityScale, gravityTangent, reelEffect, suppressEffect;

	radialSpeed = DotProduct(pmove.velocity, uv_hook);
	radialFactor = bound(0, radialSpeed / maxPull, 1);
	wishAlign = PM_HookAdjustedWishAlign(wishAlign, reelBlend);
	if (wishAlign < -0.15f) {
		radialFactor = max(radialFactor, bound(0, fabs(wishAlign) * HOOK_INPUT_BACK_GRAVITY_FACTOR, 1));
	}

	VectorSet(uv_gravity, 0, 0, -1);
	gravityInfluence = PM_HookVectorAlignment(uv_gravity, uv_hook);
	VectorMA(uv_gravity, -gravityInfluence, uv_hook, transVector);
	gravityTangent = VectorNormalize(transVector);
	gravityScale = HOOK_MIN_GRAVITY + radialFactor * (HOOK_MAX_GRAVITY - HOOK_MIN_GRAVITY);
	reelEffect = PM_HookEaseBlend(reelBlend);
	suppressEffect = reelEffect * reelEffect;
	if (suppressEffect > 0) {
		gravityScale *= 1.0f + suppressEffect * (HOOK_INPUT_REEL_GRAVITY_SCALE - 1.0f);
	}

	if (gravityTangent > HOOK_EPSILON && radialFactor > 0.02f) {
		VectorMA(pmove.velocity, gravityScale * gravityTangent * movevars.gravity * pm_frametime,
				transVector, pmove.velocity);
	}
}

static void PM_HookApplyGroundBias(vec3_t uv_hook, float maxPull)
{
	float preserveFactor, scale;

	PM_HookSetMinimumRadialSpeed(uv_hook, HOOK_GROUND_DETACH_SPEED);
	PM_HookSetMinimumGroundLift(uv_hook);

	scale = HOOK_GROUND_TANGENTIAL_SCALE;
	preserveFactor = PM_HookPreserveFactor();
	if ((preserveFactor > 0) && (pmove.hook_initial_speed > maxPull)) {
		scale += (HOOK_GROUND_FAST_TANGENTIAL_SCALE - HOOK_GROUND_TANGENTIAL_SCALE) * preserveFactor;
	}

	PM_HookDampenTangentialVelocity(uv_hook, scale);
}

static void PM_HookApplyOscillation(vec3_t uv_hook, float distanceToHook)
{
	vec3_t radialVel, tangentialVel, transVector;
	float radialSpeed, threshold, magnitude;

	threshold = HOOK_OSCILLATION_THRESHOLD_SCALE * pmove.hook_initial_length;
	if (distanceToHook >= threshold) {
		return;
	}

	magnitude = PM_HookOscillationFactor(distanceToHook, threshold, DotProduct(pmove.velocity, uv_hook));
	VectorScale(uv_hook, magnitude, transVector);
	VectorMA(pmove.velocity, pm_frametime, transVector, pmove.velocity);

	if (pmove.hook_time < HOOK_OSCILLATION_DAMPING_DELAY) {
		return;
	}

	PM_HookDecomposeVelocity(pmove.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);
	if (radialSpeed > 0) {
		VectorScale(uv_hook, radialSpeed * HOOK_OSCILLATION_DAMPING, radialVel);
	}

	VectorScale(tangentialVel, HOOK_OSCILLATION_TANGENTIAL_DAMPING, tangentialVel);
	VectorAdd(radialVel, tangentialVel, pmove.velocity);
}

static void PM_HookCapVelocity(vec3_t uv_hook, float maxPull)
{
	vec3_t radialVel;
	vec3_t tangentialVel;
	float radialSpeed;
	float tangentialSpeed;
	float totalSpeed;
	float cap;
	float radialCap;
	float preserveFactor;

	PM_HookDecomposeVelocity(pmove.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);
	preserveFactor = PM_HookPreserveFactor();

	radialCap = PM_HookPreservedCap(maxPull * HOOK_RADIAL_SPEED_CAP, pmove.hook_initial_radial_speed, preserveFactor);
	radialSpeed = bound(-(maxPull * HOOK_RADIAL_AWAY_CAP), radialSpeed, radialCap);
	VectorScale(uv_hook, radialSpeed, radialVel);

	tangentialSpeed = VectorNormalize(tangentialVel);
	cap = PM_HookPreservedCap(maxPull * HOOK_TANGENTIAL_SPEED_CAP, pmove.hook_initial_tangential_speed, preserveFactor);
	if (tangentialSpeed > cap) {
		VectorScale(tangentialVel, cap, tangentialVel);
	}
	else {
		VectorScale(tangentialVel, tangentialSpeed, tangentialVel);
	}

	VectorAdd(radialVel, tangentialVel, pmove.velocity);

	totalSpeed = VectorLength(pmove.velocity);
	cap = PM_HookPreservedCap(maxPull * HOOK_TOTAL_SPEED_CAP, pmove.hook_initial_speed, preserveFactor);
	if (totalSpeed > cap) {
		VectorScale(pmove.velocity, cap / totalSpeed, pmove.velocity);
	}
}

static void PM_HookPreserveReelSpeed(float minSpeed, float reelEffect, float entryRadialSpeed,
		float entryTangentialSpeed, float maxPull)
{
	float totalSpeed, preserveSpeed, awayFactor;

	if (reelEffect <= 0 || minSpeed <= HOOK_EPSILON) {
		return;
	}

	preserveSpeed = minSpeed;
	if (entryRadialSpeed < 0) {
		awayFactor = bound(0, -entryRadialSpeed / max(maxPull, HOOK_EPSILON), 1);
		preserveSpeed = max(entryTangentialSpeed, minSpeed * (1.0f - awayFactor));
	}

	totalSpeed = VectorLength(pmove.velocity);
	preserveSpeed = totalSpeed + (preserveSpeed - totalSpeed) * reelEffect;
	if (totalSpeed < HOOK_EPSILON || totalSpeed >= preserveSpeed) {
		return;
	}

	VectorScale(pmove.velocity, preserveSpeed / totalSpeed, pmove.velocity);
}

static qbool PM_HookMove(void)
{
	vec3_t hookVector;
	vec3_t uv_hook;
	vec3_t uv_pull;
	vec3_t wishDir;
	vec3_t tangentDir;
	vec3_t entryRadialVel;
	vec3_t entryTangentialVel;
	float distanceToHook;
	float minPull;
	float maxPull;
	float wishAlign;
	float damping, reelEffect, reelPullEffect, suppressEffect, entrySpeed, entryRadialSpeed, entryTangentialSpeed;
	qbool useGroundBias;
	qbool wasOnGround;
	qbool holdHeld;
	qbool reelHeld;

	if (pmove.hook_state != mvd_hook_anchored) {
		pmove.hook_hold_blend = 0;
		pmove.hook_reel_blend = 0;
		pmove.hook_reel_pull_blend = 0;
		pmove.hook_input_mode = HOOK_INPUT_NONE;
		pmove.hook_hold_washeld = false;
		pmove.hook_reel_washeld = false;
		return false;
	}

	VectorSubtract(pmove.hook_anchor, pmove.origin, hookVector);
	VectorCopy(hookVector, uv_hook);
	distanceToHook = VectorNormalize(uv_hook);
	if (distanceToHook < HOOK_EPSILON) {
		return false;
	}

	wasOnGround = pmove.onground;
	useGroundBias = wasOnGround && (uv_hook[2] > HOOK_GROUND_DETACH_MIN_UP);
	PM_HookGetPullVector(uv_hook, wasOnGround, uv_pull);
	pmove.onground = false;
	entrySpeed = VectorLength(pmove.velocity);
	PM_HookDecomposeVelocity(pmove.velocity, uv_pull, entryRadialVel, entryTangentialVel, &entryRadialSpeed);
	entryTangentialSpeed = VectorLength(entryTangentialVel);
	minPull = (pmove.hook_min_pull > 0) ? pmove.hook_min_pull : HOOK_INIT_PULL_SPEED;
	maxPull = (pmove.hook_max_pull > 0) ? pmove.hook_max_pull : HOOK_PULL_SPEED;
	minPull *= HOOK_MIN_PULL_SCALE;
	maxPull *= HOOK_MAX_PULL_SCALE;
	holdHeld = pmove.cmd.forwardmove < 0;
	reelHeld = pmove.cmd.forwardmove > 0;
	PM_HookUpdateInputBlends(holdHeld, reelHeld);
	reelEffect = PM_HookEaseBlend(pmove.hook_reel_blend);
	reelPullEffect = PM_HookEaseBlend(pmove.hook_reel_pull_blend);
	maxPull *= 1.0f + reelPullEffect * (HOOK_INPUT_REEL_MAX_PULL_SCALE - 1.0f);
	wishAlign = PM_HookMovementInfluence(uv_pull, wishDir, tangentDir);

	PM_HookApplyRadialPull(uv_pull, distanceToHook, minPull, maxPull, wishAlign, pmove.hook_hold_blend,
			pmove.hook_reel_blend, pmove.hook_reel_pull_blend, wasOnGround);
	PM_HookApplyInputControl(tangentDir, wishAlign, pmove.hook_reel_blend);
	suppressEffect = reelEffect * reelEffect;
	if (suppressEffect > 0) {
		damping = 1.0f + suppressEffect * (HOOK_INPUT_REEL_TANGENTIAL_DAMPING - 1.0f);
		PM_HookDampenTangentialVelocity(uv_pull, damping);
	}
	if (useGroundBias) {
		PM_HookApplyGroundBias(uv_pull, maxPull);
	}
	else if (!wasOnGround) {
		PM_HookApplyGravityInfluence(uv_pull, maxPull, wishAlign, pmove.hook_reel_blend);
	}
	PM_HookApplyOscillation(uv_pull, distanceToHook);
	PM_HookCapVelocity(uv_pull, maxPull);
	PM_HookPreserveReelSpeed(entrySpeed, reelPullEffect, entryRadialSpeed, entryTangentialSpeed, maxPull);
	return true;
}

//The basic solid body movement clip that slides along multiple planes
#define	MAX_CLIP_PLANES 5
static int PM_SlideMove (void)
{
	vec3_t dir, planes[MAX_CLIP_PLANES], primal_velocity, original_velocity, end;
	int bumpcount, numbumps, i, j, blocked, numplanes;
	float d, time_left;
	trace_t trace;

	numbumps = 4;
	blocked = 0;
	VectorCopy (pmove.velocity, original_velocity);
	VectorCopy (pmove.velocity, primal_velocity);
	numplanes = 0;

	time_left = pm_frametime;

	for (bumpcount = 0; bumpcount < numbumps; bumpcount++) {
		VectorMA(pmove.origin, time_left, pmove.velocity, end);
		trace = PM_PlayerTrace (pmove.origin, end);

		if (trace.startsolid || trace.allsolid) {
			// entity is trapped in another solid
			VectorClear (pmove.velocity);
			return 3;
		}

		if (trace.fraction > 0) {
			// actually covered some distance
			VectorCopy (trace.endpos, pmove.origin);
			numplanes = 0;
		}

		if (trace.fraction == 1) {
			break; // moved the entire distance
		}

		// save entity for contact
		PM_AddTouchedEnt (trace.e.entnum);

		if (trace.plane.normal[2] >= MIN_STEP_NORMAL)
			blocked |= BLOCKED_FLOOR;
		else if (!trace.plane.normal[2])
			blocked |= BLOCKED_STEP;
		else
			blocked |= BLOCKED_OTHER;

		time_left -= time_left * trace.fraction;

		// cliped to another plane
		if (numplanes >= MAX_CLIP_PLANES) {
			// this shouldn't really happen
			VectorClear (pmove.velocity);
			break;
		}

		VectorCopy (trace.plane.normal, planes[numplanes]);
		numplanes++;

		// modify original_velocity so it parallels all of the clip planes
		for (i = 0; i < numplanes; i++) {
			PM_ClipVelocity (original_velocity, planes[i], pmove.velocity, 1);
			for (j = 0; j < numplanes; j++) {
				if (j != i) {
					if (DotProduct(pmove.velocity, planes[j]) < 0) {
						break; // not ok
					}
				}
			}
			if (j == numplanes) {
				break;
			}
		}

		if (i != numplanes) {
			// go along this plane
		}
		else {
			// go along the crease
			if (numplanes != 2) {
				VectorClear (pmove.velocity);
				break;
			}
			CrossProduct (planes[0], planes[1], dir);
			d = DotProduct (dir, pmove.velocity);
			VectorScale (dir, d, pmove.velocity);
		}

		// if velocity is against the original velocity, stop dead
		// to avoid tiny occilations in sloping corners
		if (DotProduct (pmove.velocity, primal_velocity) <= 0) {
			VectorClear (pmove.velocity);
			break;
		}
	}

	if (pmove.waterjumptime) {
		VectorCopy(primal_velocity, pmove.velocity);
	}

	return blocked;
}

//Each intersection will try to step over the obstruction instead of sliding along it.
static int PM_StepSlideMove (qbool in_air)
{
	vec3_t original, originalvel, down, up, downvel, dest;
	float downdist, updist, stepsize;
	trace_t trace;
	int blocked;

	// try sliding forward both on ground and up 16 pixels
	// take the move that goes farthest
	VectorCopy (pmove.origin, original);
	VectorCopy (pmove.velocity, originalvel);

	blocked = PM_SlideMove ();

	if (!blocked) {
		return blocked; // moved the entire distance
	}

	if (in_air) {
		// don't let us step up unless it's indeed a step we bumped in
		// (that is, there's solid ground below)
		float *org;

		if (!(blocked & BLOCKED_STEP)) {
			return blocked;
		}

		org = (originalvel[2] < 0) ? pmove.origin : original;
		VectorCopy (org, dest);
		dest[2] -= STEPSIZE;
		trace = PM_PlayerTrace (org, dest);
		if (trace.fraction == 1 || trace.plane.normal[2] < MIN_STEP_NORMAL) {
			return blocked;
		}

		// adjust stepsize, otherwise it would be possible to walk up a
		// a step higher than STEPSIZE
		stepsize = STEPSIZE - (org[2] - trace.endpos[2]);
	}
	else {
		stepsize = STEPSIZE;
	}

	VectorCopy (pmove.origin, down);
	VectorCopy (pmove.velocity, downvel);

	VectorCopy (original, pmove.origin);
	VectorCopy (originalvel, pmove.velocity);

	// move up a stair height
	VectorCopy (pmove.origin, dest);
	dest[2] += stepsize;
	trace = PM_PlayerTrace (pmove.origin, dest);
	if (!trace.startsolid && !trace.allsolid) {
		VectorCopy(trace.endpos, pmove.origin);
	}

	if (in_air && originalvel[2] < 0) {
		pmove.velocity[2] = 0;
	}

	PM_SlideMove ();

	// press down the stepheight
	VectorCopy (pmove.origin, dest);
	dest[2] -= stepsize;
	trace = PM_PlayerTrace (pmove.origin, dest);
	if (trace.fraction != 1 && trace.plane.normal[2] < MIN_STEP_NORMAL) {
		goto usedown;
	}
	if (!trace.startsolid && !trace.allsolid) {
		VectorCopy(trace.endpos, pmove.origin);
	}

	if (pmove.origin[2] < original[2]) {
		goto usedown;
	}

	VectorCopy (pmove.origin, up);

	// decide which one went farther
	downdist = (down[0] - original[0]) * (down[0] - original[0])
		+ (down[1] - original[1]) * (down[1] - original[1]);
	updist = (up[0] - original[0]) * (up[0] - original[0])
		+ (up[1] - original[1]) * (up[1] - original[1]);

	if (downdist >= updist) {
usedown:
		VectorCopy (down, pmove.origin);
		VectorCopy (downvel, pmove.velocity);
		return blocked;
	}

	// copy z value from slide move
	pmove.velocity[2] = downvel[2];

	if (!pmove.onground && pmove.waterlevel < 2 && (blocked & BLOCKED_STEP)) {
		float scale;
		// in pm_airstep mode, walking up a 16 unit high step
		// will kill 16% of horizontal velocity
		scale = 1 - 0.01*(pmove.origin[2] - original[2]);
		pmove.velocity[0] *= scale;
		pmove.velocity[1] *= scale;
	}

	return blocked;
}

//Handles both ground friction and water friction
static void PM_Friction(void)
{
	float speed, newspeed, control, friction, drop;
	vec3_t start, stop;
	trace_t trace;

	if (pmove.waterjumptime)
		return;

	speed = VectorLength(pmove.velocity);
	if (speed < 1) {
		pmove.velocity[0] = pmove.velocity[1] = 0;
		if (pmove.pm_type == PM_FLY) {
			pmove.velocity[2] = 0;
		}
		return;
	}

	if (pmove.waterlevel >= 2) {
		// apply water friction, even if in fly mode
		drop = speed * movevars.waterfriction * pmove.waterlevel * pm_frametime;
	}
	else if (pmove.pm_type == PM_FLY) {
		// apply flymode friction
		drop = speed * pm_flyfriction * pm_frametime;
	}
	else if (pmove.onground) {
		// apply ground friction
		friction = movevars.friction;

		// if the leading edge is over a dropoff, increase friction
		start[0] = stop[0] = pmove.origin[0] + pmove.velocity[0] / speed * 16;
		start[1] = stop[1] = pmove.origin[1] + pmove.velocity[1] / speed * 16;
		start[2] = pmove.origin[2] + player_mins[2];
		stop[2] = start[2] - 34;
		trace = PM_PlayerTrace(start, stop);
		if (trace.fraction == 1) {
			friction *= 2;
		}

		control = speed < movevars.stopspeed ? movevars.stopspeed : speed;
		drop = control * friction * pm_frametime;
	}
	else {
		return; // in air, no friction
	}

	// scale the velocity
	newspeed = speed - drop;
	newspeed = max(newspeed, 0);
	newspeed /= speed;

	VectorScale(pmove.velocity, newspeed, pmove.velocity);
}

static void PM_Accelerate(vec3_t wishdir, float wishspeed, float accel)
{
	float addspeed, accelspeed, currentspeed;

	if (pmove.pm_type == PM_DEAD)
		return;
	if (pmove.waterjumptime)
		return;

	currentspeed = DotProduct(pmove.velocity, wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = accel * pm_frametime * wishspeed;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	VectorMA(pmove.velocity, accelspeed, wishdir, pmove.velocity);
}

#ifndef SERVERONLY
#ifdef EXPERIMENTAL_SHOW_ACCELERATION
qbool player_in_air = false;
float cosinus_val = 0.f;
qbool flag_player_pmove;
#endif
#endif

static void PM_AirAccelerate(vec3_t wishdir, float wishspeed, float accel)
{
	float addspeed, accelspeed, currentspeed, wishspd = wishspeed;
	float originalspeed = 0.0, newspeed = 0.0, speedcap = 0.0;

	if (pmove.pm_type == PM_DEAD)
		return;
	if (pmove.waterjumptime)
		return;

	if (movevars.bunnyspeedcap > 0)
		originalspeed = sqrt(pmove.velocity[0] * pmove.velocity[0] + pmove.velocity[1] * pmove.velocity[1]);

	wishspd = min(wishspd, 30);
	currentspeed = DotProduct(pmove.velocity, wishdir);
	addspeed = wishspd - currentspeed;

#ifdef EXPERIMENTAL_SHOW_ACCELERATION
	if (flag_player_pmove) {
		cosinus_val = 0.f;
		originalspeed = sqrt(pmove.velocity[0] * pmove.velocity[0] + pmove.velocity[1] * pmove.velocity[1]);
		if (originalspeed > 1.f) {
			cosinus_val = currentspeed / originalspeed;
		}

		player_in_air = true;
	}
#endif

	if (addspeed <= 0)
		return;
	accelspeed = accel * wishspeed * pm_frametime;
	accelspeed = min(accelspeed, addspeed);

	VectorMA(pmove.velocity, accelspeed, wishdir, pmove.velocity);

	if (movevars.bunnyspeedcap > 0) {
		newspeed = sqrt(pmove.velocity[0] * pmove.velocity[0] + pmove.velocity[1] * pmove.velocity[1]);
		if (newspeed > originalspeed) {
			speedcap = movevars.maxspeed * movevars.bunnyspeedcap;
			if (newspeed > speedcap) {
				if (originalspeed < speedcap)
					originalspeed = speedcap;
				pmove.velocity[0] *= originalspeed / newspeed;
				pmove.velocity[1] *= originalspeed / newspeed;
			}
		}
	}
}

static int PM_WaterMove(void)
{
	vec3_t wishvel, wishdir;
	float wishspeed;
	int i;

	// user intentions
	for (i = 0; i < 3; i++)
		wishvel[i] = pm_forward[i] * pmove.cmd.forwardmove + pm_right[i] * pmove.cmd.sidemove;

	if (pmove.pm_type != PM_FLY && !pmove.cmd.forwardmove && !pmove.cmd.sidemove && !pmove.cmd.upmove)
		wishvel[2] -= 60; // drift towards bottom
	else
		wishvel[2] += pmove.cmd.upmove;

	VectorCopy(wishvel, wishdir);
	wishspeed = VectorNormalize(wishdir);

	if (wishspeed > movevars.maxspeed) {
		VectorScale(wishvel, movevars.maxspeed / wishspeed, wishvel);
		wishspeed = movevars.maxspeed;
	}
	wishspeed *= 0.7;

	// water acceleration
	PM_Accelerate(wishdir, wishspeed, movevars.wateraccelerate);

	return PM_StepSlideMove(false);
}

static int PM_FlyMove(void)
{
	vec3_t wishvel, wishdir;
	float wishspeed;
	int i;

	for (i = 0; i < 3; i++)
		wishvel[i] = pm_forward[i] * pmove.cmd.forwardmove + pm_right[i] * pmove.cmd.sidemove;

	wishvel[2] += pmove.cmd.upmove;

	VectorCopy(wishvel, wishdir);
	wishspeed = VectorNormalize(wishdir);

	if (wishspeed > movevars.maxspeed) {
		VectorScale(wishvel, movevars.maxspeed / wishspeed, wishvel);
		wishspeed = movevars.maxspeed;
	}

	PM_Accelerate(wishdir, wishspeed, movevars.accelerate);
	return PM_StepSlideMove(false);
}

static int PM_AirMove(void)
{
	float fmove, smove, wishspeed;
	vec3_t wishvel, wishdir;
	int i;

	fmove = pmove.cmd.forwardmove;
	smove = pmove.cmd.sidemove;

	pm_forward[2] = 0;
	pm_right[2] = 0;
	VectorNormalize(pm_forward);
	VectorNormalize(pm_right);

	for (i = 0; i < 2; i++)
		wishvel[i] = pm_forward[i] * fmove + pm_right[i] * smove;
	wishvel[2] = 0;

	VectorCopy(wishvel, wishdir);
	wishspeed = VectorNormalize(wishdir);

	// clamp to server defined max speed
	if (wishspeed > movevars.maxspeed) {
		VectorScale(wishvel, movevars.maxspeed / wishspeed, wishvel);
		wishspeed = movevars.maxspeed;
	}

	if (pmove.onground) {
		if (movevars.slidefix) {
			pmove.velocity[2] = min(pmove.velocity[2], 0); // bound above by 0
			PM_Accelerate(wishdir, wishspeed, movevars.accelerate);
			// add gravity
			pmove.velocity[2] -= movevars.entgravity * movevars.gravity * pm_frametime;
		}
		else {
			pmove.velocity[2] = 0;
			PM_Accelerate(wishdir, wishspeed, movevars.accelerate);
		}

		if (!pmove.velocity[0] && !pmove.velocity[1]) {
			pmove.velocity[2] = 0;
			return 0;
		}

		return PM_StepSlideMove(false);
	}
	else {
		int blocked;
		// not on ground, so little effect on velocity
		PM_AirAccelerate(wishdir, wishspeed, movevars.accelerate);

		// add gravity
		pmove.velocity[2] -= movevars.entgravity * movevars.gravity * pm_frametime;

		if (movevars.airstep)
			blocked = PM_StepSlideMove(true);
		else
			blocked = PM_SlideMove();

		if (movevars.pground) {
			if (blocked & BLOCKED_FLOOR) {
				pmove.onground = true;
			}
		}

		return blocked;
	}
}

#define MAXGROUNDSPEED_DEFAULT 180
#define MAXGROUNDSPEED_MAXIMUM 240

static void PM_RampEdgeAdjustNormal(vec3_t normal, int flags)
{
	int i;

	for (i = 0; i < 3; ++i) {
		if (flags & (PHYSICSNORMAL_FLIPX << i)) {
			if (pmove.velocity[i] < 0) {
				normal[i] = -normal[i];
			}
			else if (pmove.velocity[0] == 0) {
				normal[i] = 0;
			}
		}
	}
}

#define PM_FarFromGround(trace) (((trace).fraction == 1 || (trace).plane.normal[2] < MIN_STEP_NORMAL))

static trace_t PM_CategorizePositionRunTrace(vec3_t point)
{
	trace_t trace = { 0 };

	trace = PM_PlayerTrace(pmove.origin, point);
	if (!PM_FarFromGround(trace)) {
		VectorCopy(trace.plane.normal, groundnormal);
	}

	return trace;
}

void PM_CategorizePosition(void)
{
	trace_t trace = { 0 };
	vec3_t point;
	int cont;
	mphysicsnormal_t ground;

	pmove.maxgroundspeed = MAXGROUNDSPEED_DEFAULT;

	// if the player hull point one unit down is solid, the player is on ground
	// see if standing on something solid
	point[0] = pmove.origin[0];
	point[1] = pmove.origin[1];
	point[2] = pmove.origin[2] - 1;

	if (movevars.rampjump) {
		// Increase speed limit for player as steepness of the floor increases
		trace = PM_CategorizePositionRunTrace(point);
		ground = CM_PhysicsNormal(trace.physicsnormal);

		if (ground.flags & PHYSICSNORMAL_SET) {
			VectorCopy(ground.normal, groundnormal);
			PM_RampEdgeAdjustNormal(groundnormal, ground.flags);
			VectorNormalize(groundnormal);

			if (movevars.rampjump && !PM_FarFromGround(trace) && trace.e.entnum == 0 && groundnormal[2] > MIN_STEP_NORMAL && groundnormal[2] < 1 && DotProduct(groundnormal, pmove.velocity) < MAX_JUMPFIX_DOTPRODUCT) {
				// They are moving up a ramp, increase maxspeed to check if we keep them on it
				float range = 1.0 - asin(groundnormal[2]) * 2 / M_PI; // asin() returns 0...PI/2, so range is [1...0]

				// Max out at 45 degree ramps...
				range = min(range, 0.5f) * 2;

				pmove.maxgroundspeed += (MAXGROUNDSPEED_MAXIMUM - MAXGROUNDSPEED_DEFAULT) * range;
			}
		}
	}

	if (pmove.velocity[2] > pmove.maxgroundspeed) {
		pmove.onground = false;
	}
	else if (!movevars.pground || pmove.onground) {
		if (!movevars.rampjump) {
			trace = PM_CategorizePositionRunTrace(point);
		}
		if (PM_FarFromGround(trace)) {
			pmove.onground = false;
		}
		else {
			pmove.onground = true;
			pmove.groundent = trace.e.entnum;
			pmove.waterjumptime = 0;
		}

		// standing on an entity other than the world
		if (trace.e.entnum > 0) {
			PM_AddTouchedEnt(trace.e.entnum);
		}
	}

	// get waterlevel
	pmove.waterlevel = 0;
	pmove.watertype = CONTENTS_EMPTY;

	point[2] = pmove.origin[2] + player_mins[2] + 1;
	cont = PM_PointContents (point);
	if (cont <= CONTENTS_WATER) {
		pmove.watertype = cont;
		pmove.waterlevel = 1;
		point[2] = pmove.origin[2] + (player_mins[2] + player_maxs[2]) * 0.5;
		cont = PM_PointContents (point);
		if (cont <= CONTENTS_WATER) {
			pmove.waterlevel = 2;
			point[2] = pmove.origin[2] + 22;
			cont = PM_PointContents (point);
			if (cont <= CONTENTS_WATER) {
				pmove.waterlevel = 3;
			}
		}
	}

	if (!movevars.pground) {
		if (pmove.onground && pmove.pm_type != PM_FLY && pmove.waterlevel < 2) {
			// snap to ground so that we can't jump higher than we're supposed to
			if (!trace.startsolid && !trace.allsolid) {
				VectorCopy(trace.endpos, pmove.origin);
			}
		}
	}
}

static void PM_CheckJump (void)
{
	if (pmove.pm_type == PM_FLY)
		return;

	if (pmove.pm_type == PM_DEAD) {
		pmove.jump_held = true; // don't jump on respawn
		return;
	}

	if (!(pmove.cmd.buttons & BUTTON_JUMP)) {
		pmove.jump_held = false;
		return;
	}

	if (pmove.waterjumptime) {
		return;
	}

	if (pmove.waterlevel >= 2) {
		// swimming, not jumping
		pmove.onground = false;

		if (pmove.watertype == CONTENTS_WATER)
			pmove.velocity[2] = 100;
		else if (pmove.watertype == CONTENTS_SLIME)
			pmove.velocity[2] = 80;
		else
			pmove.velocity[2] = 50;
		return;
	}

	if (!pmove.onground)
		return; // in air, so no effect

	if (pmove.jump_held && !pmove.jump_msec)
		return; // don't pogo stick

	if (!movevars.pground) {
		// check for jump bug
		// groundplane normal was set in the call to PM_CategorizePosition
		if ((movevars.rampjump || pmove.velocity[2] < 0) && DotProduct(pmove.velocity, groundnormal) < MAX_JUMPFIX_DOTPRODUCT) {
			// pmove.velocity is pointing into the ground, clip it
			PM_ClipVelocity(pmove.velocity, groundnormal, pmove.velocity, 1);
		}
	}

	pmove.onground = false;
	if (pmove.maxgroundspeed > MAXGROUNDSPEED_DEFAULT && pmove.velocity[2] > MAXGROUNDSPEED_DEFAULT) {
		// we adjusted maxspeed to keep them on ground, need to reduce velocity here so they can't jump too high
		pmove.velocity[2] = MAXGROUNDSPEED_DEFAULT;
	}
	pmove.velocity[2] += 270;

	if (movevars.ktjump > 0) {
		// meag: pmove.velocity[2] = max(pmove.velocity[2], 270); (?)
		if (movevars.ktjump > 1)
			movevars.ktjump = 1;
		if (pmove.velocity[2] < 270)
			pmove.velocity[2] = pmove.velocity[2] * (1 - movevars.ktjump) + 270 * movevars.ktjump;
	}

	pmove.jump_held = true; // don't jump again until released
	pmove.jump_msec = pmove.cmd.msec;
}

static void PM_CheckWaterJump (void)
{
	vec3_t flatforward;
	vec3_t spot;
	int cont;

	if (pmove.waterjumptime)
		return;

	// don't hop out if we just jumped in
	if (pmove.velocity[2] < -180)
		return;

	// see if near an edge
	flatforward[0] = pm_forward[0];
	flatforward[1] = pm_forward[1];
	flatforward[2] = 0;
	VectorNormalize (flatforward);

	VectorMA (pmove.origin, 24, flatforward, spot);
	spot[2] += 8;
	cont = PM_PointContents_AllBSPs (spot);
	if (cont != CONTENTS_SOLID)
		return;
	spot[2] += 24;
	cont = PM_PointContents_AllBSPs (spot);
	if (cont != CONTENTS_EMPTY)
		return;
	// jump out of water
	VectorScale (flatforward, 50, pmove.velocity);
	pmove.velocity[2] = 310;
	pmove.waterjumptime = 2; // safety net
	pmove.jump_held = true; // don't jump again until released
}

//If pmove.origin is in a solid position,
//try nudging slightly on all axis to
//allow for the cut precision of the net coordinates
static void PM_NudgePosition (void)
{
	static int sign[3] = {0, -1, 1};
	int x, y, z, i;
	vec3_t base;

	VectorCopy (pmove.origin, base);

	for (i = 0; i < 3; i++)
		pmove.origin[i] = ((int) (pmove.origin[i] * 8)) * 0.125;

	for (z = 0; z <= 2; z++) {
		for (y = 0; y <= 2; y++) {
			for (x = 0; x <= 2; x++) {
				pmove.origin[0] = base[0] + (sign[x] * 0.125);
				pmove.origin[1] = base[1] + (sign[y] * 0.125);
				pmove.origin[2] = base[2] + (sign[z] * 0.125);
				if (PM_TestPlayerPosition (pmove.origin))
					return;
			}
		}
	}

	// some maps spawn the player several units into the ground
	for (z = 1; z <= 18; z++) {
		pmove.origin[0] = base[0];
		pmove.origin[1] = base[1];
		pmove.origin[2] = base[2] + z;
		if (PM_TestPlayerPosition(pmove.origin))
			return;
	}

	VectorCopy (base, pmove.origin);
}

static void PM_SpectatorMove(void)
{
	float newspeed, currentspeed, addspeed, accelspeed, wishspeed;
	float speed, drop, friction, control, fmove, smove;
	vec3_t wishvel, wishdir;
	int i;

	// friction
	speed = VectorLength(pmove.velocity);
	if (speed < 1) {
		VectorClear(pmove.velocity);
	}
	else {
		friction = movevars.friction * 1.5; // extra friction
		control = speed < movevars.stopspeed ? movevars.stopspeed : speed;
		drop = control * friction * pm_frametime;

		// scale the velocity
		newspeed = speed - drop;
		if (newspeed < 0) {
			newspeed = 0;
		}
		newspeed /= speed;

		VectorScale(pmove.velocity, newspeed, pmove.velocity);
	}

	// accelerate
	fmove = pmove.cmd.forwardmove;
	smove = pmove.cmd.sidemove;

	VectorNormalize(pm_forward);
	VectorNormalize(pm_right);

	for (i = 0; i < 3; i++) {
		wishvel[i] = pm_forward[i] * fmove + pm_right[i] * smove;
	}
	wishvel[2] += pmove.cmd.upmove;

	VectorCopy(wishvel, wishdir);
	wishspeed = VectorNormalize(wishdir);

	// clamp to server defined max speed
	if (wishspeed > movevars.spectatormaxspeed) {
		VectorScale(wishvel, movevars.spectatormaxspeed / wishspeed, wishvel);
		wishspeed = movevars.spectatormaxspeed;
	}

	currentspeed = DotProduct(pmove.velocity, wishdir);
	addspeed = wishspeed - currentspeed;

	// Buggy QW spectator mode, kept for compatibility
	if (pmove.pm_type == PM_OLD_SPECTATOR) {
		if (addspeed <= 0) {
			return;
		}
	}

	if (addspeed > 0) {
		accelspeed = movevars.accelerate * pm_frametime * wishspeed;
		accelspeed = min(accelspeed, addspeed);
		VectorMA(pmove.velocity, accelspeed, wishdir, pmove.velocity);
	}

	// move
	VectorMA(pmove.origin, pm_frametime, pmove.velocity, pmove.origin);
}

//Returns with origin, angles, and velocity modified in place.
//Numtouch and touchindex[] will be set if any of the physents were contacted during the move.
int PM_PlayerMove(void)
{
	int blocked = 0;
	int hook_forwardmove = 0;
	qbool hook_controls_forward = false;

#ifndef SERVERONLY
#ifdef EXPERIMENTAL_SHOW_ACCELERATION
	if (flag_player_pmove) player_in_air = false;
#endif
#endif

	pm_frametime = pmove.cmd.msec * 0.001;
	pmove.numtouch = 0;

	if (pmove.pm_type == PM_NONE || pmove.pm_type == PM_LOCK) {
		PM_CategorizePosition();
		return 0;
	}

	// take angles directly from command
	VectorCopy(pmove.cmd.angles, pmove.angles);
	AngleVectors(pmove.angles, pm_forward, pm_right, NULL);

	if (pmove.pm_type == PM_SPECTATOR || pmove.pm_type == PM_OLD_SPECTATOR) {
		PM_SpectatorMove();
		pmove.onground = false;
		return 0;
	}

	PM_NudgePosition();

	// set onground, watertype, and waterlevel
	PM_CategorizePosition();

	if (pmove.waterlevel == 2 && pmove.pm_type != PM_FLY)
		PM_CheckWaterJump();

	if (pmove.velocity[2] < 0 || pmove.pm_type == PM_DEAD)
		pmove.waterjumptime = 0;

	if (pmove.waterjumptime) {
		pmove.waterjumptime -= pm_frametime;
		if (pmove.waterjumptime < 0)
			pmove.waterjumptime = 0;
	}

	if (pmove.jump_msec) {
		pmove.jump_msec += pmove.cmd.msec;
		if (pmove.jump_msec > 50)
			pmove.jump_msec = 0;
	}

	PM_CheckJump();

	PM_Friction();
	hook_forwardmove = pmove.cmd.forwardmove;
	hook_controls_forward = PM_HookMove();
	if (hook_controls_forward) {
		pmove.cmd.forwardmove = 0;
	}

	if (pmove.waterlevel >= 2)
		blocked = PM_WaterMove();
	else if (pmove.pm_type == PM_FLY)
		blocked = PM_FlyMove();
	else
		blocked = PM_AirMove();
	pmove.cmd.forwardmove = hook_forwardmove;

	// set onground, watertype, and waterlevel for final spot
	PM_CategorizePosition();

	if (!movevars.pground) {
		// this is to make sure landing sound is not played twice
		// and falling damage is calculated correctly
		if (pmove.onground && pmove.velocity[2] < -300) {
			if (DotProduct(pmove.velocity, groundnormal) < MAX_JUMPFIX_DOTPRODUCT) {
				PM_ClipVelocity(pmove.velocity, groundnormal, pmove.velocity, 1);
			}
		}
	}

	return blocked;
}
