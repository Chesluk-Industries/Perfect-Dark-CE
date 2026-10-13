/**
 * MOD: ragdoll physics - grabbing bodies and objects (hold Use). Part of the
 * ragdoll module (port/src/ragdoll.c).
 */

#include "rd_internal.h"
#include "game/options.h"
#include "game/bondgun.h"

#ifdef PD_PHYSICS

/**
 * Grabbing bodies (Skyrim/Fallout style). Hold Use (E, or B on a pad) while
 * aiming at a body within reach: the part under the crosshair is pulled to
 * a point in front of the camera by a spring that also carries the part's
 * own weight, with the force capped (Ragdoll.RagdollGrabStrength × 700 N, about
 * a body's weight). Limbs follow easily; lifting a whole body by the head is
 * slow and saggy. Let go to drop it, keeping its momentum.
 */
#define RD_GRAB_REACH       220.0f  // how far away a body can be grabbed (units)
#define RD_GRAB_MINDIST     45.0f
#define RD_GRAB_AIMRADIUS   14.0f   // aiming this close to a body part is enough
#define RD_GRAB_TRYTICKS    45      // a grab is only attempted this long into a press
#define RD_GRAB_FORCE       700.0f  // newtons at strength 1
#define RD_GRAB_FREQ        5.0f    // spring stiffness (Hz)
#define RD_GRAB_BREAKDIST   140.0f  // part this far from where it's held for 1 s: let go

struct rdgrab {
	bool active;
	s32 rdindex;
	u32 serial;
	s32 part;      // physics part
	f32 dist;      // distance from the camera it's held at
	s32 held60;    // ticks Use has been held
	s32 stuck60;   // ticks the part has been far from the hold point
};

// Per player (co-op: each player grabs with their own Use)
static struct rdgrab g_RdGrabs[MAX_PLAYERS];

// Another player is holding this body too
static bool rdBodyHeldByOther(s32 pn, s32 rdindex, u32 serial)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (i != pn && g_RdGrabs[i].active && g_RdGrabs[i].rdindex == rdindex && g_RdGrabs[i].serial == serial) {
			return true;
		}
	}

	return false;
}

static void ragdollGrabRelease(s32 pn, const char *why)
{
	struct rdgrab *gr = &g_RdGrabs[pn];

	if (gr->active) {
		struct ragdoll *rd = &g_Ragdolls[gr->rdindex];

		if (rd->serial == gr->serial && !rdBodyHeldByOther(pn, gr->rdindex, gr->serial)) {
			rd->grabbed = false;
			sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d let go (%s)", RD_RDCHRNUM(rd), why);
		}
	}

	gr->active = false;
}

/**
 * Moving objects: hold Use while aiming at something the game lets the
 * player push (OBJFLAG3_PUSHABLE: crates, chairs, hover props) and it's
 * moved along as if pushed, staying where it was picked up relative to the
 * view. It slides with the game's own collision (the projectile sliding
 * code), but without the friction and the speed cap that make pushed
 * things slow and heavy; let go and it coasts to a stop as normal.
 */
#define RD_OBJGRAB_MINDIST  60.0f
#define RD_OBJGRAB_MAXSPEED 14.0f // units per tick
#define RD_OBJGRAB_SPRING   0.35f // fraction of the gap closed per tick

static struct rdobjgrab g_RdObjGrabs[MAX_PLAYERS];

static struct projectile *rdObjProjectile(struct defaultobj *obj)
{
	if (obj->hidden & OBJHFLAG_EMBEDDED) {
		return obj->embedment ? obj->embedment->projectile : NULL;
	}

	return (obj->hidden & OBJHFLAG_PROJECTILE) ? obj->projectile : NULL;
}

bool rdObjGrabbable(struct prop *prop)
{
	struct defaultobj *obj;

	if (prop == NULL || prop->type != PROPTYPE_OBJ || (obj = prop->obj) == NULL || prop->parent) {
		return false;
	}

	if ((obj->flags3 & OBJFLAG3_PUSHABLE) == 0
			|| (obj->hidden & (OBJHFLAG_MOUNTED | OBJHFLAG_GRABBED | OBJHFLAG_HASOWNER | OBJHFLAG_EMBEDDED))
			|| (obj->hidden2 & OBJH2FLAG_DESTROYED)) {
		return false;
	}

	return true;
}

// New stage: the held objects are gone with the old one
void rdObjGrabReset(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_RdObjGrabs[i].active = false;
		g_RdObjGrabs[i].prop = NULL;
	}
}

// A player is holding this object (bodies running into it don't push it)
bool rdObjIsHeld(struct prop *prop)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (g_RdObjGrabs[i].active && g_RdObjGrabs[i].prop == prop) {
			return true;
		}
	}

	return false;
}

static void ragdollObjGrabRelease(s32 pn, const char *why)
{
	struct rdobjgrab *og = &g_RdObjGrabs[pn];

	if (og->active) {
		struct prop *prop = og->prop;

		if (prop && prop->obj && prop->type == PROPTYPE_OBJ) {
			struct coord zero = { 0, 0, 0 };

			// Back to the game's normal friction (keeps the current speed)
			objApplyMomentum(prop->obj, &zero, 0.0f, true, true);
			sysLogPrintf(LOG_NOTE, "PHYSICS: object %d let go (%s)", (s32) (prop - g_Vars.props), why);
		}
	}

	og->active = false;
	og->prop = NULL;
}

// Distance along the ray to the object's box, or -1
static f32 rdRayObjBox(struct defaultobj *obj, const f32 *from, const f32 *dir, f32 maxdist)
{
	f32 c[8][3];
	f32 tmin = 0.0f, tmax = maxdist;
	s32 axis, k;

	if (!rdObjBboxCorners(obj, c)) {
		return -1.0f;
	}

	for (axis = 0; axis < 3; axis++) {
		f32 e[3], len2, o, d;
		const s32 other = 1 << axis; // corner 1 = +x, 2 = +y, 4 = +z

		for (k = 0; k < 3; k++) {
			e[k] = c[other][k] - c[0][k];
		}

		len2 = rdVecDot(e, e);

		if (len2 < 1e-6f) {
			return -1.0f;
		}

		// Ray in units of this edge (0..1 inside)
		o = ((from[0] - c[0][0]) * e[0] + (from[1] - c[0][1]) * e[1] + (from[2] - c[0][2]) * e[2]) / len2;
		d = (rdVecDot(dir, e)) / len2;

		if (fabsf(d) < 1e-9f) {
			if (o < 0.0f || o > 1.0f) {
				return -1.0f;
			}
		} else {
			f32 t1 = (0.0f - o) / d;
			f32 t2 = (1.0f - o) / d;

			if (t1 > t2) {
				f32 tmp = t1;
				t1 = t2;
				t2 = tmp;
			}

			tmin = fmaxf(tmin, t1);
			tmax = fminf(tmax, t2);

			if (tmin > tmax) {
				return -1.0f;
			}
		}
	}

	return tmin;
}

// The pushable object under the crosshair within reach, and how far along the ray
static struct prop *rdFindGrabObject(const f32 *eye, const f32 *dir, f32 *outdist)
{
	struct prop *best = NULL;
	f32 bestdist = RD_GRAB_REACH;
	struct prop *prop;

	for (prop = g_Vars.activeprops; prop && prop != g_Vars.pausedprops; prop = prop->next) {
		f32 t;

		if (!rdObjGrabbable(prop)) {
			continue;
		}

		if (rdDistSqCoord(eye, &prop->pos) > RD_SQ(RD_GRAB_REACH + 200.0f)) {
			continue;
		}

		t = rdRayObjBox(prop->obj, eye, dir, bestdist);

		if (t >= 0.0f && t < bestdist) {
			best = prop;
			bestdist = t;
		}
	}

	if (best) {
		// Not through a wall
		f32 to[3], hit[3], n[3];

		to[0] = eye[0] + dir[0] * (bestdist - 2.0f);
		to[1] = eye[1] + dir[1] * (bestdist - 2.0f);
		to[2] = eye[2] + dir[2] * (bestdist - 2.0f);

		if (bestdist > 2.0f && pdphysRaycastLevelGroups(eye, to, (1u << RD_GROUP_FLOORS) | (1u << RD_GROUP_WALLS) | (1u << RD_GROUP_RENDER), hit, n)) {
			return NULL;
		}
	}

	*outdist = bestdist;
	return best;
}

// World position of a point given in the object's frame (as rdObjBboxCorners)
static void rdObjLocalToWorld(struct defaultobj *obj, const f32 *l, f32 *out)
{
	s32 k;

	for (k = 0; k < 3; k++) {
		out[k] = obj->prop->pos.f[k] + obj->realrot[0][k] * l[0] + obj->realrot[1][k] * l[1] + obj->realrot[2][k] * l[2];
	}
}

static void rdObjWorldToLocal(struct defaultobj *obj, const f32 *w, f32 *out)
{
	f32 d[3];
	s32 i;

	d[0] = w[0] - obj->prop->pos.x;
	d[1] = w[1] - obj->prop->pos.y;
	d[2] = w[2] - obj->prop->pos.z;

	// realrot rows are orthogonal (rotation x scale)
	for (i = 0; i < 3; i++) {
		f32 len2 = rdVecDot(obj->realrot[i], obj->realrot[i]);

		out[i] = len2 > 1e-6f ? (rdVecDot(obj->realrot[i], d)) / len2 : 0.0f;
	}
}

#define RD_GRAB_TURNSIGN    (-1.0f) // PD's spin (projectile unk0dc) turns the other way from a +y cross product
#define RD_OBJGRAB_TURN     0.5f   // how much of the sideways gap at the grabbed point is closed by turning
#define RD_OBJGRAB_MAXTURN  0.12f  // radians per tick

static void ragdollObjGrabHold(s32 pn, const f32 *eye, const f32 *dir)
{
	struct rdobjgrab *og = &g_RdObjGrabs[pn];
	struct prop *prop = og->prop;
	struct defaultobj *obj;
	struct projectile *projectile;
	struct coord speed;
	f32 target[3], point[3], gap[2], r[2], len, r2, turn = 0.0f;

	if (!rdObjGrabbable(prop)) {
		ragdollObjGrabRelease(pn, "object gone");
		return;
	}

	obj = prop->obj;

	// The grabbed point is pulled to the hold point, like a hand on that
	// corner: the part of the pull across the lever turns the object, the
	// rest moves it
	target[0] = eye[0] + dir[0] * og->dist;
	target[2] = eye[2] + dir[2] * og->dist;
	rdObjLocalToWorld(obj, og->local, point);
	gap[0] = target[0] - point[0];
	gap[1] = target[2] - point[2];
	r[0] = point[0] - prop->pos.x;
	r[1] = point[2] - prop->pos.z;
	r2 = r[0] * r[0] + r[1] * r[1];
	len = sqrtf(gap[0] * gap[0] + gap[1] * gap[1]);

	if (len > RD_GRAB_REACH + 100.0f) {
		ragdollObjGrabRelease(pn, "snagged");
		return;
	}

	speed.x = gap[0] * RD_OBJGRAB_SPRING;
	speed.y = 0.0f;
	speed.z = gap[1] * RD_OBJGRAB_SPRING;

	if (r2 > 25.0f * 25.0f) {
		// Sideways part of the gap (perpendicular to the lever) turns it...
		f32 across = (r[0] * gap[1] - r[1] * gap[0]) / sqrtf(r2);
		f32 rlen = sqrtf(r2);

		turn = RD_GRAB_TURNSIGN * across * RD_OBJGRAB_SPRING * RD_OBJGRAB_TURN / rlen;

		RD_CLAMPTO(turn, -RD_OBJGRAB_MAXTURN, RD_OBJGRAB_MAXTURN);

		// ... and the rest moves it
		speed.x -= RD_OBJGRAB_TURN * RD_OBJGRAB_SPRING * across * (-r[1] / rlen);
		speed.z -= RD_OBJGRAB_TURN * RD_OBJGRAB_SPRING * across * (r[0] / rlen);
	}

	len = sqrtf(speed.x * speed.x + speed.z * speed.z);

	if (len > RD_OBJGRAB_MAXSPEED) {
		speed.x *= RD_OBJGRAB_MAXSPEED / len;
		speed.z *= RD_OBJGRAB_MAXSPEED / len;
	}

	// Set the sliding speed and spin outright, then take the brakes off
	objApplyMomentum(obj, &speed, turn, false, false);
	projectile = rdObjProjectile(obj);

	if (projectile) {
		projectile->unk098 = 0.0f;    // friction
		projectile->unk0e4 = 1.0f;    // no extra drag above unk0f0 / unk0ec
		projectile->unk0f0 = 1000.0f;
		projectile->unk0e0 = 0.0f;    // spin friction
		projectile->unk0ec = 1.0f;    // spin cap
	}
}

// The current player is holding a body or an object
s32 ragdollIsHolding(void)
{
	s32 pn = g_Vars.currentplayernum;

	if (pn < 0 || pn >= MAX_PLAYERS) {
		return false;
	}

	return g_RdGrabs[pn].active || g_RdObjGrabs[pn].active;
}

// Something Use would operate (a button, a terminal, a door) is in front:
// Use is for that, not for grabbing
static bool rdUseHasTarget(s32 pn)
{
	s32 prevplayernum = g_Vars.currentplayernum;
	struct prop *prop;

	setCurrentPlayerNum(pn);
	prop = propFindForInteract(false);
	setCurrentPlayerNum(prevplayernum);

	return prop != NULL;
}

static bool ragdollObjGrabTry(s32 pn, const f32 *eye, const f32 *dir, f32 bodydist)
{
	struct rdobjgrab *og = &g_RdObjGrabs[pn];
	f32 dist;
	struct prop *prop = rdFindGrabObject(eye, dir, &dist);

	if (prop == NULL || (bodydist >= 0.0f && bodydist < dist) || rdObjIsHeld(prop)) {
		return false;
	}

	{
		f32 hit[3];

		rdVecMad(eye, dir, dist, hit);
		rdObjWorldToLocal(prop->obj, hit, og->local);
	}

	og->active = true;
	og->prop = prop;
	og->dist = RD_MAX(dist, RD_OBJGRAB_MINDIST);
	sysLogPrintf(LOG_NOTE, "PHYSICS: player %d picked up object %d (type %d) at %.0f units", pn, (s32) (prop - g_Vars.props), prop->obj->type, dist);

	return true;
}

// Lets go of whatever this player holds, and the press is over
static void rdGrabDrop(s32 pn, const char *why)
{
	ragdollGrabRelease(pn, why);
	ragdollObjGrabRelease(pn, why);
	g_RdGrabs[pn].held60 = 0;
}

static bool rdGrabAim(struct player *player, f32 *eye, f32 *dir)
{
	// The view's own direction: this runs at the end of the tick, when the
	// camera matrices (crosshair projection) aren't set up for this player
	rdCoordToVec(&player->cam_pos, eye);
	rdCoordToVec(&player->cam_look, dir);

	if (rdVecLen(dir) < 0.5f) {
		return false;
	}

	rdVecNorm(dir);
	return true;
}

static void ragdollGrabTickPlayer(s32 pn)
{
	struct player *player = g_Vars.players[pn];
	struct rdgrab *gr = &g_RdGrabs[pn];
	struct rdobjgrab *og = &g_RdObjGrabs[pn];
	f32 eye[3], dir[3];
	bool held;
	s32 i;

	if (!g_RagdollGrab || g_Vars.normmplayerisrunning || player == NULL || player->prop == NULL
			|| player->isdead || player->bondmovemode == MOVEMODE_CUTSCENE || g_Vars.tickmode == TICKMODE_CUTSCENE) {
		rdGrabDrop(pn, "not available");
		return;
	}

	// This player's controller (co-op: the second player's own pad). Use is
	// B, and in the Ext control style A too ("Use / Accept", modern pads)
	{
		s32 contpad = optionsGetContpadNum1(g_Vars.playerstats[pn].mpindex);

		held = inputButtonPressed(contpad, B_BUTTON) != 0
			|| (optionsGetControlMode(g_Vars.playerstats[pn].mpindex) == CONTROLMODE_PC
				&& inputButtonPressed(contpad, BUTTON_ACCEPT_USE) != 0);
	}

	if (!held) {
		rdGrabDrop(pn, "released");
		return;
	}

	gr->held60 += g_Vars.lvupdate60;

	// Holding something: this press of Use is for that, so it mustn't
	// switch the gun's function (classic controls, Use held) or reload /
	// activate when it's let go
	if (gr->active || og->active) {
		player->usedowntime = -1;
	}

	if (!rdGrabAim(player, eye, dir)) {
		return;
	}

	if (og->active) {
		ragdollObjGrabHold(pn, eye, dir);
		return;
	}

	if (!gr->active) {
		struct ragdoll *rd;
		f32 to[3], hit[3];
		s32 handle = -1, part = -1;
		f32 bodydist = -1.0f;

		if (gr->held60 > RD_GRAB_TRYTICKS) {
			return; // only right after pressing, so holding Use while walking past bodies doesn't pick them up
		}

		// Frozen bodies near the player aren't in the physics world: wake them so they can be hit
		if (gr->held60 <= g_Vars.lvupdate60) {
			for (i = 0; i < RD_MAXRAGDOLLS; i++) {
				struct ragdoll *rd = &g_Ragdolls[i];

				if (rd->state == RDSTATE_FROZEN && !rd->isplayer && rd->prop) {
					f32 d[3];

					rdCoordToVec(&rd->prop->pos, d);
					rdVecSub(d, eye, d);

					if (rdVecLenSq(d) < RD_SQ(RD_GRAB_REACH + 100.0f)) {
						rd->riding = NULL;
						ragdollWakeFrozen(rd);
					}
				}
			}

			return; // their bodies exist from the next step
		}

		// A button or door in front: Use is for that
		if (rdUseHasTarget(pn)) {
			return;
		}

		rdVecMad(eye, dir, RD_GRAB_REACH, to);

		// (level geometry blocks it, the player's own collision cylinder doesn't;
		// near misses within RD_GRAB_AIMRADIUS count)
		if (g_RagdollNumUsed > 0 && pdphysRaycastRagdoll(eye, to, RD_GRAB_AIMRADIUS, hit, &handle, &part)) {
			f32 d[3];

			rdVecSub(hit, eye, d);
			bodydist = rdVecLen(d);
		}

		// A pushable object in front of the body (or no body at all)
		if (ragdollObjGrabTry(pn, eye, dir, bodydist)) {
			player->usedowntime = -1;
			return;
		}

		if (bodydist < 0.0f) {
			return;
		}

		rd = ragdollFindByHandle(handle);

		if (rd && !rd->isplayer && !rd->fellout) {
			gr->active = true;
			gr->rdindex = rd - g_Ragdolls;
			gr->serial = rd->serial;
			gr->part = part;
			gr->dist = RD_MAX(bodydist, RD_GRAB_MINDIST);
			gr->stuck60 = 0;

			rd->grabbed = true;
			rd->riding = NULL;
			rd->powered = false;
			rd->blending = false;
			rd->reflexes = false;
			rd->settled = true; // it's a corpse being handled now, not a death in progress
			player->usedowntime = -1;
			sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d grabbed by player %d (physics part %d) at %.0f units", RD_RDCHRNUM(rd), pn, part, gr->dist);
		}

		return;
	}

	// Holding
	{
		struct ragdoll *rd = &g_Ragdolls[gr->rdindex];
		f32 target[3], pos[3], d[3];

		if (rd->serial != gr->serial || rd->state != RDSTATE_SIMULATING || rd->fellout) {
			ragdollGrabRelease(pn, "body gone");
			return;
		}

		rdVecMad(eye, dir, gr->dist, target);

		pdphysRagdollPull(rd->handle, gr->part, target, RD_GRAB_FORCE * g_RagdollGrabStrength, RD_GRAB_FREQ);

		rdVecCopy(target, pos);
		pdphysRagdollGetPartPos(rd->handle, gr->part, pos);
		rdVecSub(pos, target, d);

		if (rdVecLen(d) > RD_GRAB_BREAKDIST) {
			if ((gr->stuck60 += g_Vars.lvupdate60) > 60) {
				ragdollGrabRelease(pn, "snagged");
			}
		} else {
			gr->stuck60 = 0;
		}
	}
}

/**
 * Bodies as cover (Ragdoll.RagdollBodyCover): a character's shot that would hit
 * a player holding a body hits the body instead if it's in the way. The game
 * aims at the eyes, so the spot is picked at random between his knees and his
 * eyes: the more of him the body covers, the more shots it stops. The body
 * takes the shot like any other; the player takes nothing.
 */
#define RD_COVER_RADIUS 3.0f // near misses this close to a part still hit it

s32 ragdollBodyBlocksShot(struct chrdata *shooter, struct prop *targetprop, struct coord *gunpos,
		struct coord *aimpos, struct coord *dir, struct gset *gset, struct coord *outhit)
{
	RD_CRASHCTX("bodycover", shooter ? shooter->chrnum : -1, -1);
	static s32 lastlog60 = 0;
	struct rdgrab *gr;
	struct ragdoll *rd;
	struct coord hitpos;
	f32 from[3], to[3], hit[3];
	s32 handle, part, pn, hp;
	s32 hitpart = HITPART_TORSO;

	if (!g_RagdollsEnabled || !g_RagdollBodyCover || !g_RagdollGrab || targetprop == NULL
			|| targetprop->type != PROPTYPE_PLAYER || g_RagdollNumUsed == 0 || !pdphysIsReady()) {
		return false;
	}

	pn = playermgrGetPlayerNumByProp(targetprop);

	if (pn < 0 || pn >= MAX_PLAYERS || !(gr = &g_RdGrabs[pn])->active) {
		return false;
	}

	rd = &g_Ragdolls[gr->rdindex];

	if (rd->serial != gr->serial || rd->state != RDSTATE_SIMULATING || rd->chr == NULL) {
		return false;
	}

	rdCoordToVec(gunpos, from);
	rdCoordToVec(aimpos, to);

	{
		struct player *player = g_Vars.players[pn];
		f32 knees = player->vv_manground + 40.0f;
		f32 eyes = player->cam_pos.y;

		if (eyes > knees) {
			static u32 seed = 0x2545f491u; // its own random numbers, as rdRollRange

			seed ^= seed << 13;
			seed ^= seed >> 17;
			seed ^= seed << 5;
			to[1] = knees + (eyes - knees) * ((seed & 0xffff) / 65535.0f);
		}
	}

	if (!pdphysRaycastRagdoll(from, to, RD_COVER_RADIUS, hit, &handle, &part) || handle != rd->handle) {
		return false;
	}

	for (hp = 0; hp < 20; hp++) {
		if (rd->hitpartbody[hp] == part) {
			hitpart = hp;
			break;
		}
	}

	rdVecToCoord(hit, &hitpos);

	ragdollOnChrShot(rd->chr, hitpart, dir, gset);
	bgunPlayPropHitSound(gset, rd->prop, -1);
	chrEmitSparks(rd->chr, rd->prop, hitpart, &hitpos, dir, shooter);

	if (lastlog60 == 0 || g_Vars.lvframe60 - lastlog60 >= 60 || g_Vars.lvframe60 < lastlog60) {
		lastlog60 = RD_FRAMESTAMP();
		sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d's body (held by player %d) stopped a shot from chr %d (hit part %d)",
				RD_RDCHRNUM(rd), pn, shooter ? shooter->chrnum : -1, hitpart);
	}

	if (outhit) {
		*outhit = hitpos;
	}

	return true;
}

void ragdollGrabTick(void)
{
	RD_CRASHCTX("grab", -1, -1);
	s32 pn;

	for (pn = 0; pn < MAX_PLAYERS; pn++) {
		if (pn < PLAYERCOUNT()) {
			ragdollGrabTickPlayer(pn);
		} else {
			rdGrabDrop(pn, "no player");
		}
	}
}

#endif // PD_PHYSICS
