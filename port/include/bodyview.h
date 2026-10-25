#ifndef _IN_BODYVIEW_H
#define _IN_BODYVIEW_H

#include <PR/ultratypes.h>
#include "types.h"

/**
 * MOD: body awareness - in first person the player sees their own body when
 * looking down (legs and torso; the head, arms and the body's gun are hidden,
 * the first person gun is drawn as usual). Per player, off by default:
 *   Game.PlayerN.BodyAwareness  0 off, 1 on (an old 2, legs only, loads as on)
 *   Game.BodyAwarenessBack      units the body is drawn behind the view (pd.ini)
 *   Game.BodyAwarenessDrop      units the body is drawn lower (pd.ini)
 *   Game.BodyAwarenessPitch     degrees down before it's drawn (pd.ini)
 *
 * The Combat Simulator already animates every player's body: it's drawn in
 * its owner's view too. Solo and co-op only load the body for cutscenes and
 * death (in the gun's memory): with this on it's loaded on its own instead.
 */

extern s32 g_BodyViewEnabled[4];

// The player's body is drawn in their own first person view
bool bodyViewActive(s32 playernum);

// player.c: solo/co-op body loading
bool bodyViewWantSoloBody(void);
bool bodyViewLoadingSeparately(void);
bool bodyViewOwnsBody(void);
void bodyViewSoloTick(void);
void bodyViewBodyRemoved(void);
void bodyViewOwnerTick(struct chrdata *chr, struct player *player);

// bondgun.c: the solo body takes the gun into its hand on a switch
bool bodyViewTracksWeapons(void);

// chraTick: no footsteps from it
bool bodyViewIsSilentBody(struct chrdata *chr);

// chrRender: the current player's own body (no shadow or cloak/shield
// effects); BeginDraw returns false if it isn't drawn this frame
bool bodyViewIsOwnBody(struct chrdata *chr);
bool bodyViewBeginDraw(struct model *model);
void bodyViewEndDraw(struct model *model);

#endif
