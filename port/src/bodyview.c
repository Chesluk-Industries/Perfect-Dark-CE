/**
 * MOD: body awareness - the player's own body in first person (see
 * port/include/bodyview.h).
 */

#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "lib/model.h"
#include "lib/collision.h"
#include "game/gfxmemory.h"
#include "game/mtxf2lbulk.h"
#include "game/camera.h"
#include "game/chr.h"
#include "game/propobj.h"
#include "platform.h"
#include "config.h"
#include "system.h"
#include "utils.h"
#include "bodyview.h"

#define BODYVIEW_SLIDE      80.0f // drawn this much lower at BodyAwarenessPitch...
#define BODYVIEW_SLIDEANGLE 15.0f // ... rising fully over this many degrees further down
#define BODYVIEW_NECKHEIGHT 4.0f  // the neck above the view, standing (Joanna)
#define BODYVIEW_FOOTHEIGHT 26.0f // the lowest leg joint above the floor, standing (Joanna)
#define BODYVIEW_LEGSFOLLOW 0.5f  // the legs go back by this much of the upper body's push
#define BODYVIEW_LEGDEPTH   20.0f // kept between the legs and a wall behind
#define BODYVIEW_WALLHIDE   24.0f // closer than this to a wall the body isn't drawn...
#define BODYVIEW_WALLSHOW   8.0f  // ... until this much further out
#define BODYVIEW_MAXMTX     64

s32 g_BodyViewEnabled[4] = { 0, 0, 0, 0 };
static f32 g_BodyViewBack = 32.0f;
static f32 g_BodyViewDrop = 0.0f;
static f32 g_BodyViewPitch = 30.0f;

static bool g_BodyViewLoading = false;
static bool g_BodyViewOwns[MAX_PLAYERS];
static bool g_BodyViewWallHidden[MAX_PLAYERS];

// Restored by bodyViewEndDraw
static Mtxf *g_BodyViewMatrices;
static struct modelnode *g_BodyViewHeadspot;
static struct modelnode *g_BodyViewHeadChild;
static struct modeldef *g_BodyViewHeadDef;

static s32 bodyViewOn(struct player *player)
{
	return g_BodyViewEnabled[g_Vars.playerstats[playermgrGetPlayerNumByProp(player->prop)].mpindex & 3];
}

// Conditions shared by keeping the body (solo) and drawing it
static bool bodyViewAllowed(struct player *player)
{
	return player && player->prop && bodyViewOn(player)
		&& !player->isdead
		&& player->cameramode == CAMERAMODE_DEFAULT
		&& !g_Vars.in_cutscene
		&& g_Vars.stagenum < STAGE_TITLE;
}

bool bodyViewActive(s32 playernum)
{
	struct player *player = playernum >= 0 && playernum < MAX_PLAYERS ? g_Vars.players[playernum] : NULL;

	return bodyViewAllowed(player)
		&& player->haschrbody && player->model00d4
		&& player->visionmode != VISIONMODE_SLAYERROCKET
		&& g_Vars.tickmode == TICKMODE_NORMAL;
}

bool bodyViewWantSoloBody(void)
{
	return !g_Vars.mplayerisrunning && bodyViewAllowed(g_Vars.currentplayer);
}

bool bodyViewLoadingSeparately(void)
{
	return g_BodyViewLoading;
}

bool bodyViewOwnsBody(void)
{
	return g_BodyViewOwns[g_Vars.currentplayernum];
}

/**
 * Solo/co-op: loads the body as the Combat Simulator does (its own memory,
 * not the gun's). playerTickChrBody also sets the view height from it, as for
 * a cutscene's body.
 */
void bodyViewSoloTick(void)
{
	struct player *player = g_Vars.currentplayer;

	if (!player->haschrbody) {
		g_BodyViewLoading = true;
		playerTickChrBody();
		g_BodyViewLoading = false;

		g_BodyViewOwns[g_Vars.currentplayernum] = player->haschrbody;
	} else {
		playerTickChrBody();
	}
}

void bodyViewBodyRemoved(void)
{
	g_BodyViewOwns[g_Vars.currentplayernum] = false;
}

/**
 * What the owner's own pass in playerTickThirdPerson keeps up (floor, deleted
 * held weapons), which drawing the body in their view replaces. (Not the
 * cloak: chrTick in that branch updates it, and doing it here too used the
 * cloak twice as fast and recloaked in half the time.)
 */
void bodyViewOwnerTick(struct chrdata *chr, struct player *player)
{
	chr->ground = player->vv_ground;
	chr->manground = player->vv_manground;
	chr->sumground = chr->manground * (PAL ? 8.417509f : 9.999998f);

	if (g_Vars.mplayerisrunning) {
		s32 i;

		for (i = 0; i < 2; i++) {
			if (chr->weapons_held[i] && (chr->weapons_held[i]->obj->hidden & OBJHFLAG_DELETING)) {
				objFree(chr->weapons_held[i]->obj, true, false);
			}
		}
	}
}

bool bodyViewTracksWeapons(void)
{
	return !g_Vars.mplayerisrunning && g_BodyViewOwns[g_Vars.currentplayernum] && g_Vars.currentplayer->haschrbody;
}

bool bodyViewIsSilentBody(struct chrdata *chr)
{
	return chr && chr->prop && chr->prop->type == PROPTYPE_PLAYER
		&& bodyViewActive(playermgrGetPlayerNumByProp(chr->prop));
}

bool bodyViewIsOwnBody(struct chrdata *chr)
{
	return g_Vars.currentplayer && g_Vars.currentplayer->prop && chr == g_Vars.currentplayer->prop->chr
		&& bodyViewActive(g_Vars.currentplayernum);
}

/**
 * 0 at BodyAwarenessPitch degrees down or less (not drawn), up to 1 once
 * looking BODYVIEW_SLIDEANGLE further down
 */
static f32 bodyViewLookDown(void)
{
	f32 down = -g_Vars.currentplayer->vv_verta - g_BodyViewPitch;

	if (g_BodyViewPitch <= 0.0f) {
		return 1.0f;
	}

	return down <= 0.0f ? 0.0f : fminf(down / BODYVIEW_SLIDEANGLE, 1.0f);
}

/**
 * How far back the legs can go before a wall: lines back from the player at
 * knee and hip height
 */
static f32 bodyViewRoomBehind(const f32 *fwd, f32 want)
{
	static const f32 heights[] = { 30.0f, 90.0f };
	struct player *player = g_Vars.currentplayer;
	f32 room = want;
	s32 i;

	for (i = 0; i < ARRAYCOUNT(heights); i++) {
		struct coord from, to, hit;

		from.x = player->prop->pos.x;
		from.y = player->vv_manground + heights[i];
		from.z = player->prop->pos.z;
		to.x = from.x - fwd[0] * (want + BODYVIEW_LEGDEPTH);
		to.y = from.y;
		to.z = from.z - fwd[1] * (want + BODYVIEW_LEGDEPTH);

		if (cdExamLos08(&from, player->prop->rooms, &to, CDTYPE_BG | CDTYPE_DOORS, GEOFLAG_WALL) == CDRESULT_COLLISION) {
			cdGetPos(&hit, __LINE__, __FILE__);
			room = fminf(room, sqrtf((hit.x - from.x) * (hit.x - from.x) + (hit.z - from.z) * (hit.z - from.z)) - BODYVIEW_LEGDEPTH);
		}
	}

	return fmaxf(room, 0.0f);
}

// A matrix's position relative to the view in world axes (the camera's rotation undone)
static void bodyViewToWorld(Mtxf *cam, f32 scale, Mtxf *mtx, f32 *out)
{
	s32 c, r;

	for (c = 0; c < 3; c++) {
		out[c] = 0.0f;

		for (r = 0; r < 3; r++) {
			out[c] += cam->m[c][r] * mtx->m[3][r];
		}

		out[c] /= scale * scale;
	}
}

/**
 * Places the body in the view (the matrices are in view space). It goes
 * BodyAwarenessBack behind the view along its facing and BodyAwarenessDrop
 * lower, plus the slide in past BodyAwarenessPitch.
 *
 * - Crouch animations lean the neck forward: the upper body goes back by that
 *   much more, the legs by BODYVIEW_LEGSFOLLOW of it.
 * - The full crouch animation stands taller than the crouched view: the body
 *   goes lower by how far its neck is above standing height, but only while
 *   its feet stay above the floor; the rest goes back instead.
 * - A wall behind limits how far back the body goes; when that brings it
 *   under the view (the chest would fill it) it isn't drawn.
 *
 * Returns false if the body shouldn't be drawn.
 */
static bool bodyViewAnchor(Mtxf *matrices, s32 count, s32 neck, const u8 *upper)
{
	struct player *player = g_Vars.currentplayer;
	bool *wallhidden = &g_BodyViewWallHidden[g_Vars.currentplayernum];
	Mtxf *cam = camGetWorldToScreenMtxf();
	f32 angle = (360.0f - player->vv_theta) * (M_BADTAU / 360.0f);
	f32 fwd[2] = { sinf(angle), cosf(angle) };
	f32 scale = sqrtf(cam->m[0][0] * cam->m[0][0] + cam->m[0][1] * cam->m[0][1] + cam->m[0][2] * cam->m[0][2]);
	f32 ahead = 0.0f;
	f32 above = 0.0f;
	f32 lowest = 0.0f;
	f32 pos[3];
	f32 drop, legsback, upback, wallmin, room;
	f32 offset[2][3];
	s32 i, r;

	if (scale <= 0.0f) {
		return false;
	}

	if (neck >= 0) {
		bodyViewToWorld(cam, scale, &matrices[neck], pos);
		ahead = fmaxf(pos[0] * fwd[0] + pos[2] * fwd[1], 0.0f);
		above = fmaxf(pos[1] - BODYVIEW_NECKHEIGHT, 0.0f);
	}

	for (i = 0; i < count; i++) {
		if (!upper[i]) {
			bodyViewToWorld(cam, scale, &matrices[i], pos);
			lowest = fminf(lowest, pos[1]);
		}
	}

	// (the view's height above the floor + the lowest leg joint's below it)
	drop = fminf(above, fmaxf(player->prop->pos.y - player->vv_manground + lowest - BODYVIEW_FOOTHEIGHT, 0.0f));
	legsback = g_BodyViewBack + ahead * BODYVIEW_LEGSFOLLOW + (above - drop);
	upback = legsback + ahead * (1.0f - BODYVIEW_LEGSFOLLOW);
	room = bodyViewRoomBehind(fwd, legsback);

	if (legsback > room) {
		upback -= legsback - room;
		legsback = room;
		wallmin = BODYVIEW_WALLHIDE + ahead * (1.0f - BODYVIEW_LEGSFOLLOW);
		*wallhidden = upback < wallmin || (*wallhidden && upback < wallmin + BODYVIEW_WALLSHOW);
	} else {
		*wallhidden = false;
	}

	if (*wallhidden) {
		return false;
	}

	drop += g_BodyViewDrop + BODYVIEW_SLIDE * (1.0f - bodyViewLookDown());

	for (r = 0; r < 3; r++) {
		f32 back = fwd[0] * cam->m[0][r] + fwd[1] * cam->m[2][r];

		offset[0][r] = -drop * cam->m[1][r] - legsback * back;
		offset[1][r] = -drop * cam->m[1][r] - upback * back;
	}

	for (i = 0; i < count; i++) {
		for (r = 0; r < 3; r++) {
			matrices[i].m[3][r] += offset[upper[i] ? 1 : 0][r];
		}
	}

	return true;
}

// A position node's matrix indices (1 to 3)
static s32 bodyViewNodeMatrices(struct modelnode *node, s32 count, s32 *out)
{
	struct modelrodata_position *pos = &node->rodata->position;
	s32 n = 0;

	if (pos->mtxindex0 >= 0 && pos->mtxindex0 < count) {
		out[n++] = pos->mtxindex0;
	}

	if ((node->type & MODELNODETYPE_0100) && pos->mtxindex1 >= 0 && pos->mtxindex1 < count) {
		out[n++] = pos->mtxindex1;
	}

	if ((node->type & MODELNODETYPE_0200) && pos->mtxindex2 >= 0 && pos->mtxindex2 < count) {
		out[n++] = pos->mtxindex2;
	}

	return n;
}

/**
 * Chr skeleton parts: 0 pelvis, 1 chest, 2 neck, 3-8 arms, 9- legs. Marks the
 * upper body (1-8) and shrinks the neck and arms. All of a limb's shrunk matrices go to one point, its first shrunk
 * joint: left at their own joints, the triangles between them showed as thin
 * lines. The head model (headspot) is skipped.
 */
static void bodyViewShrinkNodes(Mtxf *matrices, s32 count, struct modelnode *node, s32 *neck, u8 *upper, const f32 *pin)
{
	for (; node; node = node->next) {
		const f32 *childpin = pin;
		f32 ownpin[3];
		s32 type = node->type & 0xff;

		if (type == MODELNODETYPE_HEADSPOT) {
			continue;
		}

		if (type == MODELNODETYPE_POSITION) {
			s32 part = node->rodata->position.part;
			s32 mtxs[3];
			s32 n = bodyViewNodeMatrices(node, count, mtxs);
			s32 i, c, r;

			if (part == 2 && n > 0) {
				*neck = mtxs[0];
			}

			if (part >= 1 && part <= 8) {
				for (i = 0; i < n; i++) {
					upper[mtxs[i]] = true;
				}
			}

			if (n > 0 && part >= 2 && part <= 8) {
				if (childpin == NULL) {
					memcpy(ownpin, matrices[mtxs[0]].m[3], sizeof(ownpin));
					childpin = ownpin;
				}

				for (i = 0; i < n; i++) {
					for (c = 0; c < 3; c++) {
						for (r = 0; r < 3; r++) {
							matrices[mtxs[i]].m[c][r] *= 0.0001f;
						}
					}

					memcpy(matrices[mtxs[i]].m[3], childpin, sizeof(ownpin));
				}
			}
		}

		bodyViewShrinkNodes(matrices, count, node->child, neck, upper, childpin);
	}
}

/**
 * The display list reads the matrices when the frame is drawn, so it gets a
 * copy (this frame's graphics memory) placed and shrunk; the model's own are
 * left as they are (the shot checks read them). The head model is detached.
 */
bool bodyViewBeginDraw(struct model *model)
{
	struct modeldef *def = model->definition;
	struct modelnode *headspot;
	s32 count = def->nummatrices;
	s32 neck = -1;
	u8 upper[BODYVIEW_MAXMTX] = { 0 };
	Mtxf *copy;

	if (bodyViewLookDown() <= 0.0f || def->skel != &g_SkelChr || count > BODYVIEW_MAXMTX || model->matrices == NULL) {
		return false;
	}

	copy = gfxAllocate(count * sizeof(Mtxf));
	memcpy(copy, model->matrices, count * sizeof(Mtxf));
	bodyViewShrinkNodes(copy, count, def->rootnode, &neck, upper, NULL);

	if (!bodyViewAnchor(copy, count, neck, upper)) {
		return false;
	}

	g_BodyViewMatrices = model->matrices;
	model->matrices = copy;

	headspot = modelGetPart(def, MODELPART_CHR_HEADSPOT);
	g_BodyViewHeadspot = headspot;

	if (headspot) {
		struct modelrwdata_headspot *rw = modelGetNodeRwData(model, headspot);

		g_BodyViewHeadChild = headspot->child;
		g_BodyViewHeadDef = rw->headmodeldef;
		rw->headmodeldef = NULL;
		headspot->child = NULL;
	}

	return true;
}

void bodyViewEndDraw(struct model *model)
{
	// (in the fixed point format the display list reads, as chrRender does
	// with the model's own after its last pass)
	mtxF2LBulk(model->matrices, model->definition->nummatrices);
	model->matrices = g_BodyViewMatrices;

	if (g_BodyViewHeadspot) {
		struct modelrwdata_headspot *rw = modelGetNodeRwData(model, g_BodyViewHeadspot);

		rw->headmodeldef = g_BodyViewHeadDef;
		g_BodyViewHeadspot->child = g_BodyViewHeadChild;
	}
}

PD_CONSTRUCTOR static void bodyViewConfigInit(void)
{
	s32 i;

	for (i = 0; i < 4; i++) {
		configRegisterInt(strFmt("Game.Player%d.BodyAwareness", i + 1), &g_BodyViewEnabled[i], 0, 1);
	}

	configRegisterFloat("Game.BodyAwarenessBack", &g_BodyViewBack, 0.0f, 80.0f);
	configRegisterFloat("Game.BodyAwarenessDrop", &g_BodyViewDrop, 0.0f, 60.0f);
	configRegisterFloat("Game.BodyAwarenessPitch", &g_BodyViewPitch, 0.0f, 80.0f);
}
