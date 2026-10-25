#ifndef _IN_GRAVITY_H
#define _IN_GRAVITY_H

#include <PR/ultratypes.h>
#include "types.h"

/**
 * MOD: gravity - gravity settings for fun.
 *   Game.Gravity        -500% to 500%: ragdolls (the physics engine's
 *                       gravity), items and the cosmetic bits: bullet
 *                       casings, sparks, glass shards. Below 0 it's
 *                       inverted: they fall upwards, to the ceiling
 *                       (bodies and items under open sky leave the level
 *                       and are removed, casings vanish once well above
 *                       the floor)
 *   Game.PlayerGravity  how fast players, guards and simulants fall (from 5%)
 * Both in percent of the original (100 = original). Items follow Gravity
 * too (dropped guns, things knocked loose or blown off); what players,
 * simulants and guards throw or fire keeps the original gravity (their
 * arcs are gameplay). Below 100% Gravity, falling bodies don't scream.
 *
 * The sliders go by 1% from -100% to 100% and by 5% beyond.
 *
 * Menus: Extended > Physics > Gravity, and Combat Simulator > Options >
 * More Options > Gravity.
 */

// A projectile thrown or fired by a player or character (bgun0f09ebcc, a
// guard's grenade): it keeps the original gravity. Cleared when it falls
// as an item (knocked loose) and on reuse (flags = 0).
#define PROJECTILEFLAG_LAUNCHED 0x20000000

extern s32 g_GravityPercent;
extern s32 g_PlayerGravityPercent;

// Game.Gravity: 1.0 at the original gravity
static inline f32 gravityScale(void)
{
	return g_GravityPercent * 0.01f;
}

// Game.PlayerGravity: 1.0 at the original gravity
static inline f32 gravityPlayerScale(void)
{
	return g_PlayerGravityPercent * 0.01f;
}

extern struct menudialogdef g_GravityMenuDialog;

// The physics Preset: Default puts both back to 100%, otherwise it shows Custom
bool gravityIsDefault(void);
void gravityResetDefaults(void);

// propobj.c: an item rising to topos (not thrown or fired) stops under a
// ceiling (the same check as ragdolls): true and the height to stop at
struct prop;
struct projectile;
bool gravityItemCeiling(struct prop *prop, struct projectile *projectile, struct coord *topos, f32 *outy);
// propobj.c: with gravity not the original, an item moving to topos bounces
// off the walls ragdolls collide with: true if it hit one (topos and speed
// changed)
bool gravityItemWall(struct prop *prop, struct coord *topos, struct coord *speed);

// lv.c: logs the gravity at each level start when it isn't the original
void gravityStageStart(void);

#endif
