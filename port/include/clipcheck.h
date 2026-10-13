#ifndef _IN_CLIPCHECK_H
#define _IN_CLIPCHECK_H

/*
 * MOD: clipping checks - more robust clipping from the level's render
 * geometry (what is actually drawn), for when the game's own collision data
 * (floor/wall tiles) is missing or doesn't reach far enough:
 *   - floors: anything drawn facing up can be stood on, so surfaces without
 *     collision tiles don't drop the player through the map
 *   - ceilings: anything drawn facing down stops upward movement (jumps,
 *     mods that lift the player), so heads don't go through ceilings
 *   - walls: anything drawn steep stops walking/jumping through it (where
 *     the game has no wall tiles, or above their tops); the game slides along it
 *
 * The surfaces live in the physics world (PD_PHYSICS) as query-only bodies
 * (PDPHYS_QUERY_GROUP_FIRST and up): nothing in the simulation collides with
 * them, so ragdolls and props behave exactly as before. Other modules can use
 * the query functions for their own checks (players, chrs, objects, cameras).
 *
 * Built when PD_CLIPCHECK is on (CMake option, needs PD_PHYSICS). Every hook
 * in the game is wrapped in #ifdef PD_CLIPCHECK and tagged MOD: clipping checks.
 */

#include <PR/ultratypes.h>
#include "types.h"

// Physics level groups (query-only, see pdphysics.h)
#define CLIPCHECK_GROUP_FLOORS   8  // render triangles facing up (normal.y >= 0.35)
#define CLIPCHECK_GROUP_CEILINGS 9  // render triangles facing down (normal.y <= -0.35)
#define CLIPCHECK_GROUP_WALLS    10 // the rest (steep): for segment checks
#define CLIPCHECK_GROUP_GLASS    11 // glass panes without collision of their own (as walls)
#define CLIPCHECK_GROUP_LIFTS    12 // lifts' walls and undersides, moving with them (as walls)

// Settings (pd.ini)
enum {
	CLIPMODE_GAME,      // the game's own collision only (as the original)
	CLIPMODE_GENERATED, // generated from the drawn geometry only (the game's level tiles off)
	CLIPMODE_BOTH,      // the game's collision, with generated surfaces filling in where it has none
};

extern s32 g_ClipPlayerMode;     // Game.ClipPlayerMode: CLIPMODE_* for the player (default the game's; experimental)
extern s32 g_ClipRenderFloors;   // Game.ClipRenderFloors: stand on drawn surfaces without collision
extern s32 g_ClipRenderCeilings; // Game.ClipRenderCeilings: drawn ceilings stop upward movement
extern s32 g_ClipRenderWalls;    // Game.ClipRenderWalls: drawn walls stop horizontal movement
extern s32 g_ClipDebug;          // Game.ClipDebug: 0 off, 1 drawn surfaces, 2 the game's tiles, 3 both
extern f32 g_ClipMaxCatchFall;   // Game.ClipMaxCatchFall: a drawn floor further below than this
                                 // (from where the fall began) doesn't catch: pits stay deadly

// lv.c
void clipcheckStageLoad(void);   // after ragdollStageLoad (which clears the level world)
void clipcheckTick(void);        // adds the surfaces of rooms as they get loaded
Gfx *clipcheckRenderDebug(Gfx *gdl); // with the world effects: the debug view

// Queries (PD units). A sample is a vertical ray at x/z, plus four more
// around it at 'radius' (0: just the one).
s32 clipcheckIsReady(void);
s32 clipcheckWaterLevel(const RoomNum *rooms, f32 x, f32 z, f32 ylo, f32 yhi, f32 *outy);
// Highest floor (normal.y >= 0.7) between fromy (above) and toy (below)
s32 clipcheckFindFloor(f32 x, f32 z, f32 radius, f32 fromy, f32 toy, f32 *outy);
// Lowest ceiling between fromy (below) and toy (above)
s32 clipcheckFindCeiling(f32 x, f32 z, f32 radius, f32 fromy, f32 toy, f32 *outy);
// How far (0..1) a centre can move from -> to at height y before coming
// within a margin of a drawn wall; the wall's horizontal normal is filled
f32 clipcheckWallFraction(const struct coord *from, const struct coord *to, f32 y, f32 *outnx, f32 *outnz);
// Same for a whole body: a vertical capsule of 'radius' from y low to top
f32 clipcheckBodyFraction(const struct coord *from, const struct coord *to, f32 low, f32 top, f32 radius, f32 *outnx, f32 *outnz);
// Any drawn surface between a and b (groupmask: 1 << CLIPCHECK_GROUP_*, 0 = all)
s32 clipcheckSegment(const struct coord *a, const struct coord *b, u32 groupmask, struct coord *hit, struct coord *normal);

// bondwalk.c: the player
s32 clipcheckPlayerCdTypes(s32 types);
f32 clipcheckPlayerGround(f32 ground, s32 inlift);
s32 clipcheckPlayerCeilingBlocks(f32 amount);
s32 clipcheckPlayerMoveBlocked(struct coord *from, struct coord *to, f32 ymax);

#endif
