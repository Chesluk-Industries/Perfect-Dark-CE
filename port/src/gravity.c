/**
 * MOD: gravity - the gravity setting and its menu (see port/include/gravity.h).
 */

#ifdef PD_GRAVITY

#include <stdio.h>
#include <math.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "game/menu.h"
#include "config.h"
#include "system.h"
#include "gravity.h"
#include "bss.h"
#include "game/bg.h"
#ifdef PD_PHYSICS
#include "pdphysics.h"
#endif

#define GRAVITY_MAX     500
#define GRAVITY_FINE    100  // between -GRAVITY_FINE and GRAVITY_FINE the slider goes by 1%, past it by GRAVITY_STEP
#define GRAVITY_MIN     -500 // Gravity (ragdolls, debris): below 0 it's inverted
#define GRAVITY_STEP    5
#define GRAVITY_DEFAULT 100
#define GRAVITY_PLAYER_MIN 5 // Player Gravity starts here: at 0% anyone stepping off a ledge floated for good

s32 g_GravityPercent = GRAVITY_DEFAULT;
s32 g_PlayerGravityPercent = GRAVITY_DEFAULT;

static s32 g_GravityLogged = -1;
static s32 g_PlayerGravityLogged = -1;

#ifdef PD_PHYSICS
// The ceiling each held item was last put under (by projectile), so a small
// push past the surface (the game's own collision moving it) doesn't let it go
#define GRAVITY_MAXHELD 256

static struct defaultobj *g_HeldObj[GRAVITY_MAXHELD];
static f32 g_HeldCeilY[GRAVITY_MAXHELD];
#endif

PD_CONSTRUCTOR static void gravityConfigInit(void)
{
	configRegisterInt("Game.Gravity", &g_GravityPercent, GRAVITY_MIN, GRAVITY_MAX);
	configRegisterInt("Game.PlayerGravity", &g_PlayerGravityPercent, GRAVITY_PLAYER_MIN, GRAVITY_MAX);
}

static void gravityLogChange(void)
{
	if (g_GravityLogged != g_GravityPercent) {
		g_GravityLogged = g_GravityPercent;
		sysLogPrintf(LOG_NOTE, "GRAVITY: ragdolls and debris at %d%% of the original", g_GravityPercent);
	}

	if (g_PlayerGravityLogged != g_PlayerGravityPercent) {
		g_PlayerGravityLogged = g_PlayerGravityPercent;
		sysLogPrintf(LOG_NOTE, "GRAVITY: players and characters at %d%% of the original", g_PlayerGravityPercent);
	}
}

void gravityStageStart(void)
{
#ifdef PD_PHYSICS
	s32 i;

	for (i = 0; i < GRAVITY_MAXHELD; i++) {
		g_HeldObj[i] = NULL;
	}
#endif


	if (g_GravityPercent != GRAVITY_DEFAULT || g_PlayerGravityPercent != GRAVITY_DEFAULT) {
		sysLogPrintf(LOG_NOTE, "GRAVITY: level starts with gravity %d%% (ragdolls, debris), player gravity %d%% (players, characters)",
				g_GravityPercent, g_PlayerGravityPercent);
	}
}

bool gravityIsDefault(void)
{
	return g_GravityPercent == GRAVITY_DEFAULT && g_PlayerGravityPercent == GRAVITY_DEFAULT;
}

void gravityResetDefaults(void)
{
	g_GravityPercent = GRAVITY_DEFAULT;
	g_PlayerGravityPercent = GRAVITY_DEFAULT;
	gravityLogChange();
}

#ifdef PD_PHYSICS
#define GRAVMIN2(a, b) ((a) < (b) ? (a) : (b))
#define GRAVMAX2(a, b) ((a) > (b) ? (a) : (b))

static void gravityRoomListAdd(s32 *list, s32 *num, s32 max, s32 room)
{
	s32 i;

	if (room <= 0 || room >= g_Vars.roomcount || *num >= max) {
		return;
	}

	for (i = 0; i < *num; i++) {
		if (list[i] == room) {
			return;
		}
	}

	list[(*num)++] = room;
}
#endif

#ifdef PD_PHYSICS
/**
 * The rooms whose level surfaces an item moving from 'from' to 'to' meets, as
 * for ragdolls (ragdollUpdateCollisionRooms): its own rooms, those reached
 * through portals around the path, and those on screen whose space the path
 * reaches (a ceiling often belongs to the room above).
 */
static s32 gravityItemRooms(struct prop *prop, f32 *from, f32 *to, s32 *ints)
{
	RoomNum entered[24];
	struct coord lower, upper;
	s32 n = 0;
	s32 i;

	lower.x = GRAVMIN2(from[0], to[0]) - 30.0f;
	lower.y = GRAVMIN2(from[1], to[1]) - 30.0f;
	lower.z = GRAVMIN2(from[2], to[2]) - 30.0f;
	upper.x = GRAVMAX2(from[0], to[0]) + 30.0f;
	upper.y = GRAVMAX2(from[1], to[1]) + 30.0f;
	upper.z = GRAVMAX2(from[2], to[2]) + 30.0f;

	for (i = 0; i < 8 && prop->rooms[i] != -1; i++) {
		gravityRoomListAdd(ints, &n, 32, prop->rooms[i]);
	}

	if (n > 0) {
		entered[0] = ints[0];
		entered[1] = -1;
		bgFindEnteredRooms(&lower, &upper, entered, 20, false);

		for (i = 0; i < 20 && entered[i] != -1; i++) {
			gravityRoomListAdd(ints, &n, 32, entered[i]);
		}
	}

	// Rooms on screen just now whose space the path reaches (like ragdolls)
	for (i = 1; i < g_Vars.roomcount && n < 32; i++) {
		if (g_Rooms[i].loaded240 <= 0 || g_Rooms[i].loaded240 > 60
				|| g_Rooms[i].bbmin[0] > upper.x || g_Rooms[i].bbmax[0] < lower.x
				|| g_Rooms[i].bbmin[1] > upper.y || g_Rooms[i].bbmax[1] < lower.y
				|| g_Rooms[i].bbmin[2] > upper.z || g_Rooms[i].bbmax[2] < lower.z) {
			continue;
		}

		gravityRoomListAdd(ints, &n, 32, i);
	}

	return n;
}
#endif

/**
 * Inverted gravity: is there a ceiling between the item and where it's
 * headed (topos)? The same check as ragdolls: the level surfaces they collide
 * with (the generated ceilings, or the game's tiles and seen rooms' render
 * surfaces), in the rooms around the item (reached through portals, and those
 * on screen), since a ceiling often belongs to the room above. Fills outy with
 * where the item stays (under the surface).
 */
bool gravityItemCeiling(struct prop *prop, struct projectile *projectile, struct coord *topos, f32 *outy)
{
#ifdef PD_PHYSICS
	f32 from[3] = { prop->pos.x, prop->pos.y, prop->pos.z };
	f32 to[3] = { topos->x, topos->y + 15.0f, topos->z };
	f32 hit[3];
	f32 normal[3];
	s32 ints[32];
	s32 index = projectile - g_Projectiles;
	s32 held = -1;
	s32 n;

	if (index >= 0 && index < GRAVITY_MAXHELD && index < g_MaxProjectiles) {
		held = index;

		if (g_HeldObj[held] != prop->obj) {
			g_HeldObj[held] = NULL;
		}
	}

	// Under a ceiling already: look from just below it, in case the item was
	// pushed a little past the surface
	if (held >= 0 && g_HeldObj[held]
			&& from[1] > g_HeldCeilY[held] - 5.0f && from[1] < g_HeldCeilY[held] + 30.0f) {
		from[1] = g_HeldCeilY[held] - 5.0f;
	}

	if (to[1] <= from[1]) {
		return false;
	}

	n = gravityItemRooms(prop, from, to, ints);

	// (A surface well below the item: it's past it already, it isn't pulled back)
	if (pdphysRaycastRagdollWorldRooms(ints, n, from, to, hit, normal, NULL) && normal[1] < -0.3f
			&& hit[1] > prop->pos.y - 30.0f) {
		*outy = hit[1] - 15.0f;

		if (held >= 0) {
			g_HeldObj[held] = prop->obj;
			g_HeldCeilY[held] = hit[1];
		}

		return true;
	}

	if (held >= 0) {
		g_HeldObj[held] = NULL;
	}
#endif

	return false;
}

/**
 * Gravity not the original: an item flying sideways bounces off the walls
 * ragdolls collide with (the generated ones), where the game's collision has
 * none. Moves topos back in front of the wall and turns the speed away from it.
 */
bool gravityItemWall(struct prop *prop, struct coord *topos, struct coord *speed)
{
#ifdef PD_PHYSICS
	f32 from[3] = { prop->pos.x, prop->pos.y, prop->pos.z };
	f32 to[3] = { topos->x, topos->y, topos->z };
	f32 hit[3];
	f32 normal[3];
	f32 dx = to[0] - from[0];
	f32 dz = to[2] - from[2];
	f32 len = sqrtf(dx * dx + dz * dz);
	s32 ints[32];
	s32 n;

	if (len < 0.01f) {
		return false;
	}

	// A little past the end, so the item doesn't stop inside the wall
	to[0] += dx / len * 10.0f;
	to[2] += dz / len * 10.0f;

	n = gravityItemRooms(prop, from, to, ints);

	if (pdphysRaycastRagdollWorldRooms(ints, n, from, to, hit, normal, NULL)
			&& normal[1] > -0.5f && normal[1] < 0.5f) {
		f32 nlen = sqrtf(normal[0] * normal[0] + normal[2] * normal[2]);
		f32 nx = normal[0] / nlen;
		f32 nz = normal[2] / nlen;
		f32 into = speed->x * nx + speed->z * nz;

		topos->x = hit[0] + nx * 10.0f;
		topos->z = hit[2] + nz * 10.0f;

		// Bounce off with half the speed into the wall
		if (into < 0.0f) {
			speed->x -= 1.5f * into * nx;
			speed->z -= 1.5f * into * nz;
		}

		return true;
	}
#endif

	return false;
}

/**
 * Slider positions: GRAVITY_STEP apart from min up to -GRAVITY_FINE, 1%
 * apart up to GRAVITY_FINE, then GRAVITY_STEP apart up to GRAVITY_MAX
 * (Player Gravity starts inside the fine part, at GRAVITY_PLAYER_MIN).
 */
// The last slider position (GRAVITY_MAX) for a slider starting at min
#define GRAVITY_SLIDER_MAX(min) (((min) < -GRAVITY_FINE ? (-GRAVITY_FINE - (min)) / GRAVITY_STEP + 2 * GRAVITY_FINE : GRAVITY_FINE - (min)) \
		+ (GRAVITY_MAX - GRAVITY_FINE) / GRAVITY_STEP)

static s32 gravityFineStart(s32 min)
{
	return min < -GRAVITY_FINE ? -GRAVITY_FINE : min;
}

static s32 gravityCoarseBelow(s32 min)
{
	return min < -GRAVITY_FINE ? (-GRAVITY_FINE - min) / GRAVITY_STEP : 0;
}

static s32 gravityFromSlider(s32 index, s32 min)
{
	s32 below = gravityCoarseBelow(min);
	s32 fine = gravityFineStart(min);

	if (index <= below) {
		return min + index * GRAVITY_STEP;
	}

	index -= below;

	if (fine + index <= GRAVITY_FINE) {
		return fine + index;
	}

	return GRAVITY_FINE + (index - (GRAVITY_FINE - fine)) * GRAVITY_STEP;
}

static s32 gravityToSlider(s32 percent, s32 min)
{
	s32 below = gravityCoarseBelow(min);
	s32 fine = gravityFineStart(min);

	if (percent < fine) {
		return (percent - min + GRAVITY_STEP / 2) / GRAVITY_STEP;
	}

	if (percent <= GRAVITY_FINE) {
		return below + percent - fine;
	}

	return below + (GRAVITY_FINE - fine) + (percent - GRAVITY_FINE + GRAVITY_STEP / 2) / GRAVITY_STEP;
}

// item->param is 1 for Player Gravity (from GRAVITY_PLAYER_MIN); Gravity starts at GRAVITY_MIN
static MenuItemHandlerResult menuhandlerGravity(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 *setting = item->param ? &g_PlayerGravityPercent : &g_GravityPercent;
	s32 min = item->param ? GRAVITY_PLAYER_MIN : GRAVITY_MIN;
	s32 percent;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = gravityToSlider(*setting, min);
		break;
	case MENUOP_SET:
		*setting = gravityFromSlider(data->slider.value, min);
		gravityLogChange();
		break;
	case MENUOP_GETSLIDERLABEL:
		percent = gravityFromSlider(data->slider.value, min);

		if (percent == GRAVITY_DEFAULT) {
			sprintf(data->slider.label, "100%% (Original)");
		} else if (percent == 0) {
			sprintf(data->slider.label, "0%% (None)");
		} else if (percent < 0) {
			sprintf(data->slider.label, "%d%% (Inverted)", percent);
		} else {
			sprintf(data->slider.label, "%d%%", percent);
		}
		break;
	}

	return 0;
}

static struct menuitem g_GravityMenuItems[] = {
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Gravity", GRAVITY_SLIDER_MAX(GRAVITY_MIN), menuhandlerGravity },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LABEL_ALTCOLOUR, (uintptr_t)"Ragdolls, items, casings and debris.\n", 0, NULL },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SLIDER, 1, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Player Gravity", GRAVITY_SLIDER_MAX(GRAVITY_PLAYER_MIN), menuhandlerGravity },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LABEL_ALTCOLOUR, (uintptr_t)"Players, guards and simulants falling.\n", 0, NULL },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LABEL_ALTCOLOUR, (uintptr_t)"Thrown and fired objects keep the original.\n", 0, NULL },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_GravityMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Gravity",
	g_GravityMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

#endif
