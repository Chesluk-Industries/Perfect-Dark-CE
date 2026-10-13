/**
 * MOD: ragdoll physics - which rooms bodies and their cut pieces are in, and
 * whether they are on screen. Part of the ragdoll module (port/src/ragdoll.c).
 */

#include "rd_internal.h"

#ifdef PD_PHYSICS

/**
 * Room tracking for bodies and cut pieces. The game's own update for a chr
 * (chr0f0220ac) only ever adds rooms touched by a small box around the prop
 * and never drops old ones, so a body thrown or dropped a long way kept its
 * old rooms and vanished once they were out of view.
 *
 * A body or piece is followed through the portals it crosses, from the room
 * it was in, as the game does for living chrs (func0f065dd8). Levels put
 * some rooms in the same space, shown one at a time through portals
 * (Defection's maintenance room and the corridor by Cassandra's office, G5
 * Building's inside and outside): any test by position alone can pick the
 * other room, which drew the body there and made it collide with that
 * room's walls and floors. Only the way it came tells them apart.
 *
 * The position tests remain as the fallback when the portals lead nowhere
 * (no room yet, or it left the level's rooms): the last room, the rooms
 * next to it, then every room.
 */
static RoomNum rdFindRoomGlobal(struct coord *pos)
{
	RoomNum best = -1;
	f32 bestvol = 0.0f;
	s32 r;

	for (r = 1; r < g_Vars.roomcount; r++) {
		if (bgRoomContainsCoord(pos, r)) {
			f32 vol;

			if (bgTestPosInRoomCheap(pos, r)) {
				return r;
			}

			vol = (g_Rooms[r].bbmax[0] - g_Rooms[r].bbmin[0]) * (g_Rooms[r].bbmax[1] - g_Rooms[r].bbmin[1])
				* (g_Rooms[r].bbmax[2] - g_Rooms[r].bbmin[2]);

			if (best < 0 || vol < bestvol) {
				best = r;
				bestvol = vol;
			}
		}
	}

	return best;
}

static bool rdValidRoom(RoomNum room)
{
	return room > 0 && room < g_Vars.roomcount;
}

/**
 * Of 'rooms' (-1 terminated), the one whose portals enclose pos (-1 if
 * none), and in *inbounds the first one whose bounds contain it.
 */
static RoomNum rdPickRoom(struct coord *pos, const RoomNum *rooms, RoomNum *inbounds)
{
	s32 i;

	*inbounds = -1;

	for (i = 0; i < 8 && rooms[i] != -1; i++) {
		if (!rdValidRoom(rooms[i]) || !bgRoomContainsCoord(pos, rooms[i])) {
			continue;
		}

		if (bgTestPosInRoomCheap(pos, rooms[i])) {
			return rooms[i];
		}

		if (*inbounds < 0) {
			*inbounds = rooms[i];
		}
	}

	return -1;
}

/**
 * Works out the room pos is in, having moved there from prevpos in
 * lastroom, and lists it in 'out' followed by the rooms the box lower..upper
 * reaches through its portals. Returns the room (lastroom if it's outside
 * every room).
 */
static RoomNum rdTrackRooms(struct coord *pos, struct coord *prevpos, struct coord *lower, struct coord *upper, RoomNum lastroom, RoomNum *out)
{
	RoomNum in = -1;
	RoomNum inbounds = -1;

	if (rdValidRoom(lastroom)) {
		RoomNum from[2] = { lastroom, -1 };
		RoomNum traced[8];

		// Through the portals it crossed (every room this finds contains
		// pos; a room that only does by its bounds will do)
		traced[0] = -1;
		func0f065dd8(prevpos, from, pos, traced);
		in = rdPickRoom(pos, traced, &inbounds);

		if (in < 0) {
			in = inbounds;
		}

		// Fallback: the rooms next to the last one
		if (in < 0) {
			RoomNum around[8] = { lastroom, -1 };

			bgFindEnteredRooms(lower, upper, around, 7, false);
			in = rdPickRoom(pos, around, &inbounds);
		}
	}

	// ... then every room. Rooms' bounds overlap round doors, so a room whose
	// portals enclose it, anywhere, beats a neighbour it's only in the
	// bounds of.
	if (in < 0) {
		in = rdFindRoomGlobal(pos);

		if (in <= 0) {
			in = inbounds;
		}
	}

	if (in <= 0) {
		// Outside every room (fell out of the level): keep what it had
		if (!rdValidRoom(lastroom)) {
			out[0] = -1;
			return lastroom;
		}

		in = lastroom;
	}

	out[0] = in;
	out[1] = -1;
	bgFindEnteredRooms(lower, upper, out, 7, false);

	return in;
}

static void rdMergeRooms(RoomNum *merged, s32 *num, const RoomNum *add)
{
	s32 i, k;

	for (i = 0; i < 7 && add[i] != -1 && *num < 7; i++) {
		bool have = false;

		for (k = 0; k < *num; k++) {
			if (merged[k] == add[i]) {
				have = true;
				break;
			}
		}

		if (!have) {
			merged[(*num)++] = add[i];
		}
	}
}

/**
 * Registers the corpse's prop in the rooms around the body (pelvis to head,
 * plus where the pelvis was last tick, for fast throws) and around each cut
 * piece. The dead player's prop stays where they died, so it also keeps the
 * death rooms.
 */
// A bounding box around points: grown to take in p, then padded all round
static void rdBoxAdd(struct coord *lower, struct coord *upper, const f32 *p)
{
	if (p[0] < lower->x) lower->x = p[0];
	if (p[1] < lower->y) lower->y = p[1];
	if (p[2] < lower->z) lower->z = p[2];
	if (p[0] > upper->x) upper->x = p[0];
	if (p[1] > upper->y) upper->y = p[1];
	if (p[2] > upper->z) upper->z = p[2];
}

static void rdBoxPad(struct coord *lower, struct coord *upper, f32 m)
{
	lower->x -= m; lower->y -= m; lower->z -= m;
	upper->x += m; upper->y += m; upper->z += m;
}

// Adds a room to a list if it isn't there yet (at most max)
static void rdRoomListAdd(s32 *list, s32 *n, s32 max, s32 room)
{
	s32 k;

	for (k = 0; k < *n && list[k] != room; k++);

	if (k == *n && *n < max) {
		list[(*n)++] = room;
	}
}

void ragdollUpdateBodyRooms(struct ragdoll *rd)
{
	RD_CRASHCTX("bodyrooms", RD_RDCHRNUM(rd), -1);
	struct prop *prop = rd->prop;
	RoomNum found[8];
	RoomNum merged[8];
	struct coord lower;
	struct coord upper;
	struct coord pelvis;
	struct coord prev;
	s32 num = 0;
	s32 i, k;
	bool same;

	if (prop == NULL) {
		return;
	}

	rdVecToCoord(rd->world[0].m[3], &pelvis);

	if (rd->bodyroom <= 0) {
		rd->bodyroom = prop->rooms[0] > 0 ? prop->rooms[0] : rdFindRoomGlobal(&pelvis);
		rdCoordToVec(&pelvis, rd->prevpelvis);

		if (rd->bodyroom <= 0) {
			return;
		}
	}

	lower = upper = pelvis;

	// Every attached part counts (a body lying across a doorway can have its
	// arms or legs in another room than its pelvis)
	for (i = 0; i <= rd->numparts; i++) {
		if (i == rd->numparts) {
			rdBoxAdd(&lower, &upper, rd->prevpelvis);
		} else if (!(rd->severed & (1u << i))) {
			rdBoxAdd(&lower, &upper, rd->world[i].m[3]);
		}
	}

	rdBoxPad(&lower, &upper, 40.0f);

	rdVecToCoord(rd->prevpelvis, &prev);
	rdCoordToVec(&pelvis, rd->prevpelvis);

	rd->bodyroom = rdTrackRooms(&pelvis, &prev, &lower, &upper, rd->bodyroom, found);

	// The body's room first (lighting), the room each cut piece is in, the
	// player's death rooms, the rooms around the body, then the rooms around
	// the pieces. (The pieces' own rooms come early: with the body's
	// surroundings first, a piece carried far off could be left out of the
	// prop's rooms, and it vanished whenever the body's rooms weren't in view.)
	if (rd->bodyroom > 0) {
		merged[num++] = rd->bodyroom;
	}

	for (k = 1; k < rd->numparts && num < 7; k++) {
		if ((rd->severed & (1u << k)) && rd->piecerooms[k][0] > 0) {
			RoomNum own[2] = { rd->piecerooms[k][0], -1 };

			rdMergeRooms(merged, &num, own);
		}
	}

	if (rd->isplayer) {
		rdMergeRooms(merged, &num, rd->playerbaserooms);
	}

	rdMergeRooms(merged, &num, found);

	for (k = 1; k < rd->numparts && num < 7; k++) {
		if ((rd->severed & (1u << k)) && rd->piecerooms[k][0] != -1) {
			rdMergeRooms(merged, &num, rd->piecerooms[k]);
		}
	}

	if (num == 0) {
		return; // never leave the prop in no room
	}

	merged[num] = -1;

	same = true;

	for (k = 0; k <= num; k++) {
		if (prop->rooms[k] != merged[k]) {
			same = false;
			break;
		}
	}

	if (same) {
		return;
	}

	propDeregisterRooms(prop);

	for (k = 0; k <= num; k++) {
		prop->rooms[k] = merged[k];
	}

	propRegisterRooms(prop);
}

/**
 * The rooms whose level geometry the body collides with: the rooms around
 * all its parts (and pieces), reached through portals from the room it's in,
 * like the game's own collision. Levels draw some rooms on top of each other
 * in the same space (shown one at a time through portals); the hidden one's
 * floors and walls must not catch the body.
 */
void ragdollUpdateCollisionRooms(struct ragdoll *rd)
{
	RoomNum rooms[24];
	s32 ints[24];
	struct coord lower, upper;
	s32 i, k, n;

	if (rd->handle < 0) {
		return;
	}

	if (rd->bodyroom <= 0 || rd->bodyroom >= g_Vars.roomcount) {
		pdphysRagdollSetRooms(rd->handle, NULL, -1); // not known: everything
		return;
	}

	rdVecToCoord(rd->world[0].m[3], &lower);
	upper = lower;

	for (i = 0; i < rd->numparts; i++) {
		rdBoxAdd(&lower, &upper, rd->world[i].m[3]);
	}

	// Rooms entered through portals close to the parts (like the game's own
	// collision, which uses a prop's own bbox)
	rdBoxPad(&lower, &upper, 30.0f);

	// MOD: clipping checks - and where the body is headed: a fast fall
	// covers more than the margin in one step, so the floor of the room
	// below (past a portal) wasn't in the set yet and was passed through.
	// Two steps' worth of the last step's move, along each axis.
	{
		f32 *pel = rd->world[0].m[3];

		if (rd->roomsprevvalid) {
			f32 d[3];

			for (i = 0; i < 3; i++) {
				d[i] = RD_CLAMP((pel[i] - rd->roomsprevpos[i]) * 2.0f, -400.0f, 400.0f);
			}

			if (d[0] < 0.0f) lower.x += d[0]; else upper.x += d[0];
			if (d[1] < 0.0f) lower.y += d[1]; else upper.y += d[1];
			if (d[2] < 0.0f) lower.z += d[2]; else upper.z += d[2];
		}

		rdVecCopy(pel, rd->roomsprevpos);
		rd->roomsprevvalid = true;
	}

	rooms[0] = rd->bodyroom;
	rooms[1] = -1;
	bgFindEnteredRooms(&lower, &upper, rooms, 20, false);

	// Wider for rooms on screen (below)
	rdBoxPad(&lower, &upper, 60.0f);

	for (n = 0; n < 20 && rooms[n] != -1; n++) {
		ints[n] = rooms[n];
	}

	// Rooms on screen just now whose space the body reaches: big rooms
	// (outdoor terrain) whose portals are far away. A room drawn in the same
	// space but not shown from here isn't on screen.
	for (i = 1; i < g_Vars.roomcount && n < 24; i++) {
		if (g_Rooms[i].loaded240 <= 0 || g_Rooms[i].loaded240 > 60
				|| g_Rooms[i].bbmin[0] > upper.x || g_Rooms[i].bbmax[0] < lower.x
				|| g_Rooms[i].bbmin[1] > upper.y || g_Rooms[i].bbmax[1] < lower.y
				|| g_Rooms[i].bbmin[2] > upper.z || g_Rooms[i].bbmax[2] < lower.z) {
			continue;
		}

		rdRoomListAdd(ints, &n, 24, i);
	}

	// The rooms the prop is registered in (body and cut pieces)
	if (rd->prop) {
		for (i = 0; i < 8 && rd->prop->rooms[i] != -1 && n < 24; i++) {
			rdRoomListAdd(ints, &n, 24, rd->prop->rooms[i]);
		}
	}

	// MOD: clipping checks - rooms the level's scripts turned off this frame
	// (e.g. an intact wall swapped for the broken one) don't collide
	for (i = 0, k = 0; i < n; i++) {
		if (ints[i] <= 0 || ints[i] >= g_Vars.roomcount || (g_Rooms[ints[i]].flags & ROOMFLAG_DISABLEDBYSCRIPT) == 0) {
			ints[k++] = ints[i];
		}
	}

	if (k == 0) {
		ints[k++] = rd->bodyroom;
	}

	n = k;

	pdphysRagdollSetRooms(rd->handle, ints, n);
}

// Piece rooms: a cut piece keeps the corpse registered in the rooms it's in,
// so the corpse is drawn while any piece is in view
void ragdollUpdatePieceRooms(struct ragdoll *rd)
{
	RD_CRASHCTX("updatepiecerooms", RD_RDCHRNUM(rd), -1);
	s32 i;

	if (rd->severed == 0) {
		return;
	}

	for (i = 1; i < rd->numparts; i++) {
		if (rd->severed & (1u << i)) {
			struct coord pos;
			struct coord prev;
			struct coord lower;
			struct coord upper;
			RoomNum last;

			rdVecToCoord(rd->world[i].m[3], &pos);

			lower.x = pos.x - 50.0f; lower.y = pos.y - 60.0f; lower.z = pos.z - 50.0f;
			upper.x = pos.x + 50.0f; upper.y = pos.y + 60.0f; upper.z = pos.z + 50.0f;

			// A piece just cut off starts where the body is
			if (rd->piecerooms[i][0] > 0) {
				last = rd->piecerooms[i][0];
				rdVecToCoord(rd->pieceprev[i], &prev);
			} else {
				last = rd->bodyroom > 0 ? rd->bodyroom : rd->prop->rooms[0];
				rdVecToCoord(rd->prevpelvis, &prev);
			}

			rdTrackRooms(&pos, &prev, &lower, &upper, last, rd->piecerooms[i]);
			rdCoordToVec(&pos, rd->pieceprev[i]);
		}
	}

	ragdollUpdateBodyRooms(rd);
}

/**
 * chrTick (chr.c): the corpse is culled by its pelvis position; with cut
 * pieces, it's drawn if any piece is in view.
 */
s32 ragdollIsAnyPieceOnScreen(struct chrdata *chr, f32 size)
{
	RD_CRASHCTX("isanypieceonscreen", RD_CHRNUM(chr), -1);
	struct ragdoll *rd = ragdollFindByChr(chr);
	s32 i;

	if (rd == NULL || rd->prop == NULL) {
		return false;
	}

	// Every part is tested, not just the pelvis: a body lying through a
	// doorway with its pelvis behind a half-open door still has limbs in view.
	// (The dead player's body also moves away from the player's prop, which
	// stays where they died.) Each part is a smaller sphere than the body.
	//
	// The game tests a chr against the screen boxes of its rooms. Through a
	// door that's opening or closing the far room's box is just the gap, and
	// a body's rooms change as it moves, so bodies by doors popped in and
	// out. A body is drawn instead when any of its rooms is being drawn and
	// any part is inside the whole view: the z-buffer hides what's behind
	// doors and walls.
	{
		struct drawslot slot;
		struct player *player = g_Vars.currentplayer;
		s32 roomshown = false;

		for (i = 0; rd->prop->rooms[i] != -1 && i < ARRAYCOUNT(rd->prop->rooms); i++) {
			if (g_Rooms[rd->prop->rooms[i]].flags & ROOMFLAG_ONSCREEN) {
				roomshown = true;
				break;
			}
		}

		if (!roomshown || player == NULL || player->projectionmtx == NULL) {
			return false;
		}

		memset(&slot, 0, sizeof(slot));
		slot.box.xmin = player->c_screenleft;
		slot.box.ymin = player->c_screentop;
		slot.box.xmax = player->c_screenleft + player->c_screenwidth;
		slot.box.ymax = player->c_screentop + player->c_screenheight;

		size *= 0.5f;

		for (i = 0; i < rd->numparts; i++) {
			if (rd->skel->hasbody[i] || (rd->severed & (1u << i))) {
				struct coord pos;

				rdVecToCoord(rd->world[i].m[3], &pos);

				if (envIsPosInFogMaxDistance(&pos, size) && posIsInObjFadeDistance(&pos, size)
						&& camIsPosInScreenBox(&pos, size, &slot)) {
					return true;
				}
			}
		}
	}

	return false;
}

#endif // PD_PHYSICS
