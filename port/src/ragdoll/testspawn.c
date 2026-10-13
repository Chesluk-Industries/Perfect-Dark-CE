/**
 * MOD: ragdoll physics - developer tool: the H key spawns a guard where the
 * crosshair points (Ragdoll.RagdollTestSpawnKey). Part of the ragdoll module
 * (port/src/ragdoll.c).
 */

#include "rd_internal.h"

#ifdef PD_PHYSICS

/* ---- test feature: H spawns an unarmed chr at the crosshair ---- */

#define RD_VK_H 11 // SDL scancode for H (VK_A is 4)

void ragdollTestSpawnTick(void)
{
	RD_CRASHCTX("testspawntick", -1, -1);
	static const s32 bodies[] = { BODY_DD_GUARD, BODY_DD_SECGUARD, BODY_DD_LABTECH, BODY_DDSHOCK };
	static s32 nextbody = 0;
	struct player *player = g_Vars.players[0];
	struct coord dir;
	struct coord pos;
	RoomNum rooms[8];
	struct prop *spawned;
	f32 from[3];
	f32 to[3];
	f32 hit[3];
	f32 normal[3];
	f32 facing;
	s32 hitroom = -1;

	if (!inputKeyJustPressed(RD_VK_H)) {
		return;
	}

	if (!g_RagdollTestSpawnKey
			|| g_Vars.normmplayerisrunning
			|| g_Vars.stagenum >= STAGE_TITLE
			|| g_Vars.tickmode != TICKMODE_NORMAL
			|| g_Vars.in_cutscene
			|| lvIsPaused()
			|| player == NULL
			|| player->prop == NULL) {
		return;
	}

	// Straight ahead from the camera (see rdGrabAim)
	dir.x = player->cam_look.x;
	dir.y = player->cam_look.y;
	dir.z = player->cam_look.z;

	rdCoordToVec(&player->cam_pos, from);
	to[0] = from[0] + dir.x * 10000.0f;
	to[1] = from[1] + dir.y * 10000.0f;
	to[2] = from[2] + dir.z * 10000.0f;

	// Any room: the ragdoll raycast only sees the rooms ragdolls are in
	if (!pdphysRaycastAnyRoom(from, to, hit, normal, &hitroom)) {
		sysLogPrintf(LOG_NOTE, "TESTSPAWN: crosshair isn't pointing at anything");
		return;
	}

	// Hit a wall or ceiling: step back towards the player and drop to the floor
	if (normal[1] < 0.6f) {
		f32 floorhit[3];

		from[0] = hit[0] - dir.x * 40.0f;
		from[1] = hit[1] - dir.y * 40.0f;
		from[2] = hit[2] - dir.z * 40.0f;
		to[0] = from[0];
		to[1] = from[1] - 2000.0f;
		to[2] = from[2];

		if (!pdphysRaycastAnyRoom(from, to, floorhit, normal, &hitroom) || normal[1] < 0.6f) {
			sysLogPrintf(LOG_NOTE, "TESTSPAWN: no floor under the crosshair");
			return;
		}

		rdVecCopy(floorhit, hit);
	}

	pos.x = hit[0];
	pos.y = hit[1] + 50.0f;
	pos.z = hit[2];

	// Seed the room list with the room of the floor that was hit (level
	// geometry bodies carry their room number; props don't), then let the
	// game add the neighbouring rooms the chr overlaps
	if (hitroom > 0) {
		rooms[0] = hitroom;
	} else {
		RoomNum inrooms[21];
		RoomNum aboverooms[21];
		RoomNum bestroom = -1;

		bgFindRoomsByPos(&pos, inrooms, aboverooms, 20, &bestroom);

		if (inrooms[0] != -1) {
			rooms[0] = inrooms[0];
		} else if (aboverooms[0] != -1) {
			rooms[0] = aboverooms[0];
		} else if (bestroom > 0) {
			rooms[0] = bestroom;
		} else {
			rooms[0] = player->prop->rooms[0];
		}
	}

	rooms[1] = -1;
	chr0f021fa8(NULL, &pos, rooms);

	// Face the player. Chr models face +Z, rotated by the facing angle
	// around Y, so the facing direction is (sin(angle), 0, cos(angle)).
	facing = atan2f(player->prop->pos.x - pos.x, player->prop->pos.z - pos.z);

	if (facing < 0.0f) {
		facing += M_BADTAU;
	}

	spawned = chrSpawnAtCoord(bodies[nextbody], HEAD_RANDOM, &pos, rooms, facing,
			ailistFindById(GAILIST_IDLE), SPAWNFLAG_ALLOWONSCREEN);

	if (spawned) {
		sysLogPrintf(LOG_NOTE, "TESTSPAWN: chr %d (body %d) at (%.0f, %.0f, %.0f)",
				spawned->chr->chrnum, bodies[nextbody], spawned->pos.x, spawned->pos.y, spawned->pos.z);
		nextbody = (nextbody + 1) % (s32) ARRAYCOUNT(bodies);
	} else {
		sysLogPrintf(LOG_NOTE, "TESTSPAWN: could not spawn at (%.0f, %.0f, %.0f) (no free chr slot or no room there)", pos.x, pos.y, pos.z);
	}
}

#endif // PD_PHYSICS
