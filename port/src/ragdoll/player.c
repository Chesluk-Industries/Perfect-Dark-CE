/**
 * MOD: ragdoll physics - the player's own body and the death camera. Part of
 * the ragdoll module (port/src/ragdoll.c).
 */

#include "rd_internal.h"
#include "lib/joy.h"
#include "game/options.h"
#include "game/menu.h"
#include "lib/vi.h"

#ifdef PD_PHYSICS

struct rdplayercam g_RdPlayerCam[MAX_PLAYERS];

struct rdplayergun g_RdPlayerGuns[MAX_PLAYERS];

/* ---- ragdoll death camera ---- */

#define RD_DEATHCAM_CLEAR 6.0f       // keep the eyes this far in front of walls
#define RD_DEATHCAM_FLOORCLEAR 10.0f  // ... and this high above floors

// MOD: death camera control (Ragdoll.RagdollDeathCamControl)
#define RD_DEATHTWITCH_CHEST    150.0f // a move key pressed: the chest jerks this way (units/s) ...
#define RD_DEATHTWITCH_BODY     20.0f  // ... the whole body a little ...
#define RD_DEATHTWITCH_LIFT     30.0f  // ... and the chest up a little (off the floor's grip)
#define RD_DEATHTWITCH_MAXSPEED 40.0f  // no twitch while the chest is still moving this fast that way
#define RD_DEATHTWITCH_GAP60    30     // ticks between twitches (holding a key does nothing more)
// Looking around turns the head as fast as the same input turns the living
// player (bondmove.c: 3.5 degrees per 60th per unit of turn speed, the mouse's
// scaled movement or the look stick squared), so the player's mouse speed and
// stick sensitivity apply
#define RD_DEATHLOOK_RATE      (3.5f * (M_PI / 180.0f)) // radians per 60th per unit

// v turned about a unit axis by angle (radians, right-handed), in place
static void rdRotateAxis(f32 *v, const f32 *axis, f32 angle)
{
	f32 c = cosf(angle), s = sinf(angle);
	f32 cross[3];
	f32 dot = rdVecDot(axis, v);
	s32 i;

	rdVecCross(axis, v, cross);

	for (i = 0; i < 3; i++) {
		v[i] = v[i] * c + cross[i] * s + axis[i] * dot * (1.0f - c);
	}
}

/**
 * MOD: death camera control - while the player's ragdoll is down, pressing
 * a move key (or pushing the left stick) makes it twitch that way at the
 * chest, along the view, and looking around (mouse, or right stick) turns its
 * head, which the camera rides in.
 */
static void rdPlayerControlTick(struct rdplayercam *pc)
{
	struct ragdoll *rd = ragdollFindPlayerRagdoll(g_Vars.currentplayernum);
	f32 dt = g_Vars.lvupdate60freal / 60.0f;
	s32 contpad;
	u32 buttons;
	s32 movex, movey, lookx, looky;
	bool extstyle;
	f32 sx, sy, fwd, side, look[2] = {0, 0};
	f32 fovscale = viGetFovY() / PLAYER_DEFAULT_FOV;
	f32 *chest;
	bool active = false;

	if (rd == NULL || !g_RagdollDeathCamControl || dt <= 0.0f || rd->handle < 0
			|| rd->state != RDSTATE_SIMULATING || currentPlayerIsMenuOpenInSoloOrMp()) {
		pc->nudgeprevset = false;
		return;
	}

	chest = rd->world[1].m[3];
	contpad = optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex);
	buttons = joyGetButtons(contpad, 0xffffffff);

	// The sticks as the game uses them: in the Ext control style the stick
	// looks and the second stick moves; in the N64 styles the stick moves
	// (twitches) and the second stick looks
	extstyle = optionsGetControlMode(g_Vars.currentplayerstats->mpindex) == CONTROLMODE_PC;

	if (extstyle) {
		movex = joyGetRStickX(contpad);
		movey = joyGetRStickY(contpad);
		lookx = joyGetStickX(contpad);
		looky = joyGetStickY(contpad);
	} else {
		movex = joyGetStickX(contpad);
		movey = joyGetStickY(contpad);
		lookx = joyGetRStickX(contpad);
		looky = joyGetRStickY(contpad);
	}

	sx = movex / 70.0f;
	sy = movey / 70.0f;

	// Move: a twitch, not a walk. Each new press of a C button (WASD) or push
	// of the stick jerks the chest once, the way all the move input points
	fwd = ((buttons & U_CBUTTONS) ? 1.0f : 0.0f) - ((buttons & D_CBUTTONS) ? 1.0f : 0.0f) + sy;
	side = ((buttons & R_CBUTTONS) ? 1.0f : 0.0f) - ((buttons & L_CBUTTONS) ? 1.0f : 0.0f) + sx;
	fwd = RD_CLAMP(fwd, -1.0f, 1.0f);
	side = RD_CLAMP(side, -1.0f, 1.0f);

	{
		u32 pressed = joyGetButtonsPressedThisFrame(contpad, U_CBUTTONS | D_CBUTTONS | L_CBUTTONS | R_CBUTTONS);
		bool stickout = sx * sx + sy * sy > 0.5f * 0.5f;
		bool trigger = pressed != 0 || (stickout && !pc->stickout);

		pc->stickout = stickout;

		if (pc->twitchwait60 > 0) {
			pc->twitchwait60 -= g_Vars.lvupdate60;
		}

		if (trigger && pc->twitchwait60 <= 0 && (fabsf(fwd) > 0.08f || fabsf(side) > 0.08f)
				&& rd->bodyindex[1] >= 0) {
			// The view's forward and right, level (the camera's own axes)
			Mtxf *cam = camGetWorldToScreenMtxf();
			f32 right[3] = { cam->m[0][0], 0.0f, cam->m[2][0] };
			f32 ahead[3] = { -cam->m[0][2], 0.0f, -cam->m[2][2] };
			f32 push[3];
			f32 len;

			rdVecNorm(right);
			rdVecNorm(ahead);

			push[0] = ahead[0] * fwd + right[0] * side;
			push[1] = 0.0f;
			push[2] = ahead[2] * fwd + right[2] * side;
			len = rdVecLen(push);

			if (len > 0.01f) {
				f32 along = 0.0f;
				f32 add[3];

				rdVecScale(push, 1.0f / len, push);

				if (pc->nudgeprevset) {
					f32 vel[3];

					rdVecSub(chest, pc->nudgeprev, vel);
					along = rdVecDot(vel, push) / dt;
				}

				if (along < RD_DEATHTWITCH_MAXSPEED) {
					rdVecScale(push, RD_DEATHTWITCH_CHEST, add);
					add[1] = RD_DEATHTWITCH_LIFT;
					pdphysRagdollAddVelocity(rd->handle, rd->bodyindex[1], add);
					rdVecScale(push, RD_DEATHTWITCH_BODY, add);
					pdphysRagdollAddVelocity(rd->handle, -1, add);
					pc->twitchwait60 = RD_DEATHTWITCH_GAP60;
					active = true;
				}
			}
		}
	}

	// Look: the mouse (first player only, as in the game) and the look stick,
	// at the living player's turn speed
	if (g_Vars.currentplayernum == 0) {
		inputMouseGetScaledDelta(&look[0], &look[1]);
		look[0] *= RD_DEATHLOOK_RATE * fovscale;
		look[1] *= RD_DEATHLOOK_RATE * fovscale;
	}

	{
		// As bondmove.c: a dead zone of 5, then the stick's fraction squared
		f32 lx = (lookx > 5 ? lookx - 5 : lookx < -5 ? lookx + 5 : 0) / 70.0f;
		f32 ly = (looky > 5 ? looky - 5 : looky < -5 ? looky + 5 : 0) / 70.0f;

		lx = RD_CLAMP(lx, -1.0f, 1.0f);
		ly = RD_CLAMP(ly, -1.0f, 1.0f);
		look[0] += lx * fabsf(lx) * RD_DEATHLOOK_RATE * fovscale * g_Vars.lvupdate60freal;
		look[1] -= ly * fabsf(ly) * RD_DEATHLOOK_RATE * fovscale * g_Vars.lvupdate60freal;
	}

	// The game's "Look Up/Down" option (Reverse) turns the pitch round
	if (!optionsGetForwardPitch(g_Vars.currentplayerstats->mpindex)) {
		look[1] = -look[1];
	}

	if (look[0] != 0.0f || look[1] != 0.0f) {
		rd->lookyaw = RD_CLAMP(rd->lookyaw - look[0], -RD_DEATHLOOK_MAXYAW, RD_DEATHLOOK_MAXYAW);
		rd->lookpitch = RD_CLAMP(rd->lookpitch + look[1], -RD_DEATHLOOK_MAXPITCH, RD_DEATHLOOK_MAXPITCH);
		active = true;
	}

	if (active) {
		pdphysRagdollKeepAwake(rd->handle, 0.5f);
		rd->sleeping = false;
	}

	rdVecCopy(chest, pc->nudgeprev);
	pc->nudgeprevset = true;
}

s32 ragdollWantPlayerBody(void)
{
	s32 pn = g_Vars.currentplayernum;

	if (!g_RagdollsEnabled || !g_RagdollDeathCam || !g_RagdollLevelLoaded || !pdphysIsReady()
			|| g_Vars.normmplayerisrunning || g_Vars.stagenum >= STAGE_TITLE
			|| pn < 0 || pn >= MAX_PLAYERS) {
		return false;
	}

	// A new death (co-op players can respawn): start over (keeping a blast
	// that just hit them)
	if (g_RdPlayerCam[pn].life != g_Vars.currentplayer->lifestarttime60) {
		struct rdplayercam keep = g_RdPlayerCam[pn];

		memset(&g_RdPlayerCam[pn], 0, sizeof(g_RdPlayerCam[pn]));
		g_RdPlayerCam[pn].life = g_Vars.currentplayer->lifestarttime60;
		g_RdPlayerCam[pn].blastframe = keep.blastframe;
		g_RdPlayerCam[pn].hitframe = keep.hitframe;
		g_RdPlayerCam[pn].hitdir = keep.hitdir;
		g_RdPlayerCam[pn].hitpart = keep.hitpart;
		g_RdPlayerCam[pn].hitgset = keep.hitgset;
		g_RdPlayerCam[pn].hithasgset = keep.hithasgset;
		memcpy(g_RdPlayerCam[pn].blastpos, keep.blastpos, sizeof(keep.blastpos));
		g_RdPlayerCam[pn].blastradius = keep.blastradius;
		g_RdPlayerCam[pn].blastdamage = keep.blastdamage;
	}

	return !g_RdPlayerCam[pn].failed;
}

/**
 * playerStartNewLife(): a co-op player respawns. In co-op the player's body
 * isn't removed when they come back (the other player sees it), and the
 * respawned player is that same chr: its ragdoll has to go, or it keeps
 * posing the living player (invisible, part ragdoll, hit through the body
 * left behind). The gun their body dropped goes too, as when a body is
 * removed (it's in this player's gun memory, which the new life reuses).
 */
void ragdollOnPlayerNewLife(void)
{
	RD_CRASHCTX("playernewlife", -1, -1);
	struct player *player = g_Vars.currentplayer;
	s32 pn = g_Vars.currentplayernum;

	if (player == NULL || player->prop == NULL || player->prop->chr == NULL) {
		return;
	}

	if (ragdollFindByChr(player->prop->chr)) {
		sysLogPrintf(LOG_NOTE, "RAGDOLL: player %d respawned: their body stops being a ragdoll", pn);
	}

	ragdollOnChrRemoved(player->prop->chr);

	if (pn >= 0 && pn < MAX_PLAYERS) {
		memset(&g_RdPlayerCam[pn], 0, sizeof(g_RdPlayerCam[pn]));
	}
}

/**
 * player.c, while the player is dead: the game has (re)built the player's
 * body; give it a death animation, then make it a ragdoll (with the
 * animation blend, the death animation leads at first).
 */
void ragdollPlayerBodyTick(void)
{
	RD_CRASHCTX("playerbodytick", -1, -1);
	struct player *player = g_Vars.currentplayer;
	struct rdplayercam *pc = &g_RdPlayerCam[g_Vars.currentplayernum];
	struct chrdata *chr;
	struct ragdoll *rd;

	// The body uses the gun's memory. Taking it makes the gun code want to
	// reload the gun ("load all"), which can't happen while the body holds the
	// memory and would freeze the screen (lockscreen) every frame. The dead
	// player has no gun to show, so stop it asking.
	if (player->haschrbody) {
		player->gunctrl.loadall = false;
	}

	if (pc->made) {
		rdPlayerControlTick(pc); // MOD: death camera control
		return;
	}

	if (pc->failed) {
		return;
	}

	if (!player->haschrbody || player->prop == NULL || player->prop->chr == NULL
			|| player->prop->chr->model == NULL || player->prop->chr->model->anim == NULL) {
		// The body shares the gun's memory, which takes a few frames to free
		if (++pc->waitframes > 90) {
			sysLogPrintf(LOG_WARNING, "RAGDOLL: death camera: no body for player %d, using the normal death camera", g_Vars.currentplayernum);
			pc->failed = true;
		}

		return;
	}

	chr = player->prop->chr;
	pc->bodyframes++;

	if (pc->bodyframes == 1) {
		// Start one of the game's death animations on the body
		s32 num = 0;

		while (g_DeathAnimations[num] != 0) {
			num++;
		}

		if (num > 0) {
			modelSetAnimation(chr->model, g_DeathAnimations[rngRandom() % num], rngRandom() % 2, 0, 0.5f, 0);
		}

		return;
	}

	// Let the game place the body for a couple of frames first
	if (pc->bodyframes < 3) {
		return;
	}

	// The gun falls from the hands, like the guards' do (can't be picked up:
	// it's the player's own, in the gun memory)
	{
		struct rdplayergun *pg = &g_RdPlayerGuns[g_Vars.currentplayernum];
		s32 h;

		pg->chr = chr;

		for (h = 0; h < 2; h++) {
			struct prop *gun = chr->weapons_held[h];

			pg->props[h] = NULL;
			pg->objs[h] = NULL;

			if (gun && gun->type == PROPTYPE_WEAPON && gun->weapon && gun->parent == chr->prop) {
				gun->weapon->base.flags |= OBJFLAG_UNCOLLECTABLE;
				objSetDropped(gun, DROPTYPE_DEFAULT);

				// Guards' guns fall in their chr tick (CHRHFLAG_DROPPINGITEM),
				// which the player's body doesn't get: fall now
				if (!objDrop(gun, false)) {
					sysLogPrintf(LOG_WARNING, "RAGDOLL: death camera: player %d's gun %d didn't drop", g_Vars.currentplayernum, h);
					continue;
				}

				pg->props[h] = gun;
				pg->objs[h] = gun->weapon;
			}
		}
	}

	// Knocked back by the hit that killed them, like NPCs (within the last second)
	g_RagdollCreatingPlayer = chr;

	if (pc->hitframe != 0 && g_Vars.lvframe60 - pc->hitframe < 60) {
		ragdollOnChrDeath(chr, &pc->hitdir, pc->hitpart, pc->hithasgset ? &pc->hitgset : NULL, false);
	} else {
		ragdollOnChrDeath(chr, NULL, HITPART_GENERAL, NULL, false);
	}

	g_RagdollCreatingPlayer = NULL;

	rd = ragdollFindByChr(chr);

	if (rd) {
		rd->isplayer = true;
		rd->playernum = g_Vars.currentplayernum;
		pc->made = true;

		// Room tracking: the player's prop stays where they died, so the
		// body's rooms are tracked separately and added to the prop's rooms
		memcpy(rd->playerbaserooms, chr->prop->rooms, sizeof(rd->playerbaserooms));
		rd->playerbaserooms[7] = -1;
		rd->bodyroom = chr->prop->rooms[0];
		rdVecCopy(rd->world[0].m[3], rd->prevpelvis);

		// The player's own body always braces the fall
		// so the arms come into view, whatever the reflex settings rolled
		rd->reflexes = true;
		rd->canbrace = true;
		rd->clutcharm = -1;
		rd->brace = 0.0f;

		// Killed by a blast (the body is made a few frames later): throw it now
		if (pc->blastframe != 0 && g_Vars.lvframe60 - pc->blastframe < 60) {
			struct coord c;

			rdVecToCoord(pc->blastpos, &c);
			ragdollOnExplosion(chr, &c, pc->blastradius, pc->blastdamage);
		}

		pc->camvalid = false;
		sysLogPrintf(LOG_NOTE, "RAGDOLL: death camera: player %d's body is a ragdoll (pelvis %.0f above the floor)",
				g_Vars.currentplayernum, rd->world[0].m[3][1] - player->vv_ground);
	} else {
		sysLogPrintf(LOG_WARNING, "RAGDOLL: death camera: player %d's body couldn't become a ragdoll, using the normal death camera", g_Vars.currentplayernum);
		pc->failed = true;
	}
}

/**
 * explosions.c, first frame of a blast reaching a player. If the player's
 * ragdoll exists it's thrown right away; otherwise it's remembered for when
 * the body is made (dying from this blast).
 */
void ragdollOnPlayerExplosion(struct prop *prop, struct coord *centre, f32 radius, f32 damage)
{
	RD_CRASHCTX("onplayerexplosion", -1, -1);
	s32 pn = playermgrGetPlayerNumByProp(prop);
	struct rdplayercam *pc;

	if (pn < 0 || pn >= MAX_PLAYERS || g_Vars.normmplayerisrunning) {
		return;
	}

	pc = &g_RdPlayerCam[pn];

	if (prop->chr && ragdollFindByChr(prop->chr)) {
		ragdollOnExplosion(prop->chr, centre, radius, damage);
		return;
	}

	pc->blastframe = RD_FRAMESTAMP();
	rdVecSet(pc->blastpos, centre->x, centre->y, centre->z);
	pc->blastradius = radius;
	pc->blastdamage = damage;
}

/**
 * chrDamage() on a player: remember the direction and weapon of the hit; if
 * it kills them, their body gets the same knock-back an NPC would.
 */
void ragdollOnPlayerHit(struct prop *prop, struct coord *dir, s32 hitpart, struct gset *gset)
{
	RD_CRASHCTX("onplayerhit", -1, hitpart);
	s32 pn = playermgrGetPlayerNumByProp(prop);
	struct rdplayercam *pc;
	f32 len;

	if (pn < 0 || pn >= MAX_PLAYERS || g_Vars.normmplayerisrunning || dir == NULL) {
		return;
	}

	pc = &g_RdPlayerCam[pn];

	// Already dead: hits on the ragdoll are handled as shots on a corpse
	if (pc->made) {
		return;
	}

	len = rdVecLen(dir->f);

	if (len < 0.0001f) {
		return;
	}

	pc->hitframe = RD_FRAMESTAMP();
	pc->hitdir = *dir;
	rdVecDiv(pc->hitdir.f, len);
	pc->hitpart = hitpart;
	pc->hithasgset = gset != NULL;

	if (gset) {
		pc->hitgset = *gset;
	}
}

struct ragdoll *ragdollFindPlayerRagdoll(s32 playernum)
{
	s32 i;

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		if (g_Ragdolls[i].state != RDSTATE_FREE && g_Ragdolls[i].isplayer && g_Ragdolls[i].playernum == playernum) {
			return &g_Ragdolls[i];
		}
	}

	return NULL;
}

/**
 * player.c: the camera for a dead player. Rides in the ragdoll's head (eyes
 * just in front of the head joint), smoothed a little. Returns false to use
 * the normal death camera.
 */
s32 ragdollGetDeathCam(struct coord *pos, struct coord *up, struct coord *look, struct coord *from)
{
	RD_CRASHCTX("getdeathcam", -1, -1);
	struct rdplayercam *pc;
	struct ragdoll *rd;
	f32 eye[3];
	f32 local[3];
	f32 off[3];
	f32 k;
	s32 i;

	if (!g_RagdollDeathCam || g_Vars.currentplayernum < 0 || g_Vars.currentplayernum >= MAX_PLAYERS) {
		return false;
	}

	pc = &g_RdPlayerCam[g_Vars.currentplayernum];

	if (!pc->made) {
		return false;
	}

	rd = ragdollFindPlayerRagdoll(g_Vars.currentplayernum);

	if (rd == NULL) {
		return false;
	}

	// Eyes: up the head and a little forward (models face +Z)
	rdVecSet(local, 0.0f, rd->torso * 0.40f, rd->torso * 0.20f);
	rdRotApply(&rd->world[2], local, off);

	rdVecAdd(rd->world[2].m[3], off, eye);

	// Keep the eyes out of walls and floors the head is lying against: trace
	// from the middle of the head to the eyes, and keep some floor clearance
	{
		f32 centre[3];
		f32 to[3];
		f32 dir[3];
		f32 hit[3];
		f32 normal[3];
		f32 len;
		s32 room;

		rdVecSet(local, 0.0f, rd->torso * 0.30f, 0.0f);
		rdRotApply(&rd->world[2], local, off);

		rdVecAdd(rd->world[2].m[3], off, centre);

		rdVecSub(eye, centre, dir);
		len = rdVecLen(dir);

		if (len > 0.01f) {
			rdVecNorm(dir);
			rdVecMad(eye, dir, RD_DEATHCAM_CLEAR, to);

			if (pdphysRaycast(centre, to, hit, normal, &room)) {
				eye[0] = hit[0] - dir[0] * RD_DEATHCAM_CLEAR + normal[0] * 2.0f;
				eye[1] = hit[1] - dir[1] * RD_DEATHCAM_CLEAR + normal[1] * 2.0f;
				eye[2] = hit[2] - dir[2] * RD_DEATHCAM_CLEAR + normal[2] * 2.0f;
			}
		}

		to[0] = eye[0];
		to[1] = eye[1] - RD_DEATHCAM_FLOORCLEAR;
		to[2] = eye[2];

		if (pdphysRaycast(eye, to, hit, normal, &room) && normal[1] > 0.7f) {
			eye[1] = hit[1] + RD_DEATHCAM_FLOORCLEAR;
		}
	}

	if (!pc->camvalid) {
		k = 1.0f;
		pc->camvalid = true;
	} else {
		f32 s = RD_CLAMP(g_RagdollDeathCamSmooth, 0.0f, 0.95f);
		k = 1.0f - powf(s, g_Vars.lvupdate60freal > 0.0f ? g_Vars.lvupdate60freal : 1.0f);
	}

	// Body awareness: turn the view from where the face points towards the
	// body (in front of the chest, where the arms reach), so the player sees
	// their own limbs while falling
	{
		f32 wantlook[3] = { rd->world[2].m[2][0], rd->world[2].m[2][1], rd->world[2].m[2][2] };
		f32 wantup[3] = { rd->world[2].m[1][0], rd->world[2].m[1][1], rd->world[2].m[1][2] };
		f32 w = RD_CLAMP(g_RagdollDeathCamBodyView, 0.0f, 1.0f);

		// No pull back to the body while the player looks around
		if (fabsf(rd->lookyaw) > 0.05f || fabsf(rd->lookpitch) > 0.05f) {
			w = 0.0f;
		}

		if (w > 0.0f && (rd->severed & (1u << 1)) == 0) {
			f32 target[3];
			f32 tobody[3];
			f32 dot;

			for (i = 0; i < 3; i++) {
				target[i] = rd->world[1].m[3][i] * 0.6f + rd->world[0].m[3][i] * 0.4f
					+ rd->world[1].m[2][i] * rd->torso * 0.6f;
			}

			rdVecSub(target, eye, tobody);

			if (rdVecLen(tobody) > 1.0f) {
				// The body may be at most this far off the middle of the view:
				// 50 degrees (bottom edge) at 0%, down to 15 degrees at 100%
				f32 maxangle = (50.0f - 35.0f * w) * 0.0174533f;
				f32 angle;
				f32 perp[3];

				rdVecNorm(tobody);
				rdVecNorm(wantlook);

				dot = rdVecDot(wantlook, tobody);
				angle = acosf(RD_CLAMP(dot, -1.0f, 1.0f));

				if (angle > maxangle) {
					// Turn just enough to bring the body to that angle,
					// staying on the side the face points to
					for (i = 0; i < 3; i++) {
						perp[i] = wantlook[i] - tobody[i] * dot;
					}

					if (rdVecLen(perp) < 0.01f) {
						rdVecCopy(rd->world[2].m[1], perp);
					}

					rdVecNorm(perp);

					for (i = 0; i < 3; i++) {
						wantlook[i] = tobody[i] * cosf(maxangle) + perp[i] * sinf(maxangle);
					}
				}

				if (rdVecLen(wantlook) > 0.01f) {
					rdVecNorm(wantlook);

					// Keep the up vector square to the new view direction
					dot = rdVecDot(wantup, wantlook);

					for (i = 0; i < 3; i++) {
						wantup[i] -= wantlook[i] * dot;
					}

					if (rdVecLen(wantup) < 0.01f) {
						rdVecCopy(rd->world[2].m[1], wantup);
					}
				} else {
					rdVecCopy(rd->world[2].m[2], wantlook);
				}
			}
		}

		// Looking past what the neck can turn (death camera control): the
		// view turns the rest of the way, about its own up and side axes
		{
			f32 extrayaw = rd->lookyaw - RD_CLAMP(rd->lookyaw, -RD_DEATHLOOK_YAW, RD_DEATHLOOK_YAW);
			f32 extrapitch = rd->lookpitch - RD_CLAMP(rd->lookpitch, -RD_DEATHLOOK_PITCH, RD_DEATHLOOK_PITCH);

			if (extrayaw != 0.0f || extrapitch != 0.0f) {
				f32 axis[3];

				rdVecNorm(wantlook);
				rdVecNorm(wantup);

				if (extrayaw != 0.0f) {
					rdVecCopy(wantup, axis);
					rdRotateAxis(wantlook, axis, extrayaw);
				}

				if (extrapitch != 0.0f) {
					rdVecCross(wantlook, wantup, axis);

					if (rdVecLen(axis) > 0.01f) {
						rdVecNorm(axis);
						rdRotateAxis(wantlook, axis, -extrapitch); // + = down
						rdRotateAxis(wantup, axis, -extrapitch);
					}
				}
			}
		}

		for (i = 0; i < 3; i++) {
			pc->pos[i] += (eye[i] - pc->pos[i]) * k;
			pc->look[i] += (wantlook[i] - pc->look[i]) * k;
			pc->up[i] += (wantup[i] - pc->up[i]) * k;
		}
	}

	rdVecNorm(pc->look);
	rdVecNorm(pc->up);

	pos->x = pc->pos[0];
	pos->y = pc->pos[1];
	pos->z = pc->pos[2];
	look->x = pc->look[0];
	look->y = pc->look[1];
	look->z = pc->look[2];
	up->x = pc->up[0];
	up->y = pc->up[1];
	up->z = pc->up[2];

	// Where to trace the camera's room from: the player's own position
	*from = g_Vars.currentplayer->prop->pos;

	return true;
}

/**
 * MOD: ragdoll physics - guards still shooting at the dead player aim at the
 * ragdoll's head, not where he died: during a chr's AI and firing tick
 * (chr.c) the target prop is moved there, and End puts it back. Returns the
 * prop that was moved, or NULL.
 */
struct prop *ragdollAimAtDeadPlayerBegin(struct chrdata *chr, struct coord *saved)
{
	struct prop *target;
	struct ragdoll *rd;
	s32 pn;
	f32 local[3];
	f32 off[3];

	if (!g_RagdollsEnabled || chr == NULL || chr->prop == NULL || g_Vars.normmplayerisrunning) {
		return NULL;
	}

	target = chrGetTargetProp(chr);

	if (target == NULL || target->type != PROPTYPE_PLAYER) {
		return NULL;
	}

	pn = playermgrGetPlayerNumByProp(target);

	if (pn < 0 || pn >= MAX_PLAYERS || !g_Vars.players[pn]->isdead || !g_RdPlayerCam[pn].made) {
		return NULL;
	}

	rd = ragdollFindPlayerRagdoll(pn);

	if (rd == NULL || rd->numparts < 3 || (rd->severed & (1u << 2))) {
		return NULL;
	}

	*saved = target->pos;

	// The middle of the head
	rdVecSet(local, 0.0f, rd->torso * 0.30f, 0.0f);
	rdRotApply(&rd->world[2], local, off);
	target->pos.x = rd->world[2].m[3][0] + off[0];
	target->pos.y = rd->world[2].m[3][1] + off[1];
	target->pos.z = rd->world[2].m[3][2] + off[2];

	return target;
}

void ragdollAimAtDeadPlayerEnd(struct prop *target, struct coord *saved)
{
	if (target) {
		target->pos = *saved;
	}
}

/**
 * MOD: death camera hold (Ragdoll.RagdollDeathCamHold) - in solo the mission
 * waits after death, death camera kept, until Start or Fire is pressed.
 * True while it should wait.
 */
s32 ragdollHoldDeathCam(void)
{
	struct rdplayercam *pc;
	s32 contpad;

	if (!g_RagdollDeathCamHold || !g_RagdollDeathCam || g_Vars.mplayerisrunning
			|| g_Vars.currentplayernum < 0 || g_Vars.currentplayernum >= MAX_PLAYERS) {
		return false;
	}

	pc = &g_RdPlayerCam[g_Vars.currentplayernum];

	if (!pc->made || pc->holdreleased || ragdollFindPlayerRagdoll(g_Vars.currentplayernum) == NULL) {
		return false;
	}

	if (currentPlayerIsMenuOpenInSoloOrMp()) {
		return true;
	}

	contpad = optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex);

	// Start (Esc on a keyboard) or Fire
	if (joyGetButtonsPressedThisFrame(contpad, START_BUTTON | Z_TRIG) || inputKeyJustPressed(VK_ESCAPE)) {
		pc->holdreleased = true;
		sysLogPrintf(LOG_NOTE, "RAGDOLL: death camera: player %d pressed Start/Fire, the mission ends", g_Vars.currentplayernum);
		return false;
	}

	return true;
}

// The player watching from inside their ragdoll's head doesn't see the head
bool ragdollHideHeadForViewer(struct ragdoll *rd)
{
	return rd->isplayer && g_RagdollDeathCam && rd->playernum == g_Vars.currentplayernum
		&& g_RdPlayerCam[rd->playernum].made;
}

#endif // PD_PHYSICS
