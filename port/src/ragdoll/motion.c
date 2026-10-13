/**
 * MOD: ragdoll physics - a dying body's own movement: muscle tone, reflexes
 * and the animation blend. Part of the ragdoll module (port/src/ragdoll.c).
 */

#include <stdarg.h>
#include <stdio.h>
#include "rd_internal.h"

#ifdef PD_PHYSICS

/* ---- muscle tone: the death animation drives the joints for a while ---- */

#define RD_MUSCLE_FREQ 8.0f // motor spring frequency (Hz)


/**
 * Samples the chr's (still advancing) death animation into joint frames,
 * without root motion. Only the rotations between parts are used.
 */
static bool ragdollSampleAnimPose(struct ragdoll *rd, Mtxf *out)
{
	struct model *model = rd->model;
	struct modeldef *def = model->definition;
	struct modelrenderdata renderdata = {NULL, true, 3};
	Mtxf identity;
	Mtxf *buffer;
	Mtxf *saved;
	s32 i;

	if (model->anim == NULL) {
		return false;
	}

	buffer = malloc(sizeof(Mtxf) * def->nummatrices);

	if (!buffer) {
		return false;
	}

	mtx4LoadIdentity(&identity);
	renderdata.unk00 = &identity;
	renderdata.unk10 = buffer;

	saved = model->matrices;
	g_RagdollSamplingAnim = true;
	modelSetMatricesWithAnim(&renderdata, model);
	g_RagdollSamplingAnim = false;
	model->matrices = saved;

	for (i = 0; i < rd->numparts; i++) {
		Mtxf *src;
		f32 len;
		s32 c, r;

		if (rd->mtxindex[i] < 0 || rd->mtxindex[i] >= def->nummatrices) {
			free(buffer);
			return false;
		}

		src = &buffer[rd->mtxindex[i]];

		for (c = 0; c < 3; c++) {
			len = rdVecLen(src->m[c]);

			if (len < 0.0001f) {
				len = 1.0f;
			}

			for (r = 0; r < 3; r++) {
				out[i].m[c][r] = src->m[c][r] / len;
			}

			out[i].m[c][3] = 0.0f;
		}

		rdVecCopy(src->m[3], out[i].m[3]);
		out[i].m[3][3] = 1.0f;
	}

	free(buffer);
	return true;
}

// Rotation matrix that turns unit vector a onto unit vector b (Rodrigues)
static void rdRotFromTo(const f32 *a, const f32 *b, Mtxf *out)
{
	f32 v[3];
	f32 c = rdVecDot(a, b);
	f32 k;
	f32 r[3][3];
	s32 i, j;

	rdVecCross(a, b, v);

	if (c < -0.999f) {
		// Opposite: half turn around any perpendicular axis
		f32 axis[3] = { -a[1], a[0], 0.0f };

		if (fabsf(a[2]) > 0.9f) {
			axis[0] = 0.0f; axis[1] = -a[2]; axis[2] = a[1];
		}

		rdVecNorm(axis);

		for (i = 0; i < 3; i++) {
			for (j = 0; j < 3; j++) {
				r[i][j] = 2.0f * axis[i] * axis[j] - (i == j ? 1.0f : 0.0f);
			}
		}
	} else {
		k = 1.0f / (1.0f + c);

		// r[row][col] = I + [v]x + [v]x^2 / (1 + c)
		r[0][0] = c + v[0] * v[0] * k;
		r[0][1] = v[0] * v[1] * k - v[2];
		r[0][2] = v[0] * v[2] * k + v[1];
		r[1][0] = v[1] * v[0] * k + v[2];
		r[1][1] = c + v[1] * v[1] * k;
		r[1][2] = v[1] * v[2] * k - v[0];
		r[2][0] = v[2] * v[0] * k - v[1];
		r[2][1] = v[2] * v[1] * k + v[0];
		r[2][2] = c + v[2] * v[2] * k;
	}

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			out->m[j][i] = r[i][j]; // column-major
		}

		out->m[i][3] = 0.0f;
	}

	out->m[3][0] = out->m[3][1] = out->m[3][2] = 0.0f;
	out->m[3][3] = 1.0f;
}

// World rotation that points part's bone (towards child) along dir, starting from its current rotation
static void ragdollAimPart(struct ragdoll *rd, s32 part, const f32 *dir, Mtxf *out)
{
	f32 bone[3];
	Mtxf q;

	rdVecSub(rd->world[rd->skel->child[part]].m[3], rd->world[part].m[3], bone);
	rdVecNorm(bone);
	rdRotFromTo(bone, dir, &q);
	rdRotMul(&q, &rd->world[part], out);
}

static void ragdollMotorsOff(struct ragdoll *rd)
{
	if (rd->driven) {
		rd->driven = false;

		if (rd->state == RDSTATE_SIMULATING && rd->handle >= 0) {
			f32 zero[3] = { 0, 0, 0 };

			pdphysRagdollDriveToPose(rd->handle, NULL, NULL, 0.0f);

			// Let whatever the muscles were holding up (the head) drop
			// before the body is allowed to sleep
			pdphysRagdollAddVelocity(rd->handle, -1, zero);
		}
	}
}

#define RD_BRACE_RISE   (1.0f / 0.15f) // reflex reaction: fully braced ~0.15 s after the fall starts
#define RD_BRACE_DECAY  (1.0f / 0.45f) // relaxes over ~0.45 s once down
#define RD_REFLEX_TIME  (4.0f * 60)    // reflexes only in the first seconds after death
#define RD_REFLEX_TIME_PLAYER (6.0f * 60) // the player's body can brace a bit longer
#define RD_HEADGUARD_FULL (2.0f * 60)  // head guard at full strength for 2 s,
#define RD_HEADGUARD_END  (3.0f * 60)  // then fades out by 3 s

/* ---- animation blend ---- */

/*
 * The death animation plays out in full with the physics body following it,
 * until:
 *  - the animation ends
 *  - a headshot (other shots only jolt the part that was hit)
 *  - a part (not the shins: steps) touches anything that isn't a floor under
 *    it: a wall, a ceiling, the side of a prop, a character, another body
 *  - no floor below (a ledge), or a sudden drop
 *  - the body can't keep up with the animation (blocked)
 * Then the animation lets go over RD_BLEND_BREAKTIME60 and the physics takes
 * over with its momentum.
 *
 * The animation was made for a flat floor at the death height: its path
 * follows the floor under the body instead (slopes, stairs, lifts), so it
 * doesn't pull the body into a slope or up off a lift.
 */
#define RD_BLEND_REACH 0.08f      // seconds to close the gap to the animation
#define RD_BLEND_BREAKTIME60 9    // once interrupted, the animation lets go over 0.15 s
#define RD_BLEND_MAXERR 18.0f     // the body is blocked (wall, prop) when it lags the animation this much
#define RD_BLEND_MAXAGE60 (8 * 60) // safety: never longer than this (looping animations)
#define RD_BLEND_AWAKETIME 1.5f   // after the blend the body may not fall asleep for this long (s)
#define RD_BLEND_ENDEARLY 10.0f   // hand over this many animation frames before it ends (smoother, and it doesn't come to rest first)
#define RD_BLEND_FLOORDROP 30.0f  // the floor dropping this much at once is a ledge
#define RD_BLEND_MAXRISE 10.0f    // the floor under it rising this much above where it died ends the blend
#define RD_BLEND_MAXERRLIFT 40.0f // ... on a moving lift (it lags the lift a little when it starts)
#define RD_BLEND_LEGCUT60 64      // a leg cut off: the animation lets go this long after
#define RD_BLEND_KNOCKLAG 0.1f   // s: the knock-back's speed times this is extra lag allowed (still catching up)
#define RD_BLEND_MAXKNOCK 60.0f   // the knock-back moves the animation's path at most this far
#define RD_BLEND_FLOORREACH 60.0f // floor looked for this far below where the animation expects it
#define RD_BLEND_LETGOSPEED 350.0f // units/s: the most a body keeps when the animation blend lets go

static f32 ragdollBlendWeight(struct ragdoll *rd)
{
	f32 w = 1.0f;

	if (rd->blendbreak60 >= 0) {
		w = 1.0f - (rd->age60 - rd->blendbreak60) / (f32) RD_BLEND_BREAKTIME60;

		RD_CLAMPLO(w, 0.0f);
	}

	return w;
}

static bool ragdollBlendAnimEnded(struct ragdoll *rd)
{
	struct model *model = rd->model;
	f32 frame = modelGetCurAnimFrame(model);
	f32 speed = modelGetAnimSpeed(model);

	if (speed < 0.0f) {
		return frame <= RD_BLEND_ENDEARLY;
	}

	return frame >= modelGetAnimEndFrame(model) - RD_BLEND_ENDEARLY;
}

static bool ragdollBlendOnLift(struct ragdoll *rd)
{
	return rd->bliftframe != 0 && g_Vars.lvframe60 - rd->bliftframe <= 3;
}

/**
 * Keeps the animation's path on the floor under the body. False when there's
 * no floor where it should be (a ledge).
 */
static bool ragdollBlendFollowFloor(struct ragdoll *rd)
{
	f32 expect;
	f32 from[3];
	f32 to[3];
	f32 hit[3];
	f32 normal[3];
	s32 room;

	// The floor it died on, measured the same way as the floor it's on now
	// (chr->ground lags behind a moving lift)
	if (!rd->blendfloorset) {
		f32 a[3] = { rd->world[0].m[3][0], rd->world[0].m[3][1], rd->world[0].m[3][2] };
		f32 b[3] = { a[0], a[1] - 300.0f, a[2] };

		rd->blendfloorset = true;

		if (pdphysRagdollRaycast(rd->handle, a, b, hit, normal, &room) && normal[1] >= 0.7f && fabsf(hit[1] - rd->deathground) < 60.0f) {
			rd->blendfloor0 = hit[1];
			rd->blendfloor += hit[1] - rd->deathground;
		}
	}

	// On a moving lift: the floor is the lift's, where the game has it now
	// (the physics lift is a step behind it, and gets there this step)
	if (ragdollBlendOnLift(rd)) {
		rd->offset[1] = rd->blendfloor - rd->blendfloor0;
		return true;
	}

	expect = rd->blendfloor0 + rd->offset[1];
	rdVecCopy(rd->world[0].m[3], from);
	to[0] = from[0];
	to[1] = expect - RD_BLEND_FLOORREACH;
	to[2] = from[2];

	if (to[1] >= from[1]) {
		return true; // the pelvis is down at the floor already
	}

	if (!pdphysRagdollRaycast(rd->handle, from, to, hit, normal, &room) || normal[1] < 0.7f) {
		return false;
	}

	if (hit[1] < expect - RD_BLEND_FLOORDROP) {
		return false;
	}

	// The floor rising under it: pushed onto a slope or a step (the
	// animation's knock-back drove bodies up slopes, and the physics then
	// threw them off). The animation was made for flat ground: let go.
	if (hit[1] > rd->blendfloor + RD_BLEND_MAXRISE) {
		return false;
	}

	rd->offset[1] = hit[1] - rd->blendfloor0;

	return true;
}

/**
 * The death animation keeps playing (with its own movement - stumbles,
 * spins, drops) and the physics body is steered to follow it, while still
 * colliding with the world.
 */
// The animation blend lets go (logged with the reason)
static void rdBlendBreak(struct ragdoll *rd, const char *why, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, why);
	vsnprintf(buf, sizeof(buf), why, ap);
	va_end(ap);

	rd->blendbreak60 = rd->age60;
	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d blend ends at %d: %s", RD_RDCHRNUM(rd), rd->age60, buf);
}

// The shot's knock-back and the lift's movement shift the animation's path
static void rdBlendOffset(struct ragdoll *rd, f32 *pos)
{
	pos[0] = pos[0] + rd->offset[0] + rd->liftoff[0];
	pos[1] = pos[1] + rd->offset[1];
	pos[2] = pos[2] + rd->offset[2] + rd->liftoff[1];
}

static void ragdollBlendTick(struct ragdoll *rd, f32 dt)
{
	RD_CRASHCTX("blendtick", RD_RDCHRNUM(rd), -1);
	Mtxf anim[RD_NUMPARTS];
	f32 targets[PDPHYS_MAX_RAGDOLL_PARTS][16];
	s32 skip[PDPHYS_MAX_RAGDOLL_PARTS];
	s32 blocks[PDPHYS_MAX_RAGDOLL_PARTS];
	f32 w;
	f32 decay;
	s32 part;
	s32 i;

	if (g_RagdollAnimBlend <= 0 || rd->model->anim == NULL || dt <= 0.0f) {
		rd->blending = false;
		return;
	}

	// Touched something that isn't a floor (not the shins: steps and kerbs)
	pdphysRagdollTakeNewBlocks(rd->handle, blocks);

	if (rd->blendbreak60 < 0 && rd->age60 > 2) {
		for (i = 0; i < PDPHYS_MAX_RAGDOLL_PARTS; i++) {
			if (blocks[i] && i != rd->bodyindex[11] && i != rd->bodyindex[12]) {
				f32 ny;
				s32 kind;
				s32 ph;
				struct ragdollprop *rp;
				struct prop *what;

				pdphysRagdollLastBlock(rd->handle, &ny, &kind, &ph);
				rp = ragdollPropByHandle(ph);
				what = rp ? rp->prop : NULL;

				rdBlendBreak(rd, "part %d touched %s (normal y %.2f)", i,
						kind == 0 ? "the level" : kind == 2 ? "another body" : what == NULL ? "a prop"
						: what->type == PROPTYPE_PLAYER ? "the player" : what->type == PROPTYPE_CHR ? "a character"
						: what->type == PROPTYPE_DOOR ? "a door" : "an object", ny);
				break;
			}
		}
	}

	// The floor under it: follow it, or let go over a ledge
	if (rd->blendbreak60 < 0 && !ragdollBlendFollowFloor(rd)) {
		rdBlendBreak(rd, "no floor");
	}

	if (rd->blendbreak60 < 0 && rd->legcut60 >= 0 && rd->age60 - rd->legcut60 >= RD_BLEND_LEGCUT60) {
		rdBlendBreak(rd, "a leg was cut off");
	}

	if (rd->blendbreak60 < 0 && (ragdollBlendAnimEnded(rd) || rd->age60 >= RD_BLEND_MAXAGE60)) {
		rdBlendBreak(rd, "animation over");
	}

	w = ragdollBlendWeight(rd);

	if (w <= 0.0f) {
		rd->blending = false;
		return;
	}

	// Advance the death animation with its root motion. Without the chr's
	// movement callback: that collides the move using the prop's position,
	// which is the body's (the knock-back away from the animation's), and
	// now and then it jumped the whole animation onto the body (seen: 58
	// units in one frame, which ended the blend as blocked and flung the
	// body). The physics does the colliding.
	{
		struct anim *a = rd->model->anim;
		bool (*movecb)(struct model *, struct coord *, struct coord *, f32 *) = a->unk70;

		a->unk70 = NULL;
		modelTickAnimQuarterSpeed(rd->model, g_Vars.lvupdate240, true);
		modelUpdateInfo(rd->model);

		if (rd->model->anim == a) {
			a->unk70 = movecb;
		}
	}

	if (!ragdollSampleAnimPose(rd, anim)) {
		rd->blending = false;
		return;
	}

	// The shot's knock-back moves the whole path
	decay = expf(-3.0f * dt);

	for (i = 0; i < 3; i += 2) {
		rd->offvel[i] *= decay;
		rd->offset[i] += rd->offvel[i] * dt;
	}

	{
		f32 knock = sqrtf(rd->offset[0] * rd->offset[0] + rd->offset[2] * rd->offset[2]);

		if (knock > RD_BLEND_MAXKNOCK) {
			rd->offset[0] *= RD_BLEND_MAXKNOCK / knock;
			rd->offset[2] *= RD_BLEND_MAXKNOCK / knock;
			rd->offvel[0] = rd->offvel[2] = 0.0f;
		}
	}

	// Blocked: the animation's path goes into something (PD's own movement
	// code would have stopped it), or the body can't keep up with it
	if (rd->blendbreak60 < 0 && rd->age60 > 3 && rd->age60 >= rd->twitchuntil60) {
		static const s8 checkparts[] = { 0, 1, 2 };

		for (i = 0; i < ARRAYCOUNT(checkparts) && rd->blendbreak60 < 0; i++) {
			s32 p = checkparts[i];
			f32 from[3];
			f32 to[3];
			f32 hit[3];
			f32 normal[3];
			f32 d[3];
			s32 room;

			rdVecCopy(rd->world[p].m[3], from);
			rdVecCopy(anim[p].m[3], to);
			rdBlendOffset(rd, to);
			rdVecSub(to, from, d);

			// (plus what the body may still be catching up on of the
			// shot's knock-back: a few frames in, a body still getting going
			// counted as blocked and the animation was cut off)
			if (rdVecLen(d) > (ragdollBlendOnLift(rd) ? RD_BLEND_MAXERRLIFT : RD_BLEND_MAXERR)
					+ sqrtf(rd->offvel[0] * rd->offvel[0] + rd->offvel[2] * rd->offvel[2]) * RD_BLEND_KNOCKLAG) {
				// Blocked, or the animation jumped (seen: 570 units in one
				// frame): stop following it now. Fading out towards that
				// target is what launched bodies.
				rdBlendBreak(rd, "part %d blocked (%.0f behind: off %.0f %.0f %.0f, lift %.0f %.0f, floor %.0f/%.0f%s)", p, rdVecLen(d),
						d[0], d[1], d[2], rd->liftoff[0], rd->liftoff[1], rd->blendfloor0 + rd->offset[1], rd->blendfloor0, ragdollBlendOnLift(rd) ? ", on a lift" : "");
				rd->blending = false;
				return;
			}

			// Look a bit ahead along the way it's heading: about to run
			// into a wall
			if (rdVecLen(d) > 0.5f) {
				rdVecNorm(d);
				rdVecMad(from, d, 25.0f, to);

				if (pdphysRagdollRaycast(rd->handle, from, to, hit, normal, &room) && normal[1] < 0.7f) {
					rdBlendBreak(rd, "part %d heading into a wall", p);
				}
			}
		}

		if (rd->blendbreak60 >= 0) {
			w = ragdollBlendWeight(rd);
		}
	}

	memset(skip, 0, sizeof(skip));

	// The knock-back of the animation's path
	for (part = 0; part < rd->numparts; part++) {
		rdBlendOffset(rd, anim[part].m[3]);
	}

	// A shot part: the part and everything hanging off it swing about its
	// joint, rigidly, so its far end moves up to RD_BLEND_TWITCHDIST along the
	// shot, and spring back over RD_BLEND_TWITCH60. (Moving them along the
	// shot instead pushed a hit chest's hands through the back of the body.)
	if (rd->twitchpart > 0 && rd->age60 < rd->twitchuntil60) {
		const s8 *tip = rd->skel->child;
		f32 u = (rd->twitchuntil60 - rd->age60) / (f32) RD_BLEND_TWITCH60;
		s32 tp = rd->twitchpart;
		f32 *pivot = anim[tp].m[3];
		f32 limb[3];
		f32 axis[3];
		f32 len;
		f32 angle;

		if (tip[tp] >= 0) {
			rdVecSub(anim[tip[tp]].m[3], pivot, limb);
		} else {
			// An end part (head, hand, foot): swing it about its parent's joint
			rdVecSub(pivot, anim[rd->skel->parent[tp]].m[3], limb);
			tp = rd->skel->parent[tp];
			pivot = anim[tp].m[3];
		}

		len = rdVecLen(limb);
		rdVecCross(limb, rd->twitchdir, axis);

		if (len > 1.0f && rdVecLen(axis) > 0.05f * len) {
			f32 c, sn;
			f32 pv[3];

			rdVecNorm(axis);
			angle = RD_BLEND_TWITCHDIST / (len * 1.6f); // the far end of what hangs off it is further out
			angle = RD_MIN(angle, RD_BLEND_TWITCHMAXANG) * u * u;
			c = cosf(angle);
			sn = sinf(angle);
			rdVecCopy(pivot, pv);

			for (part = 0; part < rd->numparts; part++) {
				s32 a = part;
				s32 k;

				while (a > 0 && a != tp) {
					a = rd->skel->parent[a];
				}

				if (a != tp) {
					continue;
				}

				// Rotate the position about the pivot and each axis of the
				// orientation (Rodrigues)
				for (k = 0; k < 4; k++) {
					f32 v[3];
					f32 cr[3];
					f32 dot;
					s32 j;

					for (j = 0; j < 3; j++) {
						v[j] = anim[part].m[k][j] - (k == 3 ? pv[j] : 0.0f);
					}

					rdVecCross(axis, v, cr);
					dot = rdVecDot(axis, v);

					for (j = 0; j < 3; j++) {
						anim[part].m[k][j] = v[j] * c + cr[j] * sn + axis[j] * dot * (1.0f - c) + (k == 3 ? pv[j] : 0.0f);
					}
				}
			}
		}
	}

	for (part = 0; part < rd->numparts; part++) {
		s32 b = rd->bodyindex[part];

		if (b >= 0 && b < PDPHYS_MAX_RAGDOLL_PARTS) {
			memcpy(targets[b], &anim[part], sizeof(targets[b]));

			// Cut pieces aren't steered
			skip[b] = ragdollPieceOf(rd, part) != 0;
		}
	}

	pdphysRagdollTrackPose(rd->handle, (const f32 (*)[16]) targets, skip, w, dt, RD_BLEND_REACH);
}

/**
 * Runs before each physics step. Combines:
 *  - muscle tone: advances the death animation invisibly and pulls
 *    every joint towards it, fading over Ragdoll.RagdollMuscleTime;
 *  - reflexes: arms reach out towards the ground the body is
 *    toppling onto (Ragdoll.RagdollReflexBrace), and the neck holds the head
 *    roughly where it was at death (Ragdoll.RagdollReflexHead).
 * Each joint follows whichever source pulls hardest.
 */
// A reflex pulls a part towards want[part] with this torque, if nothing pulls harder
static void rdReflexWant(f32 *torque, bool *reflex, s32 part, f32 tq)
{
	if (tq > torque[part]) {
		torque[part] = tq;
		reflex[part] = true;
	}
}

#define RD_DEATHLOOK_TORQUE 40.0f // N*m: the neck turning the dead player's head (the head guard is 35)

void ragdollDriveJoints(struct ragdoll *rd, f32 dt)
{
	RD_CRASHCTX("drivejoints", RD_RDCHRNUM(rd), -1);
	Mtxf target[RD_NUMPARTS]; // joint targets (only rotations relative to the parent matter)
	Mtxf want[RD_NUMPARTS];   // reflex: desired world rotation
	bool reflex[RD_NUMPARTS];
	f32 torque[RD_NUMPARTS];
	f32 targets[PDPHYS_MAX_RAGDOLL_PARTS][16];
	f32 torques[PDPHYS_MAX_RAGDOLL_PARTS];
	f32 musclestrength = 0.0f;
	f32 reflexstrength = rd->reflexstrength;
	bool any = false;
	s32 rage60; // age for the reflexes: since the animation blend ended
	s32 part;

	if (rd->state != RDSTATE_SIMULATING || rd->handle < 0) {
		rd->driven = false;
		return;
	}

	memcpy(target, rd->world, rd->numparts * sizeof(Mtxf));
	memset(reflex, 0, sizeof(reflex));
	memset(torque, 0, sizeof(torque));

	// Animation blend: the animation owns the pose; muscles and
	// reflexes (bracing, stumbling, clutching) only start once it lets go
	// (they were pulling the arms and legs towards nearby slopes and walls)
	if (rd->blending) {
		ragdollBlendTick(rd, dt);

		if (rd->blending) {
			ragdollMotorsOff(rd);
			return;
		}

		rd->blendend60 = rd->age60;
		rd->stumble = false; // the animation did the stumbling
		rd->clutcharm = -1;  // ... and the reacting: a hand going to the wound only now,
		                     // with the body already down, pulled the arm into the chest

		// It was being steered towards the animation: if something held it
		// back, that steering is all its speed, and it would fly off with it
		pdphysRagdollClampSpeed(rd->handle, ragdollBlendOnLift(rd) ? rd->liftvel : NULL, RD_BLEND_LETGOSPEED);

		// Body stiffness carries on from here if its time since the death
		// hasn't run out
		rd->powered = g_RagdollMuscles && rd->musclestrength > 0.0f && rd->model->anim != NULL
			&& rd->age60 < RD_MAX(rd->muscletime, 0.1f) * 60.0f;
		pdphysRagdollKeepAwake(rd->handle, RD_BLEND_AWAKETIME);
	}

	rage60 = rd->age60 - rd->blendend60;

	if (rd->headguard && (rage60 >= RD_HEADGUARD_END || !g_RagdollReflexHead || !rd->reflexes)) {
		rd->headguard = false;
	}

	// Muscle tone
	if (rd->powered) {
		f32 fadetime = RD_MAX(rd->muscletime, 0.1f);
		f32 t = rd->age60 / (fadetime * 60.0f);

		if (!g_RagdollMuscles || rd->musclestrength <= 0.0f || t >= 1.0f) {
			rd->powered = false;
		} else {
			// Keep the death animation going (no root motion: the physics moves the body)
			modelTickAnimQuarterSpeed(rd->model, g_Vars.lvupdate240, false);

			if (ragdollSampleAnimPose(rd, target)) {
				musclestrength = (1.0f - t) * rd->musclestrength;

				for (part = 0; part < rd->numparts; part++) {
					torque[part] = rd->skel->torque[part] * musclestrength;
				}
			} else {
				rd->powered = false;
				memcpy(target, rd->world, rd->numparts * sizeof(Mtxf));
			}
		}
	}

	// Reflexes
	if (rd->isplayer && reflexstrength < 1.0f) {
		reflexstrength = 1.0f; // the player's own body always reacts
	}

	if (rd->reflexes && (rage60 >= (rd->isplayer ? RD_REFLEX_TIME_PLAYER : RD_REFLEX_TIME) || reflexstrength <= 0.0f)) {
		rd->reflexes = false;
		rd->brace = 0.0f;
	}

	if (rd->reflexes) {
		f32 *pelvis = rd->world[0].m[3];
		f32 *chest = rd->world[1].m[3];
		f32 *head = rd->world[2].m[3];
		f32 up[3];
		f32 vel[3] = {0, 0, 0};
		f32 topple[3];
		f32 hlen;
		f32 headheight = head[1] - rd->chr->ground;
		bool falling;

		if (dt > 0.0f && rage60 > 0) {
			rdVecSub(chest, rd->prevchest, vel);
			rdVecDiv(vel, dt);
		}

		rdVecCopy(chest, rd->prevchest);

		rdVecSub(head, pelvis, up);
		rdVecNorm(up);

		// Which way the body is going over: where the head leans, or if it's
		// still upright, where the chest is moving
		topple[0] = up[0];
		topple[1] = 0.0f;
		topple[2] = up[2];
		hlen = sqrtf(topple[0] * topple[0] + topple[2] * topple[2]);

		if (hlen < 0.25f) {
			f32 hspeed = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);

			if (hspeed > 40.0f) {
				topple[0] = vel[0] / hspeed;
				topple[2] = vel[2] / hspeed;
				hlen = 1.0f;
			}
		} else {
			topple[0] /= hlen;
			topple[2] /= hlen;
		}

		// Going down and not on the floor yet
		// (leaning but not moving, e.g. resting on the knees or against a wall, isn't falling)
		// (any lean while still off the floor counts for the player's body)
		falling = hlen >= 0.25f && headheight > 35.0f
			&& (rd->severed & ((1u << 1) | (1u << 2))) == 0
			&& (vel[1] < -40.0f || (up[1] < 0.85f && rdVecLen(vel) > 40.0f) || (rd->isplayer && up[1] < 0.95f));

		if (rd->canbrace && (g_RagdollReflexBraceChance > 0 || rd->isplayer) && falling) {
			if (rd->brace < 0.5f && rd->brace + RD_BRACE_RISE * dt >= 0.5f) {
				sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d braces for a fall (lean %.0f deg, head %.0f above floor, falling at %.0f u/s)",
						rd->chr->chrnum, acosf(RD_MIN(up[1], 1.0f)) * 57.2958f, headheight, -vel[1]);
			}

			rd->brace += RD_BRACE_RISE * dt;
		} else {
			rd->brace -= RD_BRACE_DECAY * dt;
		}

		RD_CLAMPTO(rd->brace, 0.0f, 1.0f);

		memcpy(want, rd->world, rd->numparts * sizeof(Mtxf));

		// Stumble: shot while running, the legs keep going for a step or two,
		// alternating a forward-reaching leg and a trailing one, until the
		// body leans too far or the steps run out
		if (rd->stumble) {
			f32 stepdur = 0.28f * 60.0f;
			s32 step = (s32)(rage60 / stepdur);

			if (!g_RagdollReflexStumble || (rd->severed & ((1u << 1) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12))) || step >= rd->stumblesteps || up[1] < 0.7f || headheight < 60.0f) {
				rd->stumble = false;
			} else {
				f32 *md = rd->movedir;
				f32 t = reflexstrength;
				s32 leg;

				// Weaker towards the end of the last step
				if (step == rd->stumblesteps - 1) {
					t *= 1.0f - 0.6f * (rage60 - step * stepdur) / stepdur;
				}

				for (leg = 0; leg < 2; leg++) {
					s32 hip = 9 + leg;
					s32 knee = 11 + leg;
					bool front = ((step + rd->stumbleleg + leg) & 1) == 0;
					f32 thigh[3];
					f32 shin[3];

					if (front) {
						thigh[0] = md[0] * 0.55f; thigh[1] = -0.85f; thigh[2] = md[1] * 0.55f;
						shin[0] = md[0] * 0.2f; shin[1] = -1.0f; shin[2] = md[1] * 0.2f;
					} else {
						thigh[0] = -md[0] * 0.35f; thigh[1] = -0.94f; thigh[2] = -md[1] * 0.35f;
						shin[0] = -md[0] * 0.75f; shin[1] = -0.65f; shin[2] = -md[1] * 0.75f;
					}

					rdVecNorm(thigh);
					rdVecNorm(shin);
					ragdollAimPart(rd, hip, thigh, &want[hip]);
					ragdollAimPart(rd, knee, shin, &want[knee]);

					rdReflexWant(torque, reflex, hip, 110.0f * t);
					rdReflexWant(torque, reflex, knee, 60.0f * t);
				}
			}
		}

		// Clutch: a hand goes to the wound (two-bone IK for the arm),
		// from 0.1 s, held until 1.6 s, let go by 2.2 s. The brace below wins
		// when it pulls harder.
		// A cut arm can't clutch, and nothing to clutch on a cut piece
		if (rd->clutcharm >= 0 && (ragdollPieceOf(rd, rd->clutcharm + 4) != ragdollPieceOf(rd, rd->clutchpart)
					|| (rd->severed & ((1u << rd->clutcharm) | (1u << (rd->clutcharm + 2)))))) {
			rd->clutcharm = -1;
		}

		if (rd->clutcharm >= 0) {
			f32 a60 = rage60;
			f32 amount;

			if (a60 < 6.0f) {
				amount = 0.0f;
			} else if (a60 < 18.0f) {
				amount = (a60 - 6.0f) / 12.0f;
			} else if (a60 < 96.0f) {
				amount = 1.0f;
			} else {
				amount = 1.0f - (a60 - 96.0f) / 36.0f;
			}

			if (amount <= 0.0f && a60 >= 96.0f) {
				rd->clutcharm = -1;
			} else if (amount > 0.0f) {
				s32 shoulder = rd->clutcharm;
				s32 elbow = shoulder + 2;
				s32 hand = shoulder + 4;
				s32 sp = rd->clutchpart;
				f32 facing[3] = { rd->world[0].m[2][0], rd->world[0].m[2][1], rd->world[0].m[2][2] };
				f32 spine[3];
				f32 target[3];
				f32 *sh = rd->world[shoulder].m[3];
				f32 torso;
				f32 l1, l2, d, a, h;
				f32 u[3], n[3], pole[3], out[3], elbowpos[3], dir[3], fore[3];
				f32 tmp[3];
				f32 dot;
				s32 k;

				rdVecNorm(facing);
				rdVecSub(rd->world[2].m[3], rd->world[1].m[3], spine);
				torso = rdVecLen(spine);

				if (sp == 0 || sp == 1) {
					f32 f = sp == 1 ? 0.75f : 0.3f;

					// On the front of the torso, not in it: the chest and
					// hips are torso * 0.25 thick, the forearm torso * 0.10
					for (k = 0; k < 3; k++) {
						target[k] = pelvis[k] + (chest[k] - pelvis[k]) * f + facing[k] * torso * 0.4f;
					}
				} else {
					f32 *c = rd->world[rd->skel->child[sp]].m[3];

					for (k = 0; k < 3; k++) {
						target[k] = (rd->world[sp].m[3][k] + c[k]) * 0.5f + facing[k] * torso * 0.15f;
					}
				}

				rdVecSub(rd->world[elbow].m[3], sh, tmp);
				l1 = rdVecLen(tmp);
				rdVecSub(rd->world[hand].m[3], rd->world[elbow].m[3], tmp);
				l2 = rdVecLen(tmp) + torso * 0.12f; // to the palm

				rdVecSub(target, sh, u);
				d = rdVecLen(u);
				rdVecNorm(u);

				RD_CLAMPHI(d, (l1 + l2) * 0.98f);
				RD_CLAMPLO(d, fabsf(l1 - l2) + 1.0f);

				a = (l1 * l1 - l2 * l2 + d * d) / (2.0f * d);
				h = l1 * l1 - a * a;
				h = h > 0.0f ? sqrtf(h) : 0.0f;

				// Elbow points out, down and back
				rdVecSub(sh, chest, out);
				out[1] = 0.0f;
				rdVecNorm(out);

				for (k = 0; k < 3; k++) {
					pole[k] = out[k] * 0.6f - facing[k] * 0.3f;
				}

				pole[1] -= 0.6f;
				dot = rdVecDot(pole, u);

				for (k = 0; k < 3; k++) {
					n[k] = pole[k] - u[k] * dot;
				}

				rdVecNorm(n);

				for (k = 0; k < 3; k++) {
					elbowpos[k] = sh[k] + u[k] * a + n[k] * h;
					dir[k] = elbowpos[k] - sh[k];
					fore[k] = sh[k] + u[k] * d - elbowpos[k];
				}

				rdVecNorm(dir);
				rdVecNorm(fore);

				ragdollAimPart(rd, shoulder, dir, &want[shoulder]);
				ragdollAimPart(rd, elbow, fore, &want[elbow]);

				rdReflexWant(torque, reflex, shoulder, 45.0f * amount * reflexstrength);
				rdReflexWant(torque, reflex, elbow, 30.0f * amount * reflexstrength);
			}
		}

		// Brace: upper arms and forearms reach for the floor in the direction
		// of the fall, slightly spread. Falling backwards, they go out to the
		// sides and down instead (shoulders can't reach far behind).
		if (rd->brace > 0.0f && hlen >= 0.25f) {
			f32 facing[3] = { rd->world[0].m[2][0], 0.0f, rd->world[0].m[2][2] };
			f32 backwards;
			s32 side;

			rdVecNorm(facing);
			backwards = -(topple[0] * facing[0] + topple[2] * facing[2]);

			for (side = 0; side < 2; side++) {
				s32 shoulder = 3 + side;
				s32 elbow = 5 + side;
				f32 out[3];
				f32 dir[3];
				f32 fore[3];
				f32 t = rd->brace * reflexstrength;

				if (rd->severed & ((1u << shoulder) | (1u << elbow))) {
					continue; // nothing to brace with
				}

				rdVecSub(rd->world[shoulder].m[3], chest, out);
				out[1] = 0.0f;
				rdVecNorm(out);

				if (backwards > 0.3f) {
					dir[0] = out[0] * 0.6f + topple[0] * 0.3f;
					dir[1] = -0.7f;
					dir[2] = out[2] * 0.6f + topple[2] * 0.3f;
				} else {
					dir[0] = topple[0] * 0.65f + out[0] * 0.2f;
					dir[1] = -0.75f;
					dir[2] = topple[2] * 0.65f + out[2] * 0.2f;
				}

				rdVecNorm(dir);

				// Forearm a little lower, so the palms meet the floor
				fore[0] = dir[0];
				fore[1] = dir[1] - 0.15f;
				fore[2] = dir[2];
				rdVecNorm(fore);

				ragdollAimPart(rd, shoulder, dir, &want[shoulder]);
				ragdollAimPart(rd, elbow, fore, &want[elbow]);

				rdReflexWant(torque, reflex, shoulder, 60.0f * t);
				rdReflexWant(torque, reflex, elbow, 35.0f * t);
			}
		}

		// Head guard: the neck holds the head where it was relative to the
		// chest at death, so it doesn't snap back or bounce off the floor
		if (g_RagdollReflexHead && rd->headguard && (rd->severed & (1u << 2)) == 0) {
			f32 t = rage60 <= RD_HEADGUARD_FULL ? 1.0f
				: 1.0f - (rage60 - RD_HEADGUARD_FULL) / (f32)(RD_HEADGUARD_END - RD_HEADGUARD_FULL);
			f32 tq = 35.0f * t * reflexstrength;

			if (tq > torque[2]) {
				rdRotMul(&rd->world[1], &rd->headrel, &want[2]);
				torque[2] = tq;
				reflex[2] = true;
			}
		}

		// Reflex targets are world rotations: express them relative to the
		// parent's target so they come out right whatever drives the parent
		for (part = 1; part < rd->numparts; part++) {
			if (reflex[part]) {
				s32 p = rd->skel->parent[part];
				Mtxf rel;

				rdRotTransposeMul(&want[p], &want[part], &rel);
				rdRotMul(&target[p], &rel, &target[part]);
				target[part].m[3][0] = target[part].m[3][1] = target[part].m[3][2] = 0.0f;
				target[part].m[3][3] = 1.0f;
			}
		}
	}

	// MOD: death camera control - the dead player looking around: the neck
	// turns the head from where it rests on the chest (yaw about the spine,
	// nod about the chest's side axis)
	if (rd->isplayer && (rd->lookyaw != 0.0f || rd->lookpitch != 0.0f)
			&& (rd->severed & ((1u << 1) | (1u << 2))) == 0 && rd->numparts > 2) {
		Mtxf ry, rx, r, rel;
		// (the neck's share: the view turns the rest, ragdollGetDeathCam)
		f32 yaw = RD_CLAMP(rd->lookyaw, -RD_DEATHLOOK_YAW, RD_DEATHLOOK_YAW);
		f32 pitch = RD_CLAMP(rd->lookpitch, -RD_DEATHLOOK_PITCH, RD_DEATHLOOK_PITCH);
		f32 cy = cosf(yaw), sy = sinf(yaw);
		f32 cp = cosf(pitch), sp = sinf(pitch);

		memset(&ry, 0, sizeof(ry));
		memset(&rx, 0, sizeof(rx));

		// Columns are the turned axes (as rdRotApply reads them)
		ry.m[0][0] = cy;  ry.m[0][2] = -sy;
		ry.m[1][1] = 1.0f;
		ry.m[2][0] = sy;  ry.m[2][2] = cy;
		ry.m[3][3] = 1.0f;

		rx.m[0][0] = 1.0f;
		rx.m[1][1] = cp;  rx.m[1][2] = sp;
		rx.m[2][1] = -sp; rx.m[2][2] = cp;
		rx.m[3][3] = 1.0f;

		rdRotMul(&ry, &rx, &r);
		rdRotMul(&r, &rd->headrel, &rel);
		rdRotMul(&target[1], &rel, &target[2]);
		target[2].m[3][0] = target[2].m[3][1] = target[2].m[3][2] = 0.0f;
		target[2].m[3][3] = 1.0f;

		if (torque[2] < RD_DEATHLOOK_TORQUE) {
			torque[2] = RD_DEATHLOOK_TORQUE;
		}
	}

	memset(torques, 0, sizeof(torques));

	for (part = 0; part < rd->numparts; part++) {
		s32 b = rd->bodyindex[part];

		if (b >= 0 && b < PDPHYS_MAX_RAGDOLL_PARTS) {
			memcpy(targets[b], &target[part], sizeof(targets[b]));
			torques[b] = torque[part];

			if (torque[part] > 0.0f) {
				any = true;
			}
		}
	}

	if (!any) {
		ragdollMotorsOff(rd);
		return;
	}

	pdphysRagdollDriveToPose(rd->handle, (const f32 (*)[16]) targets, torques, RD_MUSCLE_FREQ);
	rd->driven = true;
}

#endif // PD_PHYSICS
