/*
 * MOD: clipping checks - see clipcheck.h.
 *
 * Each room's render geometry is only in memory while the room is loaded
 * (visible or recently visible), so its surfaces are added to the physics
 * world the first time it's seen loaded and kept until the stage ends (the
 * same way the ragdoll module adds its render walls).
 *
 * Only opaque batches count: translucent ones are glass, light beams and
 * holograms, which the player may well walk through.
 */
#ifdef PD_CLIPCHECK

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "game/bg.h"
#include "game/prop.h"
#include "game/player.h"
#include "lib/collision.h"
#include "lib/model.h"
#include "platform.h"
#include "config.h"
#include "system.h"
#include "clipcheck.h"
#include "ragdoll.h"
#include "pdphysics.h"
#include "game/propobj.h"
#include "fs.h"
#include "clipcheck/clip_internal.h"

void cdClearResults(void); // collision.c
void cdCollectGeoForCyl(struct coord *pos, f32 radius, RoomNum *rooms, u32 types, u16 geoflags, bool checkvertical, f32 ymax, f32 ymin, struct collision *collisions, s32 maxcollisions);
f32 cdFindGroundFromList(struct collision *collisions, struct coord *pos, struct collision **collisionptr, f32 width);
void cdSetObstacleVtxPropFlt(struct coord *vtx1, struct coord *vtx2, struct prop *prop, f32 arg3);

#define CLIP_VTXBATCHTYPE_OPA 0x01 // as in bg.c
#define CLIP_GLASS_MINTHICK   14.0f  // glass panes without collision become boxes at least this thick
#define CLIP_FLOORFILL_DY     16.0f  // a game floor tile with a drawn floor this close (above or below) is drawn
#define CLIP_XLUWALL_THICK    12.0f  // translucent solid walls (railings) are made this thick
#define CLIP_FLOOR_MINUP      0.35f  // added as a floor when normal.y >= this
#define CLIP_FLOOR_WALKABLE   0.7f   // stood on when the hit normal.y >= this (45 degrees)
#define CLIP_CEIL_MINDOWN     0.35f  // added as a ceiling when normal.y <= -this
#define CLIP_ROOMSPERTICK     4

#define CLIP_VTXSLOTS         32     // vertex cache slots (a load of up to 16 at slot 0..15)
#define CLIP_MAXMOVEREACH     60.0f  // the most a move per frame adds to the step reach
#define CLIP_STEP             30.0f  // the player steps up onto drawn surfaces this high
#define CLIP_PIT_MIN          8.0f   // generated mode: the game's floor this far above the drawn one is a covered pit
#define CLIP_STEP_WALKABLE    48.0f  // ...and onto flat (walkable) tops this high: out of a pool,
                                     // a gap, onto a kerb (the game pulls the feet up 50 a frame)
#define CLIP_STAIR_TOLERANCE  16.0f  // drawn steps this close above the game's floor (its
                                     // smooth stair ramps) are left to the game
#define CLIP_HEAD_SINK        8.0f   // ceiling rays start this far below the top of the head
#define CLIP_WALL_LOW         40.0f  // lowest wall ray above the feet (stair risers and curbs
                                     // below this are stepped up, not walls)
#define CLIP_WALL_MARGIN      12.0f  // the body's centre stays this far from drawn walls
                                     // (the game's own walls keep it a full radius away)
#define CLIP_WALL_STEEP       0.7f   // floors steeper than this (normal.y below) block like walls
#define CLIP_WALL_OVERHANG    -0.7f  // ... and overhangs with normal.y above this
#define CLIP_PD_GROUPS        ((1u << 0) | (1u << 1)) // the game's own floor and wall tiles in the
                                     // level world (ragdoll.c RD_GROUP_FLOORS, RD_GROUP_WALLS)
#define CLIP_COVER_DIST       10.0f  // drawn surfaces with the game's collision this close are
                                     // left to the game's collision
#define CLIP_BODY_INSET       6.0f   // the body's sides for drawn walls: this much inside its
                                     // collision radius (the game's walls already keep it out)
#define CLIP_STEP_OVER        40.0f  // an edge with a walkable top at most this far above the
                                     // contact is a step (stair riser, curb), not a wall
#define CLIP_MAXCONTACTS      48
#define CLIP_WALL_GROUPS      ((1u << CLIPCHECK_GROUP_WALLS) | (1u << CLIPCHECK_GROUP_GLASS) | (1u << CLIPCHECK_GROUP_LIFTS))
#define CLIP_MAXGLASS         256    // glass panes made solid (per stage)
#define CLIP_BACKED_DIST      40.0f  // translucent plain surfaces with the game's collision this close are solid
#define CLIP_RECENTLY_SHOWN   60     // rooms drawn within this many frames count as being in view

// Debug view (clipcheck/debugview.c): what the last checks found
struct clipdbgmark g_ClipDbgMarks[CLIPDBG_MAXMARKS];
static s32 g_ClipDbgNext = 0;
static f32 g_ClipDbgGameGround = 0.0f;
static f32 g_ClipDbgDrawnGround = 0.0f;
static s32 g_ClipDbgGroundRaised = false;
s32 g_ClipDbgBlockFrame = -1000;
static f32 g_ClipDbgBlockFrac = 1.0f;
f32 g_ClipDbgCapsule[4]; // low, top, radius, valid

// The body being checked: its feet, and how far above them a step's top may
// be (clipcheckPlayerMoveBlocked sets them for clipcheckBodyFraction)
static f32 g_ClipBodyFeet = 0.0f;
static f32 g_ClipBodyStepTop = 1e9f;
s32 g_ClipDbgBudget = 300; // triangles per frame: more when the extra memory was set aside at stage load

// Each added room's drawn triangles (struct in clip_internal.h)
struct cliproomtris *g_ClipRoomTris = NULL;

static void clipDbgMark(const f32 *pos, s32 kind)
{
	struct clipdbgmark *mark;

	if (!g_ClipDebug) {
		return;
	}

	mark = &g_ClipDbgMarks[g_ClipDbgNext];
	g_ClipDbgNext = (g_ClipDbgNext + 1) % CLIPDBG_MAXMARKS;
	memcpy(mark->pos, pos, sizeof(mark->pos));
	mark->kind = kind;
	mark->frame = g_Vars.lvframe60;
}

s32 g_ClipPlayerMode = CLIPMODE_GAME;          // the player: the game's collision (default), generated, or both

// Combat Simulator: the game's own collision, whatever the settings, unless
// the stage has none (a mod's map). Decided each tick (clipcheckTick).
static s32 g_ClipForceGame = 0;

// CLIP_LOG set in the environment: detailed logging
static s32 clipLog(void)
{
	static s32 log = -1;

	if (log < 0) {
		log = getenv("CLIP_LOG") != NULL;
	}

	return log;
}
// Scripted walks (Extraction, The Duel) too: their paths are laid out for
// the game's collision, and one straying onto a drawn door frame got stuck.
#define CLIP_PLAYERMODE ((g_ClipForceGame || g_Vars.tickmode == TICKMODE_AUTOWALK) ? CLIPMODE_GAME : g_ClipPlayerMode)

/*
 * Ladders work as in the original without handing the player's movement over
 * to the game's collision (its walls then trapped him by some ladders). Near
 * a ladder, checked once per frame per player:
 *  - the game's tests take its ladder tiles only (CDTYPE_BGLADDERONLY), which
 *    hold the player at the ladder as in the original
 *  - drawn surfaces right in front of a ladder tile (rungs, rails, the edges
 *    of a hatch it climbs through) are left out of the generated collision.
 */
#define CLIP_LADDER_REACH 40.0f // this much further out than the game's own ladder test
#define CLIP_LADDER_SPAN  60.0f // and this far above the head / below the feet
#define CLIP_LADDER_MAXZONES 4
#define CLIP_LADDER_BEHIND 4.0f   // the zone starts this far behind the ladder tile
#define CLIP_LADDER_FRONT  25.0f  // and reaches this much further out than the body's radius
#define CLIP_LADDER_SIDE   6.0f   // and this far past its sides
#define CLIP_LADDER_BELOW  30.0f  // and below its bottom
#define CLIP_LADDER_ABOVE  100.0f // and above its top (a hatch's edges, climbing out)

struct clipladderzone {
	f32 ox, oz;       // a point on the tile
	f32 nx, nz;       // its horizontal normal, towards the player
	f32 tmin, tmax;   // its extent along (-nz, nx)
	f32 ymin, ymax;
};

static s32 g_ClipLadderFrame = -1;
static s32 g_ClipLadderPlayer = -1;
static s32 g_ClipNearLadder = false;
static s32 g_ClipNumLadderZones = 0;
static f32 g_ClipLadderFront = CLIP_LADDER_FRONT;
static struct clipladderzone g_ClipLadderZones[CLIP_LADDER_MAXZONES];

static void clipAddLadderZone(struct geo *geo, struct player *player)
{
	struct geotilei *tile = (struct geotilei *) geo;
	struct clipladderzone *z;
	struct coord n;
	f32 nl, tx, tz;
	s32 i;

	if (g_ClipNumLadderZones >= CLIP_LADDER_MAXZONES || geo->type != GEOTYPE_TILE_I || geo->numvertices < 3) {
		return;
	}

	cdGetGeoNormal(geo, &n);
	nl = sqrtf(n.x * n.x + n.z * n.z);

	// Flat ladder tiles (some levels tag floors as ladders) have no front
	if (nl < 0.5f * sqrtf(n.x * n.x + n.y * n.y + n.z * n.z)) {
		return;
	}

	z = &g_ClipLadderZones[g_ClipNumLadderZones];
	z->ox = tile->vertices[0][0];
	z->oz = tile->vertices[0][2];
	z->nx = n.x / nl;
	z->nz = n.z / nl;

	// Facing the player
	if ((player->prop->pos.x - z->ox) * z->nx + (player->prop->pos.z - z->oz) * z->nz < 0.0f) {
		z->nx = -z->nx;
		z->nz = -z->nz;
	}

	tx = -z->nz;
	tz = z->nx;
	z->tmin = z->ymin = 1.0e9f;
	z->tmax = z->ymax = -1.0e9f;

	for (i = 0; i < geo->numvertices; i++) {
		f32 t = (tile->vertices[i][0] - z->ox) * tx + (tile->vertices[i][2] - z->oz) * tz;

		if (t < z->tmin) z->tmin = t;
		if (t > z->tmax) z->tmax = t;
		if (tile->vertices[i][1] < z->ymin) z->ymin = tile->vertices[i][1];
		if (tile->vertices[i][1] > z->ymax) z->ymax = tile->vertices[i][1];
	}

	g_ClipNumLadderZones++;
}

static s32 clipPlayerNearLadder(void)
{
	struct player *player = g_Vars.currentplayer;
	struct collision collisions[CLIP_LADDER_MAXZONES + 1];
	struct coord pos;
	f32 radius, ymax, ymin;
	s32 i;

	if (player == NULL || player->prop == NULL) {
		return false;
	}

	if (g_ClipLadderFrame == g_Vars.lvframenum && g_ClipLadderPlayer == g_Vars.currentplayernum) {
		return g_ClipNearLadder;
	}

	g_ClipLadderFrame = g_Vars.lvframenum;
	g_ClipLadderPlayer = g_Vars.currentplayernum;
	g_ClipNumLadderZones = 0;
	radius = player->bond2.radius;
	g_ClipLadderFront = radius + CLIP_LADDER_FRONT;
	pos = player->prop->pos;
	ymax = CLIP_LADDER_SPAN + 20.0f;
	ymin = player->vv_manground - pos.y - CLIP_LADDER_SPAN;

	// (As cdFindLadder, but every ladder tile in reach)
	cdCollectGeoForCyl(&pos, radius * 1.2f + CLIP_LADDER_REACH, player->prop->rooms, CDTYPE_BG,
			GEOFLAG_LADDER | GEOFLAG_LADDER_PLAYERONLY, CHECKVERTICAL_YES, ymax, ymin, collisions, CLIP_LADDER_MAXZONES);

	for (i = 0; i < CLIP_LADDER_MAXZONES && collisions[i].geo; i++) {
		clipAddLadderZone(collisions[i].geo, player);
	}

	g_ClipNearLadder = player->onladder || i > 0;

	return g_ClipNearLadder;
}

// A drawn surface at p is part of a ladder the player is at (see above)
static s32 clipLadderIgnores(const f32 *p)
{
	s32 i;

	if (CLIP_PLAYERMODE == CLIPMODE_GAME || !clipPlayerNearLadder()) {
		return false;
	}

	for (i = 0; i < g_ClipNumLadderZones; i++) {
		struct clipladderzone *z = &g_ClipLadderZones[i];
		f32 dx = p[0] - z->ox, dz = p[2] - z->oz;
		f32 s = dx * z->nx + dz * z->nz;
		f32 t = dx * -z->nz + dz * z->nx;

		if (s >= -CLIP_LADDER_BEHIND && s <= g_ClipLadderFront
				&& t >= z->tmin - CLIP_LADDER_SIDE && t <= z->tmax + CLIP_LADDER_SIDE
				&& p[1] >= z->ymin - CLIP_LADDER_BELOW && p[1] <= z->ymax + CLIP_LADDER_ABOVE) {
			return true;
		}
	}

	return false;
}

// Ragdolls always use the generated collision (the game's tiles alone were
// found to be no use for them in testing). MOD: combat simulator ragdolls -
// in the Combat Simulator too (only the players keep the game's collision)
#define CLIP_RAGDOLLMODE ((g_ClipForceGame && !g_RagdollsEnabled) ? CLIPMODE_GAME : CLIPMODE_GENERATED)

static s32 clipStageHasGameCollision(void)
{
	return g_TileFileData.u8 != NULL && g_TileRooms != NULL && g_TileNumRooms > 0
		&& g_TileRooms[g_TileNumRooms] > g_TileRooms[0];
}
s32 g_ClipRenderFloors = 1;
s32 g_ClipRenderCeilings = 1;
s32 g_ClipRenderWalls = 1;
s32 g_ClipDebug = 0; // 0 off, 1 drawn surfaces, 2 the game's collision tiles, 3 both
f32 g_ClipMaxCatchFall = 600.0f;

static u8 g_ClipRoomDone[CLIP_MAXROOMS];
s32 g_ClipReady = false;
static s32 g_ClipStats[3];

// The player: where the last time on solid ground was (falls further than
// g_ClipMaxCatchFall below it aren't caught)
static s32 g_ClipLastSolidValid = false;
static f32 g_ClipLastGroundX, g_ClipLastGroundZ;
static s32 g_ClipLastGroundValid = false;
static f32 g_ClipLastSolidY = 0.0f;
// Falling: where the feet were on the previous ground check. Anything facing
// up between there and the feet now was passed through on the way down (a
// fast fall onto a steep mountainside): that's where the body lands.
static f32 g_ClipPrevFeet = 0.0f;
static s32 g_ClipPrevFeetValid = false;
static f32 g_ClipLandTop = -1e9f;
// The game's own floor here (ground check only): a wall leaning back may lift
// the body up to it (climbing out of Defection's lift pits), never above
static f32 g_ClipGameFloorTop = -1e9f;
static f32 g_ClipLastBlock[4]; // CLIP_LOG: the contact that last blocked a move (point, normal.y)

struct clipmesh {
	f32 *verts;
	s32 *indices;
	s32 numverts;
	s32 numtris;
	s32 maxverts;
	s32 maxtris;
};

static void clipMeshReset(struct clipmesh *mesh)
{
	mesh->numverts = 0;
	mesh->numtris = 0;
}

static void clipMeshAddTri(struct clipmesh *mesh, const f32 *a, const f32 *b, const f32 *c)
{
	s32 first = mesh->numverts;

	if (mesh->numverts + 3 > mesh->maxverts) {
		mesh->maxverts = mesh->maxverts ? mesh->maxverts * 2 : 1024;
		mesh->verts = realloc(mesh->verts, sizeof(f32) * 3 * mesh->maxverts);
	}

	if (mesh->numtris + 1 > mesh->maxtris) {
		mesh->maxtris = mesh->maxtris ? mesh->maxtris * 2 : 512;
		mesh->indices = realloc(mesh->indices, sizeof(s32) * 3 * mesh->maxtris);
	}

	memcpy(&mesh->verts[first * 3 + 0], a, sizeof(f32) * 3);
	memcpy(&mesh->verts[first * 3 + 3], b, sizeof(f32) * 3);
	memcpy(&mesh->verts[first * 3 + 6], c, sizeof(f32) * 3);
	mesh->numverts += 3;

	mesh->indices[mesh->numtris * 3 + 0] = first;
	mesh->indices[mesh->numtris * 3 + 1] = first + 1;
	mesh->indices[mesh->numtris * 3 + 2] = first + 2;
	mesh->numtris++;
}

/**
 * The texture a vertex batch is drawn with: the last texture image set before
 * its vertices (as the game's bullet tests find it, bgTestHitInVtxBatch).
 * -1 if unknown.
 */
// dyntex.c's arrays (same layouts): the game's animated water
struct clipdyntexroom {
	u16 roomnum;
	u16 typelistoffset;
	u16 numtypes;
	s32 updatedframe;
};

struct clipdyntextype {
	u16 type : 7;
	u16 initialised : 1;
	u8 numvertices;
	u16 vertexlistoffset;
};

struct clipdyntexvtx {
	u16 offset;
	s16 s;
	s16 t;
};

extern struct clipdyntexroom *g_DyntexRooms;
extern struct clipdyntextype *g_DyntexTypes;
extern struct clipdyntexvtx *g_DyntexVertices;
extern s32 g_DyntexRoomsCount;

static u16 g_ClipWaterOffsets[1200];
static s32 g_ClipNumWaterOffsets;

// The room's vertices the game animates as water (river, ocean)
static void clipFindWaterVertices(s32 roomnum)
{
	s32 i, j, k;

	g_ClipNumWaterOffsets = 0;

	if (g_DyntexRooms == NULL || g_DyntexTypes == NULL || g_DyntexVertices == NULL) {
		return;
	}

	for (i = 0; i < g_DyntexRoomsCount; i++) {
		if (g_DyntexRooms[i].roomnum != roomnum) {
			continue;
		}

		for (j = 0; j < g_DyntexRooms[i].numtypes; j++) {
			struct clipdyntextype *type = &g_DyntexTypes[g_DyntexRooms[i].typelistoffset + j];

			if (type->type != DYNTEXTYPE_RIVER && type->type != DYNTEXTYPE_OCEAN) {
				continue;
			}

			for (k = 0; k < type->numvertices && g_ClipNumWaterOffsets < ARRAYCOUNT(g_ClipWaterOffsets); k++) {
				g_ClipWaterOffsets[g_ClipNumWaterOffsets++] = g_DyntexVertices[type->vertexlistoffset + k].offset;
			}
		}
	}
}

static s32 clipIsWaterVertex(u32 offset)
{
	s32 i;

	for (i = 0; i < g_ClipNumWaterOffsets; i++) {
		if (g_ClipWaterOffsets[i] == (u16) offset) {
			return true;
		}
	}

	return false;
}

static s32 g_ClipTexMismatch = 0;
static s32 g_ClipTexLastHeader = -1; // the game's way, for the log

// Per room log: triangles per texture (debug view / CLIP_LOG)
struct cliptexstat {
	s16 texnum;
	s16 header;
	s32 counts[3]; // solid, water, translucent
};

static struct cliptexstat g_ClipTexStats[48];
static s32 g_ClipNumTexStats;

static void clipTexStat(s32 texnum, s32 kind)
{
	s32 i;

	for (i = 0; i < g_ClipNumTexStats; i++) {
		if (g_ClipTexStats[i].texnum == texnum) {
			g_ClipTexStats[i].counts[kind]++;
			return;
		}
	}

	if (g_ClipNumTexStats < ARRAYCOUNT(g_ClipTexStats)) {
		struct cliptexstat *st = &g_ClipTexStats[g_ClipNumTexStats++];
		st->texnum = texnum;
		st->header = g_ClipTexLastHeader;
		st->counts[0] = st->counts[1] = st->counts[2] = 0;
		st->counts[kind]++;
	}
}

/**
 * The texture number for a G_SETTIMG image address. The game reads the
 * number stored 8 bytes before the image (bgTestHitInVtxBatch), but that
 * gives the wrong texture for many surfaces in the port (Carrington's walls,
 * Deep Sea's rocks came out as water). The texture pool knows which texture
 * owns which data, so look it up there; the old way is only the fallback.
 */
static s32 clipTextureForImage(Gfx *cmd)
{
	static uintptr_t cacheaddr[64];
	static s16 cachenum[64];
	static s32 cachenext = 0;
	uintptr_t addr;
	struct tex *cur;
	s32 headernum, i;

	if (cmd->words.w1 & 1) {
		return -1;
	}

	addr = (uintptr_t) PHYS_TO_K0(UNSEGADDR(cmd->words.w1));

	headernum = *(s16 *) PHYS_TO_K0(UNSEGADDR(cmd->words.w1) - 8);

	if (headernum < 0 || headernum >= NUM_TEXTURES) {
		headernum = -1;
	}

	g_ClipTexLastHeader = headernum;

	for (i = 0; i < 64; i++) {
		if (cacheaddr[i] == addr && addr != 0) {
			return cachenum[i];
		}
	}

	for (cur = g_TexSharedPool.head; cur != NULL; ) {
		if ((uintptr_t) cur->data == addr) {
			if (cur->texturenum != headernum) {
				g_ClipTexMismatch++;
			}

			headernum = cur->texturenum < NUM_TEXTURES ? cur->texturenum : -1;
			break;
		}

		if (!cur->next) {
			break;
		}

		cur = (struct tex *) PHYS_TO_K0(cur->next);
	}

	cacheaddr[cachenext] = addr;
	cachenum[cachenext] = headernum;
	cachenext = (cachenext + 1) % 64;

	return headernum;
}

static s32 clipBatchTexture(struct vtxbatch *batch)
{
	Gfx *gdl = batch->gdl;
	Gfx *iter = &gdl[batch->gbicmdindex];
	s32 texnum;

	while (iter->bytes[GFX_W0_BYTE(0)] != G_SETTIMG && iter > gdl) {
		iter--;
	}

	if (iter == gdl) {
		return -1;
	}

	texnum = clipTextureForImage(iter);

	return texnum;
}

enum {
	CLIPSOLID_NO,
	CLIPSOLID_YES,
	CLIPSOLID_IFBACKED, // only where the game's own collision is right at it
	CLIPSOLID_WATERTYPE, // surface type says water: solid unless it's a water surface (flat, nothing under it)
	CLIPSOLID_LIGHT,     // light (debug view: magenta)
};

/**
 * Whether a batch's surfaces are solid. By the texture's surface type, as the
 * game decides what bullets hit:
 *   - water (shallow or deep, opaque or not) is never solid: it's waded or
 *     swum into, the floor is below it
 *   - translucent batches with a plain texture (no surface type) are light
 *     beams, glows and holograms, but also railings and grates: solid where
 *     the game has its own collision right at them (the invisible walls
 *     along railings, the floor under a grate), not elsewhere (light)
 *   - everything else is: opaque surfaces, glass, metal grates, fences
 */
static s32 g_ClipTexRoom = -1; // the room being generated (clipAddRoom)

/*
 * Death floors: drawn floors the player dies on, with the generated
 * collision (Game mode keeps the game's own floors as they are).
 * Carrington Institute: the blank texture 0x0001 floor (not the ceiling).
 */
#define CLIP_DEATHFLOOR_REACH 20.0f // feet this close above it (or below) count

static f32 *g_ClipDeathTris = NULL; // 9 floats per triangle
static s32 g_ClipNumDeathTris = 0;
static s32 g_ClipMaxDeathTris = 0;

static bool clipTexIsDeathFloor(s32 texnum)
{
	return texnum == 0x0001 && g_Vars.stagenum == STAGE_CITRAINING;
}

static void clipAddDeathTri(const f32 *pa, const f32 *pb, const f32 *pc)
{
	if (g_ClipNumDeathTris >= g_ClipMaxDeathTris) {
		s32 newmax = g_ClipMaxDeathTris ? g_ClipMaxDeathTris * 2 : 64;
		f32 *tris = realloc(g_ClipDeathTris, newmax * 9 * sizeof(f32));

		if (tris == NULL) {
			return;
		}

		g_ClipDeathTris = tris;
		g_ClipMaxDeathTris = newmax;
	}

	memcpy(&g_ClipDeathTris[g_ClipNumDeathTris * 9 + 0], pa, 3 * sizeof(f32));
	memcpy(&g_ClipDeathTris[g_ClipNumDeathTris * 9 + 3], pb, 3 * sizeof(f32));
	memcpy(&g_ClipDeathTris[g_ClipNumDeathTris * 9 + 6], pc, 3 * sizeof(f32));
	g_ClipNumDeathTris++;
}

/**
 * The height of a death floor triangle at x, z, if x, z is over one.
 */
static bool clipDeathFloorAt(f32 x, f32 z, f32 *outy)
{
	s32 i;

	for (i = 0; i < g_ClipNumDeathTris; i++) {
		const f32 *t = &g_ClipDeathTris[i * 9];
		f32 d = (t[5] - t[8]) * (t[0] - t[6]) + (t[6] - t[3]) * (t[2] - t[8]);
		f32 w0, w1, w2;

		if (d > -0.0001f && d < 0.0001f) {
			continue;
		}

		w0 = ((t[5] - t[8]) * (x - t[6]) + (t[6] - t[3]) * (z - t[8])) / d;
		w1 = ((t[8] - t[2]) * (x - t[6]) + (t[0] - t[6]) * (z - t[8])) / d;
		w2 = 1.0f - w0 - w1;

		if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
			*outy = w0 * t[1] + w1 * t[4] + w2 * t[7];
			return true;
		}
	}

	return false;
}

/**
 * Is (x, y, z) on a floor triangle of the mesh (within CLIP_FLOORFILL_DY)?
 */
static bool clipMeshCoversPoint(const struct clipmesh *m, const f32 *p)
{
	s32 i;

	for (i = 0; i < m->numtris; i++) {
		const f32 *t = &m->verts[i * 9];
		f32 d = (t[5] - t[8]) * (t[0] - t[6]) + (t[6] - t[3]) * (t[2] - t[8]);
		f32 w0, w1, w2, y;

		if (d > -0.0001f && d < 0.0001f) {
			continue;
		}

		w0 = ((t[5] - t[8]) * (p[0] - t[6]) + (t[6] - t[3]) * (p[2] - t[8])) / d;
		w1 = ((t[8] - t[2]) * (p[0] - t[6]) + (t[0] - t[6]) * (p[2] - t[8])) / d;
		w2 = 1.0f - w0 - w1;

		if (w0 < -0.01f || w1 < -0.01f || w2 < -0.01f) {
			continue;
		}

		y = w0 * t[1] + w1 * t[4] + w2 * t[7];

		if (fabsf(y - p[1]) <= CLIP_FLOORFILL_DY) {
			return true;
		}
	}

	return false;
}

// The nearest drawn floor under p (x/z), from mindy to maxdy below it: how far
// below, or -1
static f32 clipMeshFloorBelow(const struct clipmesh *m, const f32 *p, f32 mindy, f32 maxdy)
{
	f32 best = -1.0f;
	s32 i;

	for (i = 0; i < m->numtris; i++) {
		const f32 *t = &m->verts[i * 9];
		f32 d = (t[5] - t[8]) * (t[0] - t[6]) + (t[6] - t[3]) * (t[2] - t[8]);
		f32 w0, w1, w2, dy;

		if (d > -0.0001f && d < 0.0001f) {
			continue;
		}

		w0 = ((t[5] - t[8]) * (p[0] - t[6]) + (t[6] - t[3]) * (p[2] - t[8])) / d;
		w1 = ((t[8] - t[2]) * (p[0] - t[6]) + (t[0] - t[6]) * (p[2] - t[8])) / d;
		w2 = 1.0f - w0 - w1;

		if (w0 < -0.01f || w1 < -0.01f || w2 < -0.01f) {
			continue;
		}

		dy = p[1] - (w0 * t[1] + w1 * t[4] + w2 * t[7]);

		if (dy >= mindy && dy <= maxdy && (best < 0.0f || dy < best)) {
			best = dy;
		}
	}

	return best;
}

/**
 * The game's floor tiles with nothing drawn on them are kept as floors (the
 * invisible separators in Defection's central shaft), except where the room
 * itself draws a floor further down: that's a trench or pit the game covered
 * with its collision (Infiltration's hangars), and bodies fall into it as
 * drawn. (The shaft's floor below is another room.)
 */
#define CLIP_FLOORFILL_GAPMIN 30.0f   // a drawn floor at least this far below
#define CLIP_FLOORFILL_GAPMAX 3000.0f // and at most this far

static void clipAddMissingGameFloors(s32 roomnum, struct clipmesh *floors, struct clipmesh *xlufloors)
{
	const f32 *tris;
	s32 n = ragdollGetRoomGameFloors(roomnum, &tris);
	s32 i, added = 0, bridged = 0;
	s32 nfloors = floors->numtris;

	for (i = 0; i < n; i++) {
		const f32 *t = &tris[i * 9];
		f32 e1[3], e2[3], ny, len;
		f32 c[3];
		s32 k, s;
		bool missing = false;
		bool gap = false;

		for (k = 0; k < 3; k++) {
			e1[k] = t[3 + k] - t[k];
			e2[k] = t[6 + k] - t[k];
			c[k] = (t[k] + t[3 + k] + t[6 + k]) / 3.0f;
		}

		ny = e1[2] * e2[0] - e1[0] * e2[2];
		len = sqrtf((e1[1] * e2[2] - e1[2] * e2[1]) * (e1[1] * e2[2] - e1[2] * e2[1]) + ny * ny
				+ (e1[0] * e2[1] - e1[1] * e2[0]) * (e1[0] * e2[1] - e1[1] * e2[0]));

		if (len < 0.01f || fabsf(ny) / len < 0.7f) {
			continue;
		}

		// The middle and three points towards the corners
		for (s = 0; s < 4 && !missing; s++) {
			f32 p[3];

			for (k = 0; k < 3; k++) {
				p[k] = s == 0 ? c[k] : c[k] + (t[(s - 1) * 3 + k] - c[k]) * 0.8f;
			}

			if (!clipMeshCoversPoint(floors, p) && !clipMeshCoversPoint(xlufloors, p)) {
				if (clipMeshFloorBelow(floors, p, CLIP_FLOORFILL_GAPMIN, CLIP_FLOORFILL_GAPMAX) >= 0.0f
						|| clipMeshFloorBelow(xlufloors, p, CLIP_FLOORFILL_GAPMIN, CLIP_FLOORFILL_GAPMAX) >= 0.0f) {
					gap = true;
				} else {
					missing = true;
				}
			}
		}

		if (missing) {
			// Both windings (the generated floors are made one-sided)
			if (ny > 0.0f) {
				clipMeshAddTri(xlufloors, &t[0], &t[3], &t[6]);
			} else {
				clipMeshAddTri(xlufloors, &t[0], &t[6], &t[3]);
			}

			added++;
		} else if (gap) {
			bridged++;
		}
	}

	(void) nfloors;

	if (added > 0) {
		sysLogPrintf(LOG_NOTE, "CLIP: room %d: %d of the game's floor tiles have nothing drawn: kept as floors", roomnum, added);
	}

	if (bridged > 0) {
		sysLogPrintf(LOG_NOTE, "CLIP: room %d: %d of the game's floor tiles cover a gap drawn in the room: left open", roomnum, bridged);
	}
}

static s32 clipTexIsSolid(s32 batchtype, s32 texnum)
{
	s32 type = texnum >= 0 ? g_Textures[texnum].surfacetype : SURFACETYPE_DEFAULT;

	// Attack Ship's cockpit window (room 0x6e): the blank texture, but glass
	if (texnum == 0x0001 && g_Vars.stagenum == STAGE_ATTACKSHIP && g_ClipTexRoom == 0x6e) {
		return CLIPSOLID_YES;
	}

	// The game's own animated water (tex.c, the dyntex river/ocean
	// textures): Deep Sea's river, Air Base and Investigation's water,
	// Chicago's canal, Villa's shallow and deep water, Complex
	if (texnum == 0x06cb || texnum == 0x01c7 || texnum == 0x0dae || texnum == 0x090f
			|| texnum == 0x06f6) { // Deep Sea's water
		return CLIPSOLID_NO;
	}

	// Always solid: Chicago's metal grate, Deep Sea's rock (the game
	// marks the rock as water)
	if (texnum == 0x01d4 || texnum == 0x0687) {
		return CLIPSOLID_YES;
	}

	// Never drawn / light: never solid
	if (texnum == 0x01e7 // Skedar Ruins' lit floors
			|| texnum == 0x0001 // Skedar Ruins' big invisible box by the ravine (blank texture)
			|| texnum == 0x0125) { // Area 51's translucent surface blocking a path
		return CLIPSOLID_LIGHT;
	}

	// The surface type is for bullet effects and sounds: the game marks
	// plenty of things water that aren't (Carrington's wall panels, oil
	// spills, Deep Sea's rocks). Decided per triangle.
	if (type == SURFACETYPE_SHALLOWWATER || type == SURFACETYPE_DEEPWATER) {
		return CLIPSOLID_WATERTYPE;
	}

	if (batchtype != CLIP_VTXBATCHTYPE_OPA && (type == SURFACETYPE_DEFAULT || type == SURFACETYPE_NONE)) {
		return CLIPSOLID_IFBACKED;
	}

	return CLIPSOLID_YES;
}

// The texture a G_SETTIMG command sets (-1 if unknown)
static s32 clipSetTimgTexture(Gfx *cmd)
{
	return clipTextureForImage(cmd);
}

static const f32 g_ClipTriSamples[5][3] = {
	{ 0.334f, 0.333f, 0.333f },
	{ 0.7f, 0.15f, 0.15f },
	{ 0.15f, 0.7f, 0.15f },
	{ 0.15f, 0.15f, 0.7f },
	{ 0.45f, 0.45f, 0.1f },
};

/**
 * Signed distance (along the normal) from 5 points on a triangle to the
 * nearest surface of the given groups within 'dist' that lies the same way
 * (normals within ~35 degrees); 1e9 where there's none.
 */
static void clipTriSampleDistsRange(const f32 *a, const f32 *b, const f32 *c, const f32 *nrm,
		f32 above, f32 below, f32 mindot, u32 groups, f32 *out)
{
	s32 i, k;

	for (i = 0; i < 5; i++) {
		f32 p[3], from[3], to[3], hit[3], hitn[3];

		out[i] = 1e9f;

		for (k = 0; k < 3; k++) {
			p[k] = a[k] * g_ClipTriSamples[i][0] + b[k] * g_ClipTriSamples[i][1] + c[k] * g_ClipTriSamples[i][2];
			from[k] = p[k] + nrm[k] * above;
			to[k] = p[k] - nrm[k] * below;
		}

		if (pdphysRaycastLevelGroups(from, to, groups, hit, hitn)
				&& fabsf(hitn[0] * nrm[0] + hitn[1] * nrm[1] + hitn[2] * nrm[2]) > mindot) {
			out[i] = (hit[0] - p[0]) * nrm[0] + (hit[1] - p[1]) * nrm[1] + (hit[2] - p[2]) * nrm[2];
		}
	}
}

static void clipTriSampleDists(const f32 *a, const f32 *b, const f32 *c, const f32 *nrm, f32 dist, u32 groups, f32 *out)
{
	clipTriSampleDistsRange(a, b, c, nrm, dist, dist, 0.8f, groups, out);
}

#define CLIP_GEN_GROUPS ((1u << CLIPCHECK_GROUP_FLOORS) | (1u << CLIPCHECK_GROUP_CEILINGS) | CLIP_WALL_GROUPS)

/**
 * A surface drawn just in front of another drawn surface, with the game's
 * collision at that other surface rather than at this one: a light pool or
 * glow over a floor or wall (Skedar's lit floors). The player stands on the
 * surface below; this one isn't solid.
 */
static s32 clipTriIsOverlay(const f32 *a, const f32 *b, const f32 *c, const f32 *nrm)
{
	f32 game[5], below[5];
	s32 i, count = 0;

	clipTriSampleDistsRange(a, b, c, nrm, 4.0f, 30.0f, 0.8f, CLIP_PD_GROUPS, game);
	clipTriSampleDistsRange(a, b, c, nrm, -1.5f, 30.0f, 0.8f, CLIP_GEN_GROUPS, below);

	for (i = 0; i < 5; i++) {
		if (game[i] < 1e8f && game[i] < -3.0f && below[i] < 1e8f && fabsf(game[i] - below[i]) < 4.0f) {
			count++;
		}
	}

	return count >= 3;
}

/**
 * A translucent plain-textured triangle is solid (railing, grate) when the
 * game's own collision lies along it close by (the invisible wall along a
 * railing, the floor tile of a grated walkway). A light pool or glow drawn
 * just off a floor or wall has the game's collision near it too, but there
 * it's at the opaque surface the light is drawn over, not at the light:
 * those aren't solid. Light beams across a room have neither.
 */
static s32 clipXluTriSolid(const f32 *a, const f32 *b, const f32 *c, const f32 *nrm)
{
	f32 game[5];
	s32 i, backed = 0;

	// The game's collision along it: an invisible wall along a railing, a
	// fence's or grate's own collision, the floor tile of a grated walkway
	clipTriSampleDistsRange(a, b, c, nrm, CLIP_BACKED_DIST, CLIP_BACKED_DIST, 0.8f, CLIP_PD_GROUPS, game);

	for (i = 0; i < 5; i++) {
		if (game[i] < 1e8f) {
			backed++;
		}
	}

	return backed >= 3 && !clipTriIsOverlay(a, b, c, nrm);
}

/**
 * Glass panes are objects, drawn apart from the rooms. Most have collision of
 * their own (a block from their model's bounds), which the generated mode
 * keeps. Some don't (OBJFLAG_00000100 off: Defection's lobby panes by the
 * lift, pads 0x24d-0x250): the game relies on its wall tiles there, which
 * the generated mode leaves out. Those panes become walls of the rooms
 * they're in until they're shattered.
 */

// ragdoll.c: an object's model as world triangles
s32 ragdollObjModelTris(struct prop *prop, f32 *verts, s32 maxverts, s32 *indices, s32 maxtris, s32 *outnumverts);

#define CLIP_GLASS_MAXVERTS 4096
#define CLIP_GLASS_MAXTRIS  4096

struct clipglass g_ClipGlass[CLIP_MAXGLASS];
s32 g_ClipNumGlass = 0;
static s32 g_ClipPreloaded = 0; // rooms generated before they were seen

/**
 * Lifts: the game gives a lift its floor, and some lifts walls from model
 * parts. The others (the A51 hangar and control room lifts) rely on the
 * shaft's wall tiles to keep you in, which the generated mode leaves out.
 * Their drawn walls and undersides (the model without the faces looking up:
 * the floor is the game's own tile) become surfaces of the rooms they're in
 * (CLIPCHECK_GROUP_LIFTS), moved with them.
 */
#define CLIP_MAXLIFTS 32

struct cliplift {
	struct prop *prop;
	f32 pos[3];
	RoomNum rooms[8];
	f32 *verts; // its walls and undersides relative to its position (lifts don't turn)
	s32 *indices;
	s32 numverts;
	s32 numtris;
};

static struct cliplift g_ClipLifts[CLIP_MAXLIFTS];
static s32 g_ClipNumLifts = -1; // -1: not looked for yet

// A lift's model without the faces looking up, once
static void clipLiftLoadTris(struct cliplift *cl)
{
	static f32 verts[CLIP_GLASS_MAXVERTS * 3];
	static s32 indices[CLIP_GLASS_MAXTRIS * 3];
	s32 nv = 0, nt, q, k = 0, m;

	nt = ragdollObjModelTris(cl->prop, verts, CLIP_GLASS_MAXVERTS, indices, CLIP_GLASS_MAXTRIS, &nv);

	for (q = 0; q < nt; q++) {
		f32 *a = &verts[indices[q * 3 + 0] * 3], *b = &verts[indices[q * 3 + 1] * 3], *c = &verts[indices[q * 3 + 2] * 3];
		f32 e1[3], e2[3], n[3], len;

		for (m = 0; m < 3; m++) {
			e1[m] = b[m] - a[m];
			e2[m] = c[m] - a[m];
		}

		n[0] = e1[1] * e2[2] - e1[2] * e2[1];
		n[1] = e1[2] * e2[0] - e1[0] * e2[2];
		n[2] = e1[0] * e2[1] - e1[1] * e2[0];
		len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

		if (len >= 1e-6f && n[1] < CLIP_FLOOR_MINUP * len) {
			memmove(&indices[k * 3], &indices[q * 3], 3 * sizeof(s32));
			k++;
		}
	}

	cl->verts = k > 0 ? malloc(nv * 3 * sizeof(f32)) : NULL;
	cl->indices = k > 0 ? malloc(k * 3 * sizeof(s32)) : NULL;
	cl->numverts = 0;
	cl->numtris = 0;

	if (cl->verts && cl->indices) {
		for (q = 0; q < nv * 3; q++) {
			cl->verts[q] = verts[q] - cl->prop->pos.f[q % 3];
		}

		memcpy(cl->indices, indices, k * 3 * sizeof(s32));
		cl->numverts = nv;
		cl->numtris = k;
	}
}

static void clipFreeLifts(void)
{
	s32 i;

	for (i = 0; i < g_ClipNumLifts; i++) {
		free(g_ClipLifts[i].verts);
		free(g_ClipLifts[i].indices);
	}

	g_ClipNumLifts = -1;
}

static s32 clipGlassGone(struct clipglass *g)
{
	return g->obj->prop != g->prop || g->prop->obj != g->obj
		|| (g->obj->hidden2 & OBJH2FLAG_DESTROYED) || (g->obj->hidden & OBJHFLAG_DELETING);
}

static void clipBuildRoomGlass(s32 roomnum)
{
	static const s32 quads[6][4] = {
		{ 0, 1, 3, 2 }, { 4, 6, 7, 5 }, // x min, x max
		{ 0, 4, 5, 1 }, { 2, 3, 7, 6 }, // y min, y max
		{ 0, 2, 6, 4 }, { 1, 5, 7, 3 }, // z min, z max
	};
	static f32 verts[CLIP_GLASS_MAXVERTS * 3];
	static s32 indices[CLIP_GLASS_MAXTRIS * 3];
	s32 nv = 0, nt = 0;
	s32 i, q;

	pdphysLevelRemoveRoomGroup(roomnum, CLIPCHECK_GROUP_GLASS);

	for (i = 0; i < g_ClipNumGlass && nv + 8 <= CLIP_GLASS_MAXVERTS && nt + 12 <= CLIP_GLASS_MAXTRIS; i++) {
		struct clipglass *g = &g_ClipGlass[i];

		if (!g->live || g->room != roomnum) {
			continue;
		}

		if (g->model) {
			s32 mv = 0;
			s32 mt = ragdollObjModelTris(g->prop, &verts[nv * 3], CLIP_GLASS_MAXVERTS - nv,
					&indices[nt * 3], CLIP_GLASS_MAXTRIS - nt, &mv);

			for (q = 0; q < mt * 3; q++) {
				indices[nt * 3 + q] += nv;
			}

			nv += mv;
			nt += mt;
			continue;
		}

		memcpy(&verts[nv * 3], g->verts, sizeof(g->verts));

		for (q = 0; q < 6; q++) {
			indices[nt * 3 + 0] = nv + quads[q][0];
			indices[nt * 3 + 1] = nv + quads[q][1];
			indices[nt * 3 + 2] = nv + quads[q][2];
			nt++;
			indices[nt * 3 + 0] = nv + quads[q][0];
			indices[nt * 3 + 1] = nv + quads[q][2];
			indices[nt * 3 + 2] = nv + quads[q][3];
			nt++;
		}

		nv += 8;
	}

	if (nt > 0) {
		pdphysLevelAddRoomGroup(roomnum, CLIPCHECK_GROUP_GLASS, verts, nv, indices, nt);
	}
}

/**
 * Objects without collision of their own that stand in for level walls
 * (Setup Editor pads): solid boxes like the glass panes, until
 * destroyed.
 */
static s32 clipObjIsSolidModel(struct defaultobj *obj)
{
	// Area 51 Escape: the wall Jonathan blows up
	return g_Vars.stagenum == STAGE_ESCAPE && obj->pad == 0x0423;
}

static void clipAddRoomGlass(s32 roomnum)
{
	RoomNum rooms[2];
	s16 propnums[256];
	s32 i, j, k, added = 0;

	rooms[0] = roomnum;
	rooms[1] = -1;
	roomGetProps(rooms, propnums, 256);

	for (i = 0; propnums[i] >= 0 && g_ClipNumGlass < CLIP_MAXGLASS; i++) {
		struct prop *prop = &g_Vars.props[propnums[i]];
		struct defaultobj *obj = prop->obj;
		struct modelrodata_bbox *bbox;
		struct clipglass *g;
		f32 lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 }, height, w[2];
		u8 *start, *end;
		s32 dup = false;

		if (prop->type != PROPTYPE_OBJ || obj == NULL || obj->model == NULL
				|| (obj->type != OBJTYPE_GLASS && obj->type != OBJTYPE_TINTEDGLASS && !clipObjIsSolidModel(obj))) {
			continue;
		}

		if ((obj->hidden2 & OBJH2FLAG_DESTROYED) || (obj->hidden & OBJHFLAG_DELETING)) {
			continue;
		}

		// Its own collision: left to the game's object checks
		if (propUpdateGeometry(prop, &start, &end)) {
			continue;
		}

		for (j = 0; j < g_ClipNumGlass; j++) {
			if (g_ClipGlass[j].prop == prop && g_ClipGlass[j].room == roomnum) {
				dup = true;
			}
		}

		bbox = objFindBboxRodata(obj);

		if (dup || bbox == NULL) {
			continue;
		}

		g = &g_ClipGlass[g_ClipNumGlass];
		g->prop = prop;
		g->obj = obj;
		g->room = roomnum;
		g->live = true;
		g->model = clipObjIsSolidModel(obj);

		// Corners: pos + local * realrot (as the game's own glass blocks)
		for (k = 0; k < 8; k++) {
			f32 l[3];
			s32 c;

			l[0] = (k & 4) ? bbox->xmax : bbox->xmin;
			l[1] = (k & 2) ? bbox->ymax : bbox->ymin;
			l[2] = (k & 1) ? bbox->zmax : bbox->zmin;

			// A pane is paper thin: at least CLIP_GLASS_MINTHICK, so bodies
			// can't push a part through it
			if (bbox->xmax - bbox->xmin < CLIP_GLASS_MINTHICK) {
				l[0] = (bbox->xmax + bbox->xmin) * 0.5f + ((k & 4) ? 0.5f : -0.5f) * CLIP_GLASS_MINTHICK;
			}

			if (bbox->zmax - bbox->zmin < CLIP_GLASS_MINTHICK) {
				l[2] = (bbox->zmax + bbox->zmin) * 0.5f + ((k & 1) ? 0.5f : -0.5f) * CLIP_GLASS_MINTHICK;
			}

			for (c = 0; c < 3; c++) {
				g->verts[k][c] = prop->pos.f[c] + l[0] * obj->realrot[0][c] + l[1] * obj->realrot[1][c] + l[2] * obj->realrot[2][c];

				if (k == 0 || g->verts[k][c] < lo[c]) lo[c] = g->verts[k][c];
				if (k == 0 || g->verts[k][c] > hi[c]) hi[c] = g->verts[k][c];
			}
		}

		// Only upright panes: a pane lying flat (a glass floor) would stop
		// the player walking onto it
		height = hi[1] - lo[1];
		w[0] = hi[0] - lo[0];
		w[1] = hi[2] - lo[2];

		if (!clipObjIsSolidModel(obj) && (height < 40.0f || height < (w[0] < w[1] ? w[0] : w[1]))) {
			continue;
		}

		g_ClipNumGlass++;
		added++;

		if (g_ClipDebug) {
			sysLogPrintf(LOG_NOTE, "CLIP: room %d glass/wall pad %d (type %d) without collision made solid: (%.0f %.0f %.0f)..(%.0f %.0f %.0f)",
					roomnum, obj->pad, obj->type, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
		}
	}

	if (added) {
		clipBuildRoomGlass(roomnum);
	}
}

/**
 * Shattered panes stop being walls.
 */
// A lift in a shaft (doors at its stops) without walls of its own: the
// platforms, bridges and the dish keep the game's collision only
static s32 clipLiftNeedsWalls(struct liftobj *lift)
{
	struct modeldef *def = lift->base.model->definition;
	s32 i, doors = false;

	for (i = 0; i < ARRAYCOUNT(lift->doors); i++) {
		doors = doors || (lift->pads[i] >= 0 && lift->doors[i]);
	}

	return doors
		&& modelGetPartRodata(def, MODELPART_LIFT_WALL1) == NULL
		&& modelGetPartRodata(def, MODELPART_LIFT_WALL2) == NULL
		&& modelGetPartRodata(def, MODELPART_LIFT_WALL3) == NULL;
}

static void clipTickLifts(void)
{
	s32 i, j;

	if (g_ClipNumLifts < 0) {
		struct prop *prop;

		g_ClipNumLifts = 0;

		// The props in use (active, then paused): free slots still hold the
		// last stage's pointers
		for (prop = g_Vars.activeprops; prop && g_ClipNumLifts < CLIP_MAXLIFTS; prop = prop->next) {
			if (prop->type == PROPTYPE_OBJ && prop->obj && prop->obj->type == OBJTYPE_LIFT && prop->obj->model
					&& clipLiftNeedsWalls((struct liftobj *) prop->obj)) {
				struct cliplift *cl = &g_ClipLifts[g_ClipNumLifts++];

				cl->prop = prop;
				cl->rooms[0] = -1;
				clipLiftLoadTris(cl);
			}
		}
	}

	for (i = 0; i < g_ClipNumLifts; i++) {
		struct cliplift *cl = &g_ClipLifts[i];
		struct prop *prop = cl->prop;
		s32 moved = false, samerooms = cl->rooms[0] >= 0;

		if (cl->numtris == 0 || prop->type != PROPTYPE_OBJ || prop->obj == NULL || prop->obj->type != OBJTYPE_LIFT) {
			continue;
		}

		for (j = 0; j < 3; j++) {
			moved = moved || prop->pos.f[j] != cl->pos[j];
			cl->pos[j] = prop->pos.f[j];
		}

		for (j = 0; j < 8 && samerooms; j++) {
			samerooms = prop->rooms[j] == cl->rooms[j];

			if (prop->rooms[j] < 0) {
				break;
			}
		}

		if (samerooms) {
			if (moved) {
				pdphysLevelMoveMover(i, cl->pos);
			}

			continue;
		}

		// Into other rooms: one part per room it's in
		memcpy(cl->rooms, prop->rooms, sizeof(cl->rooms));
		pdphysLevelRemoveMover(i);

		for (j = 0; j < 8 && cl->rooms[j] >= 0; j++) {
			pdphysLevelAddMoverPart(cl->rooms[j], CLIPCHECK_GROUP_LIFTS, i, cl->pos, cl->verts, cl->numverts, cl->indices, cl->numtris);
		}
	}
}

static void clipTickGlass(void)
{
	s32 i, j;

	for (i = 0; i < g_ClipNumGlass; i++) {
		struct clipglass *g = &g_ClipGlass[i];

		if (g->live && clipGlassGone(g)) {
			// Every room the pane was added to
			for (j = 0; j < g_ClipNumGlass; j++) {
				if (g_ClipGlass[j].prop == g->prop && g_ClipGlass[j].live) {
					g_ClipGlass[j].live = false;
					clipBuildRoomGlass(g_ClipGlass[j].room);
				}
			}
		}
	}
}

/**
 * Sorts a loaded room's opaque render triangles into floors, ceilings and
 * walls (same vertex fetch as the game's bullet tests, bgTestHitInVtxBatch).
 */
static void clipAddRoom(s32 roomnum)
{
	// 0-2 floors, ceilings, walls; 3 water and 4 non-solid translucent (debug
	// view only); 5 translucent waiting for the opaque ones to be in;
	// 6-8 solid translucent floors, ceilings, walls
	static struct clipmesh meshes[10]; // 9: opaque floors waiting for the overlay test
	s32 badslots = 0;
	struct vtxbatch *batch = g_Rooms[roomnum].vtxbatches;
	s32 numbatches = g_Rooms[roomnum].numvtxbatches;
	f32 slots[CLIP_VTXSLOTS][3];
	s32 slotvalid[CLIP_VTXSLOTS];
	u32 slotoffset[CLIP_VTXSLOTS];
	Gfx *lastgdl = NULL;
	Vtx *lastvtx = NULL;
	s32 b, i;
	s32 solid, tsolid;

	for (i = 0; i < 10; i++) {
		clipMeshReset(&meshes[i]);
	}

	clipFindWaterVertices(roomnum);

	g_ClipNumTexStats = 0;
	g_ClipTexRoom = roomnum;

	for (b = 0; b < numbatches; b++, batch++) {
		Gfx *iter;
		Vtx *vtx;
		s32 n, v0, k, texnum;

		if (batch->gdl == NULL) {
			continue;
		}

		// The vertex cache carries over between the loads of one display
		// list (a load can go to any slot and triangles can use vertices
		// from earlier loads), as the renderer does; the game's bullet
		// tests only look at the latest load and miss those triangles
		if (batch->gdl != lastgdl) {
			lastgdl = batch->gdl;
			lastvtx = bgFindVerticesForGdl(roomnum, batch->gdl);

			for (k = 0; k < CLIP_VTXSLOTS; k++) {
				slotvalid[k] = false;
			}
		}

		if (lastvtx == NULL) {
			continue;
		}

		iter = &batch->gdl[batch->gbicmdindex];

		if (iter->dma.cmd != G_VTX) {
			continue;
		}

		vtx = (Vtx *)((UNSEGADDR(iter->words.w1) & 0xffffff) + (uintptr_t) lastvtx);
		n = (((u32) iter->bytes[GFX_W0_BYTE(1)] >> 4) & 0xf) + 1;
		v0 = (u32) iter->bytes[GFX_W0_BYTE(1)] & 0xf;

		for (k = 0; k < n && v0 + k < CLIP_VTXSLOTS; k++) {
			slots[v0 + k][0] = g_BgRooms[roomnum].pos.x + vtx[k].x;
			slots[v0 + k][1] = g_BgRooms[roomnum].pos.y + vtx[k].y;
			slots[v0 + k][2] = g_BgRooms[roomnum].pos.z + vtx[k].z;
			slotvalid[v0 + k] = true;
			slotoffset[v0 + k] = (UNSEGADDR(iter->words.w1) & 0xffffff) + k * sizeof(Vtx);
		}

		// The texture is tracked per command: it can change between the
		// triangles of one load
		texnum = clipBatchTexture(batch);
		solid = clipTexIsSolid(batch->type, texnum);

		for (iter++; iter->dma.cmd != G_VTX && iter->dma.cmd != G_ENDDL; iter++) {
			s32 tris[4][3];
			s32 numtris = 0;
			s32 t;

			if (iter->bytes[GFX_W0_BYTE(0)] == G_SETTIMG) {
				texnum = clipSetTimgTexture(iter);
				solid = clipTexIsSolid(batch->type, texnum);
				continue;
			}

			if (iter->dma.cmd == G_TRI1) {
				tris[0][0] = iter->tri.tri.v[GFX_TRI_VTX(0)] / 10;
				tris[0][1] = iter->tri.tri.v[GFX_TRI_VTX(1)] / 10;
				tris[0][2] = iter->tri.tri.v[GFX_TRI_VTX(2)] / 10;
				numtris = 1;
			} else if (iter->dma.cmd == G_TRI4) {
				tris[0][0] = iter->tri4.x1; tris[0][1] = iter->tri4.y1; tris[0][2] = iter->tri4.z1;
				tris[1][0] = iter->tri4.x2; tris[1][1] = iter->tri4.y2; tris[1][2] = iter->tri4.z2;
				tris[2][0] = iter->tri4.x3; tris[2][1] = iter->tri4.y3; tris[2][2] = iter->tri4.z3;
				tris[3][0] = iter->tri4.x4; tris[3][1] = iter->tri4.y4; tris[3][2] = iter->tri4.z4;
				numtris = 4;
			}

			for (t = 0; t < numtris; t++) {
				f32 *pa, *pb, *pc;
				f32 e1[3], e2[3], nrm[3];
				f32 len;

				if (tris[t][0] == 0 && tris[t][1] == 0 && tris[t][2] == 0) {
					continue;
				}

				if (tris[t][0] >= CLIP_VTXSLOTS || tris[t][1] >= CLIP_VTXSLOTS || tris[t][2] >= CLIP_VTXSLOTS
						|| !slotvalid[tris[t][0]] || !slotvalid[tris[t][1]] || !slotvalid[tris[t][2]]) {
					badslots++;
					continue;
				}

				pa = slots[tris[t][0]];
				pb = slots[tris[t][1]];
				pc = slots[tris[t][2]];

				for (k = 0; k < 3; k++) {
					e1[k] = pb[k] - pa[k];
					e2[k] = pc[k] - pa[k];
				}

				nrm[0] = e1[1] * e2[2] - e1[2] * e2[1];
				nrm[1] = e1[2] * e2[0] - e1[0] * e2[2];
				nrm[2] = e1[0] * e2[1] - e1[1] * e2[0];
				len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);

				if (len < 0.01f) {
					continue;
				}

				nrm[0] /= len;
				nrm[1] /= len;
				nrm[2] /= len;

				if (clipTexIsDeathFloor(texnum) && nrm[1] >= CLIP_FLOOR_MINUP) {
					clipAddDeathTri(pa, pb, pc);
				}

				// Marked water: walls, slopes and the like are solid (panels,
				// rocks); flat ones too where the game's floor is right at them
				// (a spill or puddle decal). A flat one with no floor at it is
				// a water surface to wade into.
				tsolid = solid;

				// Vertices the game animates as water (its rivers and pools)
				if (g_ClipNumWaterOffsets > 0 && clipIsWaterVertex(slotoffset[tris[t][0]])
						&& clipIsWaterVertex(slotoffset[tris[t][1]]) && clipIsWaterVertex(slotoffset[tris[t][2]])) {
					tsolid = CLIPSOLID_NO;
				}

				if (tsolid == CLIPSOLID_WATERTYPE) {
					if (nrm[1] < CLIP_WALL_STEEP) {
						tsolid = CLIPSOLID_YES;
					} else {
						f32 game[5];
						s32 n2 = 0, q;

						clipTriSampleDists(pa, pb, pc, nrm, 6.0f, CLIP_PD_GROUPS, game);

						for (q = 0; q < 5; q++) {
							if (game[q] < 1e8f) {
								n2++;
							}
						}

						tsolid = n2 >= 3 ? CLIPSOLID_YES : CLIPSOLID_NO;
					}
				}

				clipTexStat(texnum, tsolid == CLIPSOLID_NO ? 1 : tsolid == CLIPSOLID_IFBACKED ? 2 : 0);

				if (tsolid == CLIPSOLID_LIGHT) {
					clipMeshAddTri(&meshes[4], pa, pb, pc);
					continue;
				}

				// Not solid: kept for the debug view only
				if (tsolid == CLIPSOLID_NO) {
					clipMeshAddTri(&meshes[3], pa, pb, pc);
					continue;
				}

				if (tsolid == CLIPSOLID_IFBACKED) {
					clipMeshAddTri(&meshes[5], pa, pb, pc);
					continue;
				}

				// Opaque floors without the game's collision right at them
				// might be a light drawn over another floor: decided once the
				// rest are in
				if (nrm[1] >= CLIP_WALL_STEEP) {
					f32 game[5];

					clipTriSampleDistsRange(pa, pb, pc, nrm, 4.0f, 4.0f, 0.8f, CLIP_PD_GROUPS, game);

					if (game[0] >= 1e8f) {
						clipMeshAddTri(&meshes[9], pa, pb, pc);
						continue;
					}
				}

				if (nrm[1] >= CLIP_FLOOR_MINUP) {
					clipMeshAddTri(&meshes[0], pa, pb, pc);
				} else if (nrm[1] <= -CLIP_CEIL_MINDOWN) {
					clipMeshAddTri(&meshes[1], pa, pb, pc);
				} else {
					clipMeshAddTri(&meshes[2], pa, pb, pc);
				}
			}
		}
	}

	for (i = 0; i < 3; i++) {
		if (meshes[i].numtris > 0) {
			pdphysLevelAddRoomGroup(roomnum, CLIPCHECK_GROUP_FLOORS + i, meshes[i].verts, meshes[i].numverts, meshes[i].indices, meshes[i].numtris);
			g_ClipStats[i] += meshes[i].numtris;
		}
	}

	// Translucent plain surfaces, now that the opaque ones can be tested against
	{
		s32 t;

		for (t = 0; t < meshes[5].numtris; t++) {
			f32 *pa = &meshes[5].verts[t * 9];
			f32 *pb = pa + 3;
			f32 *pc = pa + 6;
			f32 e1[3], e2[3], nrm[3], len;
			s32 k;

			for (k = 0; k < 3; k++) {
				e1[k] = pb[k] - pa[k];
				e2[k] = pc[k] - pa[k];
			}

			nrm[0] = e1[1] * e2[2] - e1[2] * e2[1];
			nrm[1] = e1[2] * e2[0] - e1[0] * e2[2];
			nrm[2] = e1[0] * e2[1] - e1[1] * e2[0];
			len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
			nrm[0] /= len;
			nrm[1] /= len;
			nrm[2] /= len;

			if (!clipXluTriSolid(pa, pb, pc, nrm)) {
				clipMeshAddTri(&meshes[4], pa, pb, pc);
			} else if (nrm[1] >= CLIP_FLOOR_MINUP) {
				clipMeshAddTri(&meshes[6], pa, pb, pc);
			} else if (nrm[1] <= -CLIP_CEIL_MINDOWN) {
				clipMeshAddTri(&meshes[7], pa, pb, pc);
			} else {
				clipMeshAddTri(&meshes[8], pa, pb, pc);
			}
		}

		for (t = 0; t < meshes[9].numtris; t++) {
			f32 *pa = &meshes[9].verts[t * 9];
			f32 *pb = pa + 3;
			f32 *pc = pa + 6;
			f32 e1[3], e2[3], nrm[3], len;
			s32 k;

			for (k = 0; k < 3; k++) {
				e1[k] = pb[k] - pa[k];
				e2[k] = pc[k] - pa[k];
			}

			nrm[0] = e1[1] * e2[2] - e1[2] * e2[1];
			nrm[1] = e1[2] * e2[0] - e1[0] * e2[2];
			nrm[2] = e1[0] * e2[1] - e1[1] * e2[0];
			len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
			nrm[0] /= len;
			nrm[1] /= len;
			nrm[2] /= len;

			if (clipTriIsOverlay(pa, pb, pc, nrm)) {
				clipMeshAddTri(&meshes[4], pa, pb, pc);
			} else {
				clipMeshAddTri(&meshes[6], pa, pb, pc);
			}
		}

		// The game's floors with nothing drawn there (invisible floors: the
		// separators in Defection's central shaft above the lobby). The game
		// has them, so bodies (and the player) land on them as in the
		// original instead of falling through to the floor below.
		clipAddMissingGameFloors(roomnum, &meshes[0], &meshes[6]);

		// Translucent solid walls (railings, grates, fences) are single
		// planes: bodies tunnelled through them or were pushed out the far
		// side. Each becomes a slab CLIP_XLUWALL_THICK thick (both sides).
		{
			s32 n0 = meshes[8].numtris;
			s32 t;

			for (t = 0; t < n0; t++) {
				f32 tri[9], e1[3], e2[3], nrm[3], len;
				f32 front[9], back[9];
				s32 k, e;

				memcpy(tri, &meshes[8].verts[t * 9], sizeof(tri));

				for (k = 0; k < 3; k++) {
					e1[k] = tri[3 + k] - tri[k];
					e2[k] = tri[6 + k] - tri[k];
				}

				nrm[0] = e1[1] * e2[2] - e1[2] * e2[1];
				nrm[1] = e1[2] * e2[0] - e1[0] * e2[2];
				nrm[2] = e1[0] * e2[1] - e1[1] * e2[0];
				len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);

				if (len < 0.0001f) {
					continue;
				}

				for (k = 0; k < 9; k++) {
					f32 off = nrm[k % 3] / len * (CLIP_XLUWALL_THICK * 0.5f);

					front[k] = tri[k] + off;
					back[k] = tri[k] - off;
				}

				clipMeshAddTri(&meshes[8], &front[0], &front[3], &front[6]);
				clipMeshAddTri(&meshes[8], &back[0], &back[6], &back[3]);

				// The three sides (a railing's top edge is a ledge, as in the game)
				for (e = 0; e < 3; e++) {
					s32 a = e * 3, b = ((e + 1) % 3) * 3;

					clipMeshAddTri(&meshes[8], &front[a], &back[a], &back[b]);
					clipMeshAddTri(&meshes[8], &front[a], &back[b], &front[b]);
				}
			}
		}

		for (i = 6; i < 9; i++) {
			if (meshes[i].numtris > 0) {
				pdphysLevelAddRoomGroup(roomnum, CLIPCHECK_GROUP_FLOORS + i - 6, meshes[i].verts, meshes[i].numverts, meshes[i].indices, meshes[i].numtris);
				g_ClipStats[i - 6] += meshes[i].numtris;
			}
		}
	}

	clipAddRoomGlass(roomnum);

	pdphysMarkRoomGenerated(roomnum);

	if (g_ClipDebug || clipLog()) {
		for (i = 0; i < g_ClipNumTexStats; i++) {
			struct cliptexstat *st = &g_ClipTexStats[i];
			s32 t = st->texnum;

			sysLogPrintf(LOG_NOTE, "CLIP: room %d tex %d (game's way %d) surface %d sound %d: %d solid, %d water, %d translucent",
					roomnum, t, st->header,
					t >= 0 ? g_Textures[t].surfacetype : -1, t >= 0 ? g_Textures[t].soundsurfacetype : -1,
					st->counts[0], st->counts[1], st->counts[2]);
		}

		sysLogPrintf(LOG_NOTE, "CLIP: room %d: %d batches, %d floor %d ceiling %d wall tris (+%d translucent); skipped %d water, %d translucent, %d bad vertex slots; %d texture lookups differed from the game's so far",
				roomnum, numbatches, meshes[0].numtris, meshes[1].numtris, meshes[2].numtris,
				meshes[6].numtris + meshes[7].numtris + meshes[8].numtris,
				meshes[3].numtris, meshes[4].numtris, badslots, g_ClipTexMismatch);
	}

	// A copy for the debug view
	if (g_ClipRoomTris) {
		struct cliproomtris *rt = &g_ClipRoomTris[roomnum];
		s32 total = meshes[0].numtris + meshes[1].numtris + meshes[2].numtris + meshes[3].numtris + meshes[4].numtris
				+ meshes[6].numtris + meshes[7].numtris + meshes[8].numtris;
		s32 k = 0;

		rt->verts = malloc(sizeof(f32) * 9 * (total ? total : 1));
		rt->kinds = malloc(total ? total : 1);
		rt->numtris = total;

		for (i = 0; i < 9; i++) {
			s32 t;

			if (i == 5) {
				continue;
			}

			for (t = 0; t < meshes[i].numtris; t++, k++) {
				memcpy(&rt->verts[k * 9], &meshes[i].verts[t * 9], sizeof(f32) * 9);
				rt->kinds[k] = i >= 6 ? i - 6 : i;
			}
		}
	}
}

static void clipFreeRoomTris(void)
{
	s32 i;

	if (g_ClipRoomTris) {
		for (i = 0; i < CLIP_MAXROOMS; i++) {
			free(g_ClipRoomTris[i].verts);
			free(g_ClipRoomTris[i].kinds);
		}

		memset(g_ClipRoomTris, 0, sizeof(struct cliproomtris) * CLIP_MAXROOMS);
	} else {
		g_ClipRoomTris = calloc(CLIP_MAXROOMS, sizeof(struct cliproomtris));
	}
}

/**
 * Collision cache (cache/collisions/ next to pd.ini): the generated surfaces
 * of every room of a stage, written once all of them have been generated
 * (seen, or loaded in the background), read back at the next load of that
 * stage so nothing has to be generated again (no background loading, no
 * stutter). One file per stage. The key covers the level data (room file
 * offsets, positions, bounds) and this file's build time, so a changed
 * ROM/mod or a rebuilt clipcheck.c (changed rules) makes a fresh one.
 * Glass panes are props: not cached, added per room as usual.
 *
 *   u32 magic 'PDCC', u32 version, u32 key, u32 stagenum, u32 roomcount
 *   per room 1..roomcount-1: u32 numtris, numtris * 9 f32, numtris u8 kinds
 */
#define CLIP_CACHE_MAGIC   0x50444343
#define CLIP_CACHE_VERSION 1
#define CLIP_CACHE_PARENT  "$S/cache"
#define CLIP_CACHE_DIR     CLIP_CACHE_PARENT "/collisions"
#define CLIP_CACHE_BUDGET  3000 // microseconds per tick for adding cached rooms

static s32 g_ClipCacheState = 0; // 0 not looked at yet this stage, 1 read in (adding), 2 none (generating), 3 written/done
static u32 g_ClipCacheKey = 0;

static u32 clipCacheHash(u32 h, const void *data, s32 len)
{
	const u8 *b = data;
	s32 i;

	for (i = 0; i < len; i++) {
		h = (h ^ b[i]) * 16777619u;
	}

	return h;
}

static u32 clipCacheMakeKey(void)
{
	static const char build[] = __DATE__ " " __TIME__;
	u32 h = 2166136261u;
	s32 r;

	h = clipCacheHash(h, build, sizeof(build));
	h = clipCacheHash(h, &g_Vars.stagenum, sizeof(g_Vars.stagenum));
	h = clipCacheHash(h, &g_Vars.roomcount, sizeof(g_Vars.roomcount));

	for (r = 0; r < g_Vars.roomcount; r++) {
		u32 off = (u32) g_BgRooms[r].unk00;

		h = clipCacheHash(h, &off, sizeof(off));
		h = clipCacheHash(h, &g_BgRooms[r].pos, sizeof(g_BgRooms[r].pos));
		h = clipCacheHash(h, g_Rooms[r].bbmin, sizeof(g_Rooms[r].bbmin));
		h = clipCacheHash(h, g_Rooms[r].bbmax, sizeof(g_Rooms[r].bbmax));
	}

	return h;
}

static const char *clipCachePath(void)
{
	static char path[FS_MAXPATH + 1];
	char rel[64];

	snprintf(rel, sizeof(rel), CLIP_CACHE_DIR "/stage_%02x.bin", g_Vars.stagenum);
	snprintf(path, sizeof(path), "%s", fsFullPath(rel));

	return path;
}

/**
 * Reads the stage's cache into the per-room copies. Returns true if every
 * room was read.
 */
static s32 clipCacheRead(void)
{
	FILE *f = fopen(clipCachePath(), "rb");
	u32 head[5];
	s32 r, ok = true;

	if (f == NULL) {
		return false;
	}

	if (fread(head, sizeof(head), 1, f) != 1 || head[0] != CLIP_CACHE_MAGIC || head[1] != CLIP_CACHE_VERSION
			|| head[2] != g_ClipCacheKey || head[3] != (u32) g_Vars.stagenum || head[4] != (u32) g_Vars.roomcount
			|| g_Vars.roomcount > CLIP_MAXROOMS) {
		fclose(f);
		return false;
	}

	for (r = 1; r < g_Vars.roomcount && ok; r++) {
		struct cliproomtris *rt = &g_ClipRoomTris[r];
		u32 n;

		if (fread(&n, sizeof(n), 1, f) != 1 || n > 1000000) {
			ok = false;
			break;
		}

		rt->verts = malloc(sizeof(f32) * 9 * (n ? n : 1));
		rt->kinds = malloc(n ? n : 1);
		rt->numtris = n;

		if (n && (fread(rt->verts, sizeof(f32) * 9, n, f) != n || fread(rt->kinds, 1, n, f) != n)) {
			ok = false;
		}
	}

	fclose(f);

	if (!ok) {
		clipFreeRoomTris();
	}

	return ok;
}

static void clipCacheWrite(void)
{
	const char *path;
	FILE *f;
	u32 head[5];
	s32 r;

	if (fsFileSize(CLIP_CACHE_PARENT) < 0) {
		fsCreateDir(CLIP_CACHE_PARENT);
	}

	if (fsFileSize(CLIP_CACHE_DIR) < 0) {
		fsCreateDir(CLIP_CACHE_DIR);
	}

	path = clipCachePath();
	f = fopen(path, "wb");

	if (f == NULL) {
		sysLogPrintf(LOG_WARNING, "CLIP: can't write the collision cache %s", path);
		return;
	}

	head[0] = CLIP_CACHE_MAGIC;
	head[1] = CLIP_CACHE_VERSION;
	head[2] = g_ClipCacheKey;
	head[3] = g_Vars.stagenum;
	head[4] = g_Vars.roomcount;
	fwrite(head, sizeof(head), 1, f);

	for (r = 1; r < g_Vars.roomcount; r++) {
		struct cliproomtris *rt = &g_ClipRoomTris[r];
		u32 n = rt->verts ? rt->numtris : 0;

		fwrite(&n, sizeof(n), 1, f);

		if (n) {
			fwrite(rt->verts, sizeof(f32) * 9, n, f);
			fwrite(rt->kinds, 1, n, f);
		}
	}

	fclose(f);
	sysLogPrintf(LOG_NOTE, "CLIP: collision cache written: %s", path);
}

/**
 * Water: the highest water surface (the drawn surfaces that aren't solid:
 * the game's water textures and animated water) over (x, z) between ylo and
 * yhi, in the given rooms (-1 terminated, up to 8). For bodies that float.
 */
s32 clipcheckWaterLevel(const RoomNum *rooms, f32 x, f32 z, f32 ylo, f32 yhi, f32 *outy)
{
	s32 found = false;
	s32 r;

	if (!g_ClipReady || g_ClipRoomTris == NULL) {
		return false;
	}

	for (r = 0; r < 8 && rooms[r] >= 0; r++) {
		struct cliproomtris *rt;
		s32 t;

		if (rooms[r] <= 0 || rooms[r] >= CLIP_MAXROOMS) {
			continue;
		}

		rt = &g_ClipRoomTris[rooms[r]];

		for (t = 0; t < rt->numtris; t++) {
			const f32 *v;
			f32 d, w0, w1, w2, y;
			f32 e1[3], e2[3], n[3], len;
			s32 k;

			if (rt->kinds[t] != 3) {
				continue;
			}

			v = &rt->verts[t * 9];

			for (k = 0; k < 3; k++) {
				e1[k] = v[3 + k] - v[k];
				e2[k] = v[6 + k] - v[k];
			}

			n[0] = e1[1] * e2[2] - e1[2] * e2[1];
			n[1] = e1[2] * e2[0] - e1[0] * e2[2];
			n[2] = e1[0] * e2[1] - e1[1] * e2[0];
			len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

			if (len < 0.01f || fabsf(n[1]) < 0.9f * len) {
				continue; // only level surfaces
			}

			d = (v[5] - v[8]) * (v[0] - v[6]) + (v[6] - v[3]) * (v[2] - v[8]);

			if (d > -0.0001f && d < 0.0001f) {
				continue;
			}

			w0 = ((v[5] - v[8]) * (x - v[6]) + (v[6] - v[3]) * (z - v[8])) / d;
			w1 = ((v[8] - v[2]) * (x - v[6]) + (v[0] - v[6]) * (z - v[8])) / d;
			w2 = 1.0f - w0 - w1;

			if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
				continue;
			}

			y = w0 * v[1] + w1 * v[4] + w2 * v[7];

			if (y >= ylo && y <= yhi && (!found || y > *outy)) {
				*outy = y;
				found = true;
			}
		}
	}

	return found;
}

/**
 * A room from the cache: its solid surfaces into the physics groups (as
 * clipAddRoom does), and its glass panes.
 */
static void clipAddRoomFromCache(s32 roomnum)
{
	struct cliproomtris *rt = &g_ClipRoomTris[roomnum];
	s32 kind, t, i;

	for (kind = 0; kind < 3; kind++) {
		s32 n = 0;
		s32 *indices;

		for (t = 0; t < rt->numtris; t++) {
			if (rt->kinds[t] == kind) {
				n++;
			}
		}

		if (n == 0) {
			continue;
		}

		{
			f32 *verts = malloc(sizeof(f32) * 9 * n);
			s32 k = 0;

			indices = malloc(sizeof(s32) * 3 * n);

			for (t = 0; t < rt->numtris; t++) {
				if (rt->kinds[t] == kind) {
					memcpy(&verts[k * 9], &rt->verts[t * 9], sizeof(f32) * 9);

					for (i = 0; i < 3; i++) {
						indices[k * 3 + i] = k * 3 + i;
					}

					k++;
				}
			}

			pdphysLevelAddRoomGroup(roomnum, CLIPCHECK_GROUP_FLOORS + kind, verts, n * 3, indices, n);
			g_ClipStats[kind] += n;
			free(verts);
			free(indices);
		}
	}

	clipAddRoomGlass(roomnum);
	pdphysMarkRoomGenerated(roomnum);
}

void clipcheckStageLoad(void)
{
	memset(g_ClipRoomDone, 0, sizeof(g_ClipRoomDone));
	memset(g_ClipStats, 0, sizeof(g_ClipStats));
	memset(g_ClipDbgMarks, 0, sizeof(g_ClipDbgMarks));
	g_ClipLastSolidValid = false;
	g_ClipPrevFeetValid = false;
	g_ClipLastGroundValid = false;
	g_ClipNumGlass = 0;
	clipFreeLifts();
	g_ClipPreloaded = 0;
	g_ClipCacheState = 0; // looked at in the first tick (the room count isn't this stage's yet here)
	clipFreeRoomTris();
	free(g_ClipDeathTris);
	g_ClipDeathTris = NULL;
	g_ClipNumDeathTris = 0;
	g_ClipMaxDeathTris = 0;

	// gfxmemory.c sets aside extra display list memory for the debug view
	// when it's on at stage load; turned on mid-level, it draws less
	g_ClipDbgBudget = g_ClipDebug ? 6000 : 300;
	g_ClipReady = pdphysIsReady() && g_Vars.stagenum < STAGE_TITLE;
}

void clipcheckTick(void)
{
	s32 done = 0;
	s32 r;

	if (!g_ClipReady || g_Vars.stagenum >= STAGE_TITLE) {
		return;
	}

	g_ClipForceGame = g_Vars.normmplayerisrunning && clipStageHasGameCollision();

	// Ragdolls: the game's tiles and the render walls, or the generated surfaces
	pdphysSetRagdollLevelGroups(CLIP_RAGDOLLMODE == CLIPMODE_GENERATED
			? (1u << CLIPCHECK_GROUP_FLOORS) | (1u << CLIPCHECK_GROUP_CEILINGS) | CLIP_WALL_GROUPS
				| 0x3 // the game's tiles: only used in rooms without generated surfaces (not seen yet)
			: 0x7);

	clipTickGlass();
	clipTickLifts();

	// The stage's collision cache
	if (g_ClipCacheState == 0) {
		g_ClipCacheKey = clipCacheMakeKey();

		if (clipCacheRead()) {
			g_ClipCacheState = 1;

			for (r = 1; r < g_Vars.roomcount && r < CLIP_MAXROOMS; r++) {
				g_ClipRoomDone[r] = 2; // cached, physics not added yet
			}

			sysLogPrintf(LOG_NOTE, "CLIP: collision cache read: %s", clipCachePath());
		} else {
			g_ClipCacheState = 2;
			sysLogPrintf(LOG_NOTE, "CLIP: no collision cache for stage 0x%02x yet: generating it", g_Vars.stagenum);
		}
	}

	// Cached rooms into the physics world: nearest the player first, a few
	// milliseconds' worth per tick (rooms not added yet use the game's tiles)
	if (g_ClipCacheState == 1) {
		u64 start = sysGetMicroseconds();
		struct player *player = g_Vars.currentplayer;
		s32 left = 0;

		while (sysGetMicroseconds() - start < CLIP_CACHE_BUDGET) {
			s32 best = -1;
			f32 bestdist = 1e30f;

			for (r = 1; r < g_Vars.roomcount && r < CLIP_MAXROOMS; r++) {
				f32 dist = 0.0f;
				s32 k;

				if (g_ClipRoomDone[r] != 2) {
					continue;
				}

				if (player && player->prop) {
					for (k = 0; k < 3; k++) {
						f32 p = player->prop->pos.f[k];
						f32 d = p < g_Rooms[r].bbmin[k] ? g_Rooms[r].bbmin[k] - p : p > g_Rooms[r].bbmax[k] ? p - g_Rooms[r].bbmax[k] : 0.0f;

						dist += d * d;
					}
				}

				if (dist < bestdist) {
					bestdist = dist;
					best = r;
				}
			}

			if (best < 0) {
				break;
			}

			g_ClipRoomDone[best] = 1;
			clipAddRoomFromCache(best);
		}

		for (r = 1; r < g_Vars.roomcount && r < CLIP_MAXROOMS; r++) {
			if (g_ClipRoomDone[r] == 2) {
				left++;
			}
		}

		if (left == 0) {
			g_ClipCacheState = 3;
			sysLogPrintf(LOG_NOTE, "CLIP: cached surfaces added: %d floor, %d ceiling, %d wall tris", g_ClipStats[0], g_ClipStats[1], g_ClipStats[2]);
		}

		return;
	}

	for (r = 1; r < g_Vars.roomcount && r < CLIP_MAXROOMS && done < CLIP_ROOMSPERTICK; r++) {
		if (!g_ClipRoomDone[r] && g_Rooms[r].loaded240 && g_Rooms[r].gfxdata && g_Rooms[r].vtxbatches) {
			g_ClipRoomDone[r] = 1;
			clipAddRoom(r);
			done++;
		}
	}

	// Rooms not loaded yet (never seen): loaded here just to generate their
	// surfaces, then unloaded again, one per tick, nearest to the player
	// first. Otherwise a room the player or a body enters before it's ever
	// been drawn has no generated walls or floors (bodies falling down a
	// shaft into rooms not seen yet). Not in Game mode for both: the game's
	// room loading stays as the original there.
	if (done == 0 && g_Vars.lvframe60 > 60
			&& (CLIP_PLAYERMODE != CLIPMODE_GAME || CLIP_RAGDOLLMODE != CLIPMODE_GAME)) {
		struct player *player = g_Vars.currentplayer;
		s32 best = -1;
		f32 bestdist = 1e30f;

		for (r = 1; r < g_Vars.roomcount && r < CLIP_MAXROOMS; r++) {
			f32 dist = 0.0f;
			s32 k;

			if (g_ClipRoomDone[r] || g_Rooms[r].loaded240) {
				continue;
			}

			if (player && player->prop) {
				for (k = 0; k < 3; k++) {
					f32 p = player->prop->pos.f[k];
					f32 d = p < g_Rooms[r].bbmin[k] ? g_Rooms[r].bbmin[k] - p : p > g_Rooms[r].bbmax[k] ? p - g_Rooms[r].bbmax[k] : 0.0f;

					dist += d * d;
				}
			}

			if (dist < bestdist) {
				bestdist = dist;
				best = r;
			}
		}

		if (best > 0) {
			g_ClipRoomDone[best] = 1;
			bgLoadRoom(best);

			if (g_Rooms[best].loaded240 && g_Rooms[best].gfxdata && g_Rooms[best].vtxbatches) {
				clipAddRoom(best);
				g_ClipPreloaded++;
			}

			if (g_Rooms[best].loaded240) {
				bgUnloadRoom(best);
			}

			if (clipLog() || (g_ClipDebug && g_ClipPreloaded % 25 == 1)) {
				sysLogPrintf(LOG_NOTE, "CLIP: generated room %d before it was seen (%d so far)", best, g_ClipPreloaded);
			}
		}
	}

	// Every room generated: save them for next time
	if (g_ClipCacheState == 2) {
		for (r = 1; r < g_Vars.roomcount && r < CLIP_MAXROOMS && g_ClipRoomDone[r]; r++);

		if (r >= g_Vars.roomcount) {
			g_ClipCacheState = 3;
			clipCacheWrite();
		}
	}

	if (done && clipLog()) {
		sysLogPrintf(LOG_NOTE, "CLIP: render surfaces so far: %d floor, %d ceiling, %d wall tris", g_ClipStats[0], g_ClipStats[1], g_ClipStats[2]);
	}
}

s32 clipcheckIsReady(void)
{
	return g_ClipReady;
}

/**
 * Levels draw some rooms on top of each other in the same space, shown one at
 * a time through portals (e.g. Defection's stairwell). The game's own
 * collision only looks at the rooms an actor is in, and the checks do the
 * same. The rooms around the player are:
 *   - the rooms it's in, and rooms entered through portals within 'tight' of
 *     it (the game does the same with the player's own bbox),
 *   - rooms drawn on screen just now whose space reaches within 'wide': big
 *     rooms (outdoor terrain, mountains) whose portals are far away.
 * A room drawn in the same space but not shown from here is in neither.
 * Fills rooms (-1 terminated, at most max - 1) and returns how many.
 */
s32 clipPlayerRoomList(f32 tight, f32 wide, f32 ylow, f32 yhigh, RoomNum *rooms, s32 max)
{
	struct player *player = g_Vars.currentplayer;
	struct coord lower, upper;
	s32 n = 0;
	s32 i;

	for (i = 0; i < ARRAYCOUNT(player->prop->rooms) && player->prop->rooms[i] != -1 && n < max - 1; i++) {
		rooms[n++] = player->prop->rooms[i];
	}

	rooms[n] = -1;

	if (n == 0) {
		return 0;
	}

	lower.x = player->prop->pos.x - tight;
	lower.y = ylow;
	lower.z = player->prop->pos.z - tight;
	upper.x = player->prop->pos.x + tight;
	upper.y = yhigh;
	upper.z = player->prop->pos.z + tight;

	bgFindEnteredRooms(&lower, &upper, rooms, max - 1, false);

	for (n = 0; n < max - 1 && rooms[n] != -1; n++);

	lower.x = player->prop->pos.x - wide;
	lower.z = player->prop->pos.z - wide;
	upper.x = player->prop->pos.x + wide;
	upper.z = player->prop->pos.z + wide;

	for (i = 1; i < g_Vars.roomcount && n < max - 1; i++) {
		s32 k;

		if (g_Rooms[i].loaded240 <= 0 || g_Rooms[i].loaded240 > CLIP_RECENTLY_SHOWN
				|| g_Rooms[i].bbmin[0] > upper.x || g_Rooms[i].bbmax[0] < lower.x
				|| g_Rooms[i].bbmin[1] > upper.y || g_Rooms[i].bbmax[1] < lower.y
				|| g_Rooms[i].bbmin[2] > upper.z || g_Rooms[i].bbmax[2] < lower.z) {
			continue;
		}

		for (k = 0; k < n && rooms[k] != i; k++);

		if (k == n) {
			rooms[n++] = i;
		}
	}

	// Rooms the level's scripts turned off (drawn in place of others
	// depending on the state, e.g. an intact wall swapped for the broken one)
	{
		s32 k = 0;

		for (i = 0; i < n; i++) {
			if (rooms[i] <= 0 || rooms[i] >= g_Vars.roomcount || (g_Rooms[rooms[i]].flags & ROOMFLAG_DISABLEDBYSCRIPT) == 0) {
				rooms[k++] = rooms[i];
			}
		}

		if (k > 0) {
			n = k;
		}
	}

	rooms[n] = -1;

	return n;
}

// Limits the following level queries to the rooms around the player
static void clipQueryPlayerRooms(f32 tight, f32 wide, f32 ylow, f32 yhigh)
{
	RoomNum rooms[32];
	s32 ints[32];
	s32 n = clipPlayerRoomList(tight, wide, ylow, yhigh, rooms, ARRAYCOUNT(rooms));
	s32 i;

	if (n == 0) {
		pdphysSetQueryRooms(NULL, -1); // not in any room: everything
		return;
	}

	for (i = 0; i < n; i++) {
		ints[i] = rooms[i];
	}

	pdphysSetQueryRooms(ints, n);
}

static void clipQueryAllRooms(void)
{
	pdphysSetQueryRooms(NULL, -1);
}

/**
 * bondwalk.c: the collision types the game checks the player against. With
 * only generated collision (Game.ClipPlayerMode 1), the game's own level
 * tiles are left out; doors, objects and characters still count.
 */
s32 clipcheckPlayerCdTypes(s32 types)
{
	if (g_ClipReady && CLIP_PLAYERMODE == CLIPMODE_GENERATED) {
		// Near a ladder: its tiles only (see clipLadderIgnores)
		if ((types & CDTYPE_BG) && clipPlayerNearLadder()) {
			types |= CDTYPE_BGLADDERONLY;
		} else {
			types &= ~CDTYPE_BG;
		}
	}

	return types;
}

// Center, then four around it
static void clipSamplePoint(s32 i, f32 x, f32 z, f32 radius, f32 *outx, f32 *outz)
{
	static const f32 offsets[5][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

	*outx = x + offsets[i][0] * radius;
	*outz = z + offsets[i][1] * radius;
}

s32 clipcheckFindFloor(f32 x, f32 z, f32 radius, f32 fromy, f32 toy, f32 *outy)
{
	s32 found = false;
	f32 best = 0.0f;
	s32 i;

	if (!g_ClipReady || fromy <= toy) {
		return false;
	}

	for (i = 0; i < (radius > 0.0f ? 5 : 1); i++) {
		f32 from[3], to[3], hit[3], nrm[3];

		clipSamplePoint(i, x, z, radius, &from[0], &from[2]);
		from[1] = fromy;
		to[0] = from[0];
		to[1] = toy;
		to[2] = from[2];

		if (pdphysRaycastLevelGroups(from, to, 1u << CLIPCHECK_GROUP_FLOORS, hit, nrm)
				&& nrm[1] >= CLIP_FLOOR_WALKABLE
				&& (!found || hit[1] > best)) {
			best = hit[1];
			found = true;
		}
	}

	if (found) {
		*outy = best;
	}

	return found;
}

s32 clipcheckFindCeiling(f32 x, f32 z, f32 radius, f32 fromy, f32 toy, f32 *outy)
{
	s32 found = false;
	f32 best = 0.0f;
	s32 i;

	if (!g_ClipReady || toy <= fromy) {
		return false;
	}

	for (i = 0; i < (radius > 0.0f ? 5 : 1); i++) {
		f32 from[3], to[3], hit[3], nrm[3];

		clipSamplePoint(i, x, z, radius, &from[0], &from[2]);
		from[1] = fromy;
		to[0] = from[0];
		to[1] = toy;
		to[2] = from[2];

		if (pdphysRaycastLevelGroups(from, to, 1u << CLIPCHECK_GROUP_CEILINGS, hit, nrm)
				&& nrm[1] <= -0.3f
				&& (!found || hit[1] < best)) {
			best = hit[1];
			found = true;
		}
	}

	if (found) {
		*outy = best;
	}

	return found;
}

s32 clipcheckSegment(const struct coord *a, const struct coord *b, u32 groupmask, struct coord *hit, struct coord *normal)
{
	f32 from[3] = { a->x, a->y, a->z };
	f32 to[3] = { b->x, b->y, b->z };
	f32 h[3], n[3];

	if (!g_ClipReady) {
		return false;
	}

	if (groupmask == 0) {
		groupmask = (1u << CLIPCHECK_GROUP_FLOORS) | (1u << CLIPCHECK_GROUP_CEILINGS) | CLIP_WALL_GROUPS;
	}

	if (!pdphysRaycastLevelGroups(from, to, groupmask, h, n)) {
		return false;
	}

	if (hit) {
		hit->x = h[0]; hit->y = h[1]; hit->z = h[2];
	}

	if (normal) {
		normal->x = n[0]; normal->y = n[1]; normal->z = n[2];
	}

	return true;
}

/**
 * bwalkUpdateVertical, right after the game has worked out the ground under
 * the player: a drawn surface above the game's ground (that the player can
 * step onto) becomes the ground.
 */
/**
 * The ground under a body (generated collision only): a sphere of the given
 * radius dropped from 'from' (the highest the feet may step up to) down to
 * 'to'; the first surface it lands on, whatever its slope, is the ground.
 * Rays could slip between triangles or past steep surfaces and let the body
 * sink through them; the sphere can't. Steep slopes hold the body like the
 * game's own ramps do (the walls stop it climbing them).
 */
static s32 clipBodyGround(f32 x, f32 z, f32 radius, f32 base, f32 steptop, f32 walktop, f32 from, f32 to, f32 *outy)
{
	static pdphysContact contacts[CLIP_MAXCONTACTS];
	f32 a[3], b[3];
	f32 dist = from - to;
	s32 num, i;

	if (dist <= 0.0f) {
		return false;
	}

	a[0] = x; a[1] = from + radius; a[2] = z;
	b[0] = x; b[1] = to + radius; b[2] = z;

	num = pdphysSweepCapsuleContacts(a, b, radius, 0.0f,
			(1u << CLIPCHECK_GROUP_FLOORS) | CLIP_WALL_GROUPS,
			contacts, CLIP_MAXCONTACTS);

	{
		s32 skipped = false;
		s32 found = false;
		f32 y = 0.0f;

		for (i = 0; i < num; i++) {
			pdphysContact *c = &contacts[i];

			// Something under the sphere (facing up)
			if (c->normal[1] < 0.05f || clipLadderIgnores(c->point)) {
				continue;
			}

			// Higher than a step up: only a walkable slope climbed during the
			// move (up to walktop), never a steep face (it would lift the body
			// up and into it). Below steptop anything holds (steep slopes hold
			// the body like the game's ramps do; the walls stop it climbing).
			// Falling: passed through on the way down this frame. Lands on
			// any slope; on a wall leaning back only below where the body
			// last stood (or jumps would climb it)
			if (c->point[1] <= g_ClipLandTop
					&& (c->normal[1] >= CLIP_FLOOR_MINUP || c->point[1] <= g_ClipLastSolidY + 2.0f)) {
				y = from - c->fraction * dist;

				if (c->fraction <= 0.0f) {
					y = c->point[1];
				}

				found = true;
				break;
			}

			if (c->point[1] > walktop) {
				continue;
			}

			if (c->point[1] > steptop && c->normal[1] < CLIP_WALL_STEEP) {
				skipped = true;
				continue;
			}

			// A wall leaning back (steeper than any floor) never lifts the
			// body, even by a step: it would climb it a step per frame, up
			// and out of the level. It only stops the body sinking into it.
			if (c->normal[1] < CLIP_FLOOR_MINUP && c->point[1] > base + 2.0f && c->point[1] > g_ClipGameFloorTop + 2.0f) {
				skipped = true;
				continue;
			}

			y = from - c->fraction * dist;

			if (c->fraction <= 0.0f) {
				y = c->point[1];
			}

			found = true;
			break;
		}

		// A steep face was passed over above step height. Usually it's
		// beside the body (the foot of a rock face) and the ground is
		// further down. But when it's right under the middle of the body
		// (a fast fall, or landing on a mountainside while moving), the body
		// is already partly in it: what's straight below the middle is the
		// ground then, or the body would drop into the slope.
		if (skipped) {
			f32 p0[3] = { x, walktop > g_ClipLandTop ? walktop : g_ClipLandTop, z };
			f32 p1[3] = { x, to, z };
			f32 hit[3], hitn[3];

			// (Floors only, walkable or steep: a leaning wall under the middle
			// isn't ground, the body would climb it)
			if (pdphysRaycastLevelGroups(p0, p1, (1u << CLIPCHECK_GROUP_FLOORS), hit, hitn)
					&& !clipLadderIgnores(hit)
					&& hitn[1] >= CLIP_FLOOR_MINUP && (!found || hit[1] > y + 4.0f)
					&& (hit[1] <= walktop || hit[1] <= g_ClipLandTop)) {
				if (g_ClipDebug && clipLog()) {
					sysLogPrintf(LOG_NOTE, "CLIP: steep ground under the middle at %.0f (sphere found %s%.0f)",
							hit[1], found ? "" : "none ", found ? y : 0.0f);
				}

				y = hit[1];
				found = true;
			}
		}

		if (found) {
			*outy = y;
		}

		return found;
	}
}

/**
 * Where the player's steps are measured from. The feet (vv_manground) ease
 * up towards the ground over several frames (up to 50 behind), so running up
 * a slope leaves them further and further below it; the last ground found
 * is where the body really stands then.
 */
static f32 clipPlayerStepBase(struct player *player)
{
	f32 feet = player->vv_manground;

	if (!player->isfalling && player->vv_ground > feet && player->vv_ground < feet + 60.0f) {
		return player->vv_ground;
	}

	return feet;
}

// How far the player moved sideways since the last ground check (a slope
// rises by up to that much in the meantime), capped

static f32 clipPlayerMoveReach(struct player *player)
{
	f32 dx, dz, dist = 0.0f;

	if (g_ClipLastGroundValid) {
		dx = player->prop->pos.x - g_ClipLastGroundX;
		dz = player->prop->pos.z - g_ClipLastGroundZ;
		dist = sqrtf(dx * dx + dz * dz);

		if (dist > CLIP_MAXMOVEREACH) {
			dist = dist > 200.0f ? 0.0f : CLIP_MAXMOVEREACH; // a warp isn't a move
		}
	}

	g_ClipLastGroundX = player->prop->pos.x;
	g_ClipLastGroundZ = player->prop->pos.z;
	g_ClipLastGroundValid = true;

	return dist;
}

/**
 * Debug view on: what the ground check saw, twice a second (and whenever the
 * ground changes source), plus the objects around the player with their
 * collision: for places where the game's floor, the drawn floor and objects
 * disagree (glass floors, covered pits).
 */
static void clipDbgLogGround(struct player *player, f32 ground, s32 found, f32 y, f32 objground, s32 objfound, f32 base, const char *chosen)
{
	static s32 lastframe = -1000;
	static const char *lastchosen = NULL;
	s16 propnums[256];
	s32 i;

	if (!g_ClipDebug) {
		return;
	}

	if (g_Vars.lvframe60 - lastframe < 30 && chosen == lastchosen) {
		return;
	}

	lastframe = g_Vars.lvframe60;
	lastchosen = chosen;

	sysLogPrintf(LOG_NOTE, "CLIP: ground at (%.0f %.0f %.0f) feet %.1f base %.1f falling %d: game %.1f (room %d flags 0x%x), drawn %s%.1f, objects %s%.1f -> %s",
			player->prop->pos.x, player->prop->pos.y, player->prop->pos.z, player->vv_manground, base, player->isfalling,
			ground, player->floorroom, player->floorflags,
			found ? "" : "none ", found ? y + player->bondonground : 0.0f,
			objfound ? "" : "none ", objfound ? objground : 0.0f,
			chosen ? chosen : "drawn floor");

	roomGetProps(player->prop->rooms, propnums, 256);

	for (i = 0; propnums[i] >= 0; i++) {
		struct prop *prop = &g_Vars.props[propnums[i]];
		f32 dx = prop->pos.x - player->prop->pos.x;
		f32 dz = prop->pos.z - player->prop->pos.z;
		u8 *start, *end;
		struct geo *geo;
		s32 n = 0;

		if (prop->type != PROPTYPE_OBJ || dx * dx + dz * dz > 250.0f * 250.0f || prop->obj == NULL) {
			continue;
		}

		sysLogPrintf(LOG_NOTE, "CLIP:   obj type %d model %d pad %d at (%.0f %.0f %.0f) flags 0x%08x cd objs %d glass %d bg %d",
				prop->obj->type, prop->obj->modelnum, prop->obj->pad, prop->pos.x, prop->pos.y, prop->pos.z, prop->obj->flags,
				propIsOfCdType(prop, CDTYPE_OBJS), propIsOfCdType(prop, CDTYPE_PATHBLOCKER), propIsOfCdType(prop, CDTYPE_BG));

		if (!propUpdateGeometry(prop, &start, &end)) {
			sysLogPrintf(LOG_NOTE, "CLIP:     no geometry");
			continue;
		}

		geo = (struct geo *) start;

		while ((u8 *) geo < end && n < 6) {
			f32 ymin = 1e9f, ymax = -1e9f;
			s32 k;
			s32 gtype = geo->type;
			s32 gflags = geo->flags;

			if (geo->type == GEOTYPE_TILE_I) {
				struct geotilei *tile = (struct geotilei *) geo;

				for (k = 0; k < geo->numvertices; k++) {
					if (tile->vertices[k][1] < ymin) ymin = tile->vertices[k][1];
					if (tile->vertices[k][1] > ymax) ymax = tile->vertices[k][1];
				}

				geo = (struct geo *) ((u8 *) geo + (uintptr_t) (geo->numvertices - 0x40) * 6 + 0x18e);
			} else if (geo->type == GEOTYPE_TILE_F) {
				struct geotilef *tile = (struct geotilef *) geo;

				for (k = 0; k < geo->numvertices; k++) {
					if (tile->vertices[k].y < ymin) ymin = tile->vertices[k].y;
					if (tile->vertices[k].y > ymax) ymax = tile->vertices[k].y;
				}

				geo = (struct geo *) ((u8 *) geo + (uintptr_t) (geo->numvertices - 0x40) * 12 + 0x310);
			} else if (geo->type == GEOTYPE_BLOCK) {
				ymin = ((struct geoblock *) geo)->ymin;
				ymax = ((struct geoblock *) geo)->ymax;
				geo = (struct geo *) ((u8 *) geo + sizeof(struct geoblock));
			} else if (geo->type == GEOTYPE_CYL) {
				ymin = ((struct geocyl *) geo)->ymin;
				ymax = ((struct geocyl *) geo)->ymax;
				geo = (struct geo *) ((u8 *) geo + sizeof(struct geocyl));
			} else {
				break;
			}

			sysLogPrintf(LOG_NOTE, "CLIP:     geo type %d flags 0x%04x y %.1f..%.1f", gtype, gflags, ymin, ymax);
			n++;
		}
	}
}

/**
 * The highest a walkable surface may be for the feet to get onto it: a step
 * above the step base, plus how far a slope can rise during the move.
 */
static f32 clipPlayerWalkTop(struct player *player, f32 base, f32 move)
{
	return base + CLIP_STEP_WALKABLE + move;
}

static f32 clipPlayerGroundInner(f32 ground, s32 inlift)
{
	struct player *player = g_Vars.currentplayer;
	f32 feet, y, lowest, base, move, walktop, prevfeet;
	s32 found;
	s32 generated = CLIP_PLAYERMODE == CLIPMODE_GENERATED;

	if (!g_ClipRenderFloors || !g_ClipReady || CLIP_PLAYERMODE == CLIPMODE_GAME || player == NULL || player->prop == NULL) {
		return ground;
	}

	feet = player->vv_manground;
	g_ClipDbgGameGround = ground;
	g_ClipDbgGroundRaised = false;

	// Where the player last stood (before this tick's move)
	if (!g_ClipLastSolidValid || (!player->isfalling && feet <= player->vv_ground + 2.0f)) {
		g_ClipLastSolidY = feet;
		g_ClipLastSolidValid = true;
	}

	move = clipPlayerMoveReach(player);
	base = clipPlayerStepBase(player);

	prevfeet = g_ClipPrevFeetValid ? g_ClipPrevFeet : feet;
	g_ClipPrevFeet = feet;
	g_ClipPrevFeetValid = true;

	if (inlift || player->onladder) {
		return ground;
	}

	// Both: only surfaces above the game's own ground matter.
	// Generated only: the game's ground doesn't count.
	lowest = !generated && ground > -30000.0f ? ground : feet - 4000.0f;

	clipQueryPlayerRooms(player->bond2.radius + 8.0f, player->bond2.radius + 20.0f, lowest - 10.0f,
			clipPlayerWalkTop(player, base, move) + 10.0f);

	if (generated) {
		f32 sweeptop;

		// From as high as the feet can have stepped up to since the last
		// check (a fast run up a slope rises more per frame)
		walktop = clipPlayerWalkTop(player, base, move);
		sweeptop = walktop;

		// Falling: from where the feet were last time, so a surface the
		// body dropped into during the last frame is still found
		g_ClipLandTop = -1e9f;

		if (player->isfalling && prevfeet > feet && prevfeet < feet + 400.0f) {
			g_ClipLandTop = prevfeet + 2.0f;

			if (g_ClipLandTop > sweeptop) {
				sweeptop = g_ClipLandTop;
			}
		}

		g_ClipGameFloorTop = ground > -29000.0f ? ground - player->bondonground : -1e9f;
		found = clipBodyGround(player->prop->pos.x, player->prop->pos.z, player->bond2.radius * 0.6f,
				base, base + CLIP_STEP, walktop, sweeptop, lowest, &y);
		g_ClipLandTop = -1e9f;
		g_ClipGameFloorTop = -1e9f;
	} else {
		found = clipcheckFindFloor(player->prop->pos.x, player->prop->pos.z, player->bond2.radius * 0.7f,
				feet + CLIP_STEP, lowest, &y);
	}

	clipQueryAllRooms();

	// Generated only: the game's level tiles are left out, but objects still
	// count (glass floor panels, crates, platforms): their tops are ground
	// as the game has it
	if (generated) {
		struct collision collisions[21];
		struct collision *col = NULL;
		struct coord testpos = player->prop->pos;
		f32 objground = -30000.0f;

		cdCollectGeoForCyl(&testpos, player->bond2.radius, player->prop->rooms, CDTYPE_ALL & ~CDTYPE_BG,
				GEOFLAG_FLOOR1 | GEOFLAG_FLOOR2, CHECKVERTICAL_NO, 0, 0, collisions, 20);
		objground = cdFindGroundFromList(collisions, &testpos, &col, player->bond2.radius);

		if (col && objground > -30000.0f && objground <= clipPlayerWalkTop(player, base, move)) {
			objground += player->bondonground;

			if (!found || objground > y + player->bondonground) {
				return objground;
			}
		}

		// The game's own floor over a drawn pit (Defection's lift gaps, where
		// glass panels stand in a recess the game floors over): stand on it
		// as the game does, also to climb back out once fallen in (the game
		// takes any floor below the eyes). Only when it's well above what's
		// drawn (a shallow recess counts, the small rounding between the
		// game's tiles and the drawn floor doesn't).
		if (ground > -30000.0f && (!found || ground > y + player->bondonground + CLIP_PIT_MIN)
				&& ground <= player->prop->pos.y - 10.0f) {
			clipDbgLogGround(player, ground, found, y, objground, col != NULL, base, "game floor over a drawn pit");
			return ground;
		}

		clipDbgLogGround(player, ground, found, y, objground, col != NULL, base, NULL);
	}

	if (!found) {
		// Generated only, nothing drawn below (a room not seen yet): the
		// game's ground rather than falling out of the level
		return ground;
	}

	y += player->bondonground;

	if (!generated) {
		// The game's smooth stair ramps stay (drawn steps would make stairs bumpy)
		if (ground > -30000.0f && y <= ground + CLIP_STAIR_TOLERANCE) {
			return ground;
		}
	}

	// Too far down onto a floor the game kills on (a pit's bottom) or with
	// no floor of the game's at all: a pit, not a floor. Long falls onto a
	// floor the game has (a shaft, a drop between levels) are caught.
	// (No floor of the game's at all, as under Villa's mountainsides: only a
	// much longer fall is a pit, a jump off a cliff lands on the slope)
	if ((y < g_ClipLastSolidY - g_ClipMaxCatchFall && (player->floorflags & GEOFLAG_DIE))
			|| (y < g_ClipLastSolidY - g_ClipMaxCatchFall * 4.0f && ground <= -29000.0f)) {
		return ground;
	}

	// Standing above the game's floor tile: its "deadly" flag is for the
	// floor down there (a pit under a drawn bridge), not this surface
	if (y > ground + 5.0f) {
		player->floorflags &= ~GEOFLAG_DIE;
	}

	if (g_ClipDebug && (generated ? y != ground : true)) {
		f32 p[3] = { player->prop->pos.x, y, player->prop->pos.z };

		g_ClipDbgGroundRaised = true;
		g_ClipDbgDrawnGround = y;
		clipDbgMark(p, CLIPDBG_FLOOR);
	}

	return y;
}

/**
 * The tops of objects (crates, consoles, tables) the feet can stand on. The
 * game has floors on only a few objects (glass panels): landing on a crate
 * (a jump, a drop from a ledge) left the player hanging in the air on it,
 * held up by it. Their blocks and cylinders count, up to maxtop (-30000 if
 * none).
 */
static f32 clipObjectTopUnder(struct player *player, f32 maxtop)
{
	struct collision collisions[21];
	struct coord pos = player->prop->pos;
	f32 best = -30000.0f;
	s32 i;

	cdCollectGeoForCyl(&pos, player->bond2.radius * 0.6f, player->prop->rooms, CDTYPE_OBJS,
			GEOFLAG_WALL | GEOFLAG_BLOCK_SIGHT | GEOFLAG_BLOCK_SHOOT, CHECKVERTICAL_NO, 0, 0, collisions, 20);

	for (i = 0; i < 20 && collisions[i].geo; i++) {
		struct geo *geo = collisions[i].geo;
		f32 top;

		if (collisions[i].prop == NULL || collisions[i].prop->type != PROPTYPE_OBJ) {
			continue;
		}

		if (geo->type == GEOTYPE_BLOCK) {
			top = ((struct geoblock *) geo)->ymax;
		} else if (geo->type == GEOTYPE_CYL) {
			top = ((struct geocyl *) geo)->ymax;
		} else {
			continue;
		}

		if (top <= maxtop && top > best) {
			best = top;
		}
	}

	return best;
}

f32 clipcheckPlayerGround(f32 ground, s32 inlift)
{
	struct player *player = g_Vars.currentplayer;
	f32 result = clipPlayerGroundInner(ground, inlift);
	f32 base, top;

	if (!g_ClipReady || player == NULL || player->prop == NULL || inlift || player->onladder) {
		return result;
	}

	// Death floors: reaching one (standing on it, or fallen through it) kills
	if (g_ClipNumDeathTris > 0 && CLIP_PLAYERMODE != CLIPMODE_GAME && !player->isdead) {
		f32 dy;

		if (clipDeathFloorAt(player->prop->pos.x, player->prop->pos.z, &dy)
				&& player->vv_manground <= dy + CLIP_DEATHFLOOR_REACH
				&& player->vv_manground >= dy - 400.0f
				&& result <= dy + CLIP_DEATHFLOOR_REACH) {
			sysLogPrintf(LOG_NOTE, "CLIP: player %d reached a death floor at %.0f %.0f %.0f",
					g_Vars.currentplayernum, player->prop->pos.x, dy, player->prop->pos.z);
			playerDie(true);
			return result;
		}
	}

	// Game mode stays as the original, except when the player can jump
	if (CLIP_PLAYERMODE == CLIPMODE_GAME) {
		return result;
	}

	base = clipPlayerStepBase(player);
	// Up to the bottom of the player's collision box (feet + 30): lower
	// objects don't block the legs and are stepped onto; a body dropping
	// onto a taller one is held up by it with the feet that far below its top
	top = clipObjectTopUnder(player, base + CLIP_STEP + 0.5f);

	if (top > -30000.0f && top + player->bondonground > result) {
		result = top + player->bondonground;

		// Standing above the game's floor tile: its "deadly" flag is for down there
		if (result > ground + 5.0f) {
			player->floorflags &= ~GEOFLAG_DIE;
		}
	}

	return result;
}

/**
 * bwalkTryMoveUpwards: a drawn ceiling over the head stops the move (the
 * game then treats it like its own ceilings: the climb ends and gravity
 * takes over).
 */
/**
 * Standing up (bwalkTryMoveUpwards / bwalkCanMoveUpwards with amount 0, the
 * bbox already at the new height): the body must fit where it is. Crouched
 * under an inward-leaning wall or a low overhang, standing up would put the
 * head inside it.
 */
static s32 clipPlayerBodyFits(void)
{
	static pdphysContact contacts[CLIP_MAXCONTACTS];
	struct player *player = g_Vars.currentplayer;
	f32 radius, ymax, ymin;
	f32 feet = player->vv_manground;
	f32 low, top, half, r;
	f32 a[3], b[3];
	s32 num, i;
	s32 fits = true;

	playerGetBbox(player->prop, &radius, &ymax, &ymin);

	low = feet + (CLIP_PLAYERMODE == CLIPMODE_GENERATED ? CLIP_STEP + 2.0f : CLIP_WALL_LOW);
	top = ymax;

	if (top <= low + 4.0f) {
		return true;
	}

	r = radius * 0.85f; // a little inside the sides: touching a wall is fine
	half = (top - low) * 0.5f;

	if (r > half) {
		r = half;
	}

	a[0] = player->prop->pos.x; a[1] = low + half; a[2] = player->prop->pos.z;
	b[0] = a[0]; b[1] = a[1] + 1.0f; b[2] = a[2];

	clipQueryPlayerRooms(radius + 8.0f, radius + 20.0f, feet - 10.0f, ymax + 20.0f);
	num = pdphysSweepCapsuleContacts(a, b, r, half - r,
			(1u << CLIPCHECK_GROUP_FLOORS) | (1u << CLIPCHECK_GROUP_CEILINGS) | CLIP_WALL_GROUPS,
			contacts, CLIP_MAXCONTACTS);
	clipQueryAllRooms();

	for (i = 0; i < num; i++) {
		pdphysContact *c = &contacts[i];

		if (c->fraction > 0.0f || clipLadderIgnores(c->point)) {
			continue; // not overlapping now (or part of a ladder)
		}

		// Something to stand on near the feet doesn't count
		if (c->normal[1] >= CLIP_WALL_STEEP && c->point[1] <= feet + CLIP_STEP + 12.0f) {
			continue;
		}

		fits = false;
		clipDbgMark(c->point, CLIPDBG_CEILING);
		break;
	}

	return fits;
}

s32 clipcheckPlayerCeilingBlocks(f32 amount)
{
	struct player *player = g_Vars.currentplayer;
	f32 radius, ymax, ymin, y;
	f32 ceil[3];

	if (!g_ClipRenderCeilings || !g_ClipReady || CLIP_PLAYERMODE == CLIPMODE_GAME
			|| amount < 0.0f || player == NULL || player->prop == NULL) {
		return false;
	}

	// Standing up from a crouch
	if (amount == 0.0f) {
		if (!clipPlayerBodyFits()) {
			cdClearResults();
			return true;
		}

		return false;
	}

	// Only for rising off the ground (jumps, mods that lift the player):
	// following the ground (stairs, ramps, lifts) is the game's own business,
	// and it lets the head clip into low stair undersides and the like
	if (player->vv_manground + amount <= player->vv_ground + 2.0f) {
		return false;
	}

	playerGetBbox(player->prop, &radius, &ymax, &ymin);

	clipQueryPlayerRooms(radius + 8.0f, radius + 20.0f, ymin - 10.0f, ymax + amount + 20.0f);

	if (!clipcheckFindCeiling(player->prop->pos.x, player->prop->pos.z, radius * 0.6f,
				ymax - CLIP_HEAD_SINK, ymax + amount + 1.0f, &y)) {
		clipQueryAllRooms();
		return false;
	}

	clipQueryAllRooms();

	ceil[0] = player->prop->pos.x;
	ceil[1] = y;
	ceil[2] = player->prop->pos.z;

	if (clipLadderIgnores(ceil)) {
		return false; // a hatch the ladder climbs through
	}

	if (g_ClipDebug) {
		clipDbgMark(ceil, CLIPDBG_CEILING);
	}

	cdClearResults(); // no stale obstacle for the caller to act on
	return true;
}

/**
 * How far along from -> to (0..1) a body's centre can go at height y before
 * coming within CLIP_WALL_MARGIN of a drawn wall (steep slopes and overhangs
 * count too). 1 if nothing is in the way. Fills the wall's horizontal normal.
 */
f32 clipcheckWallFraction(const struct coord *from, const struct coord *to, f32 y, f32 *outnx, f32 *outnz)
{
	f32 dx = to->x - from->x;
	f32 dz = to->z - from->z;
	f32 len = sqrtf(dx * dx + dz * dz);
	f32 a[3], b[3], hit[3], nrm[3];
	f32 dist, frac;

	if (!g_ClipReady || len < 0.001f) {
		return 1.0f;
	}

	a[0] = from->x;
	a[1] = y;
	a[2] = from->z;
	b[0] = to->x + dx / len * CLIP_WALL_MARGIN;
	b[1] = y;
	b[2] = to->z + dz / len * CLIP_WALL_MARGIN;

	if (!pdphysRaycastLevelGroups(a, b, CLIP_WALL_GROUPS | (1u << CLIPCHECK_GROUP_FLOORS) | (1u << CLIPCHECK_GROUP_CEILINGS), hit, nrm)) {
		return 1.0f;
	}

	// A walkable floor or a flat-ish ceiling isn't a wall
	if (nrm[1] >= CLIP_WALL_STEEP || nrm[1] <= CLIP_WALL_OVERHANG) {
		return 1.0f;
	}

	dist = sqrtf((hit[0] - a[0]) * (hit[0] - a[0]) + (hit[2] - a[2]) * (hit[2] - a[2]));
	frac = (dist - CLIP_WALL_MARGIN) / len;

	if (frac < 0.0f) frac = 0.0f;
	if (frac > 1.0f) frac = 1.0f;

	{
		f32 nl = sqrtf(nrm[0] * nrm[0] + nrm[2] * nrm[2]);

		*outnx = nl > 0.001f ? nrm[0] / nl : -dx / len;
		*outnz = nl > 0.001f ? nrm[2] / nl : -dz / len;
	}

	return frac;
}

/**
 * How far (0..1) a standing body can move from -> to before its sides touch a
 * drawn wall: a vertical capsule of the given radius from low to top (world
 * y), swept horizontally. Walkable floors and flat ceilings don't count, nor
 * do surfaces the move doesn't go into, nor drawn surfaces that have the
 * game's own collision (floor or wall tiles) at them: those are left to the
 * game, so its clipping is respected (e.g. steep ramps it lets you walk) and
 * only the parts of the map without clipping are filled in. Fills the wall's
 * horizontal normal.
 */
// Something facing down right above x/z between y0 and y1
static s32 clipCeilingAbove(f32 x, f32 z, f32 y0, f32 y1)
{
	f32 a[3] = { x, y0, z };
	f32 b[3] = { x, y1, z };
	f32 hit[3], hitn[3];

	return y1 > y0 && pdphysRaycastLevelGroups(a, b, 1u << CLIPCHECK_GROUP_CEILINGS, hit, hitn);
}

f32 clipcheckBodyFraction(const struct coord *from, const struct coord *to, f32 low, f32 top, f32 radius, f32 *outnx, f32 *outnz)
{
	static pdphysContact contacts[CLIP_MAXCONTACTS];
	f32 dx = to->x - from->x;
	f32 dz = to->z - from->z;
	f32 len = sqrtf(dx * dx + dz * dz);
	f32 a[3], b[3];
	f32 half, best = 2.0f;
	f32 dirx, dirz;
	s32 num, i;

	if (!g_ClipReady || len < 0.001f || top <= low) {
		return 1.0f;
	}

	dirx = dx / len;
	dirz = dz / len;
	half = (top - low) * 0.5f;

	if (radius > half) {
		radius = half;
	}

	a[0] = from->x;
	a[1] = low + half;
	a[2] = from->z;
	b[0] = to->x;
	b[1] = a[1];
	b[2] = to->z;

	if (g_ClipDebug) {
		g_ClipDbgCapsule[0] = low;
		g_ClipDbgCapsule[1] = top;
		g_ClipDbgCapsule[2] = radius;
		g_ClipDbgCapsule[3] = 1.0f;
	}

	num = pdphysSweepCapsuleContacts(a, b, radius, half - radius,
			CLIP_WALL_GROUPS | (1u << CLIPCHECK_GROUP_FLOORS) | (1u << CLIPCHECK_GROUP_CEILINGS),
			contacts, CLIP_MAXCONTACTS);

	for (i = 0; i < num; i++) {
		pdphysContact *c = &contacts[i];
		f32 nl, nx, nz;
		f32 p0[3], p1[3], hit[3], hitn[3];

		if (c->fraction >= best) {
			break; // sorted: nothing earlier left
		}

		if (clipLadderIgnores(c->point)) {
			continue;
		}

		// Floors to stand on are the ground's business, as long as they're
		// within step reach. Anything else counts, including the tops of
		// things too high to step onto and sloping ceilings (inward-leaning
		// walls, overhangs) that the head runs into.
		if (c->normal[1] >= CLIP_WALL_STEEP && c->point[1] <= g_ClipBodyFeet + g_ClipBodyStepTop) {
			continue;
		}

		// Which way it pushes the body: from the contact towards the body's
		// axis (at the time of contact), horizontally. The surface normal
		// alone can't tell for edges and corners, or a slope's edge met
		// from the side.
		{
			f32 cx = a[0] + (b[0] - a[0]) * c->fraction;
			f32 cz = a[2] + (b[2] - a[2]) * c->fraction;

			nx = cx - c->point[0];
			nz = cz - c->point[2];
			nl = sqrtf(nx * nx + nz * nz);
		}

		// Right above or below the axis: a ceiling grazed by the head or
		// something under the feet, not in the way of walking
		if (nl < radius * 0.3f) {
			continue;
		}

		nx /= nl;
		nz /= nl;

		// Only surfaces the move goes into (sliding along or moving away is fine)
		if (dirx * nx + dirz * nz > -0.05f) {
			continue;
		}

		// The game's own collision right at it: the game decides
		p0[0] = c->point[0] + c->normal[0] * CLIP_COVER_DIST;
		p0[1] = c->point[1] + c->normal[1] * CLIP_COVER_DIST;
		p0[2] = c->point[2] + c->normal[2] * CLIP_COVER_DIST;
		p1[0] = c->point[0] - c->normal[0] * CLIP_COVER_DIST;
		p1[1] = c->point[1] - c->normal[1] * CLIP_COVER_DIST;
		p1[2] = c->point[2] - c->normal[2] * CLIP_COVER_DIST;

		if (CLIP_PLAYERMODE != CLIPMODE_GENERATED && pdphysRaycastLevelGroups(p0, p1, CLIP_PD_GROUPS, hit, hitn)) {
			clipDbgMark(c->point, CLIPDBG_COVERED);
			continue;
		}

		// A step: just past the edge there's something to stand on, not far
		// above the contact (stair risers, curbs, ledges you walk onto).
		// Never the underside of something the body is already under (a
		// slope or deck met from below: its top is on the other side). From
		// beside it (back onto a ledge just fallen from) it's a step.
		if (CLIP_PLAYERMODE == CLIPMODE_GENERATED && c->normal[1] < -0.2f && clipCeilingAbove(a[0], a[2], g_ClipBodyFeet + 2.0f, c->point[1] + CLIP_STEP_OVER)) {
			best = c->fraction;
			*outnx = nx;
			*outnz = nz;
			g_ClipLastBlock[0] = c->point[0]; g_ClipLastBlock[1] = c->point[1]; g_ClipLastBlock[2] = c->point[2]; g_ClipLastBlock[3] = -9.0f; // underside
			clipDbgMark(c->point, CLIPDBG_BLOCK);
			continue;
		}

		p0[0] = c->point[0] + dirx * 6.0f;
		p0[1] = c->point[1] + CLIP_STEP_OVER;
		p0[2] = c->point[2] + dirz * 6.0f;
		p1[0] = p0[0];
		p1[1] = c->point[1] - 2.0f;
		p1[2] = p0[2];

		if (pdphysRaycastLevelGroups(p0, p1, (CLIP_PLAYERMODE == CLIPMODE_GENERATED ? 0 : CLIP_PD_GROUPS) | (1u << CLIPCHECK_GROUP_FLOORS), hit, hitn)
				&& hitn[1] >= CLIP_FLOOR_WALKABLE
				&& hit[1] <= g_ClipBodyFeet + g_ClipBodyStepTop) {
			clipDbgMark(c->point, CLIPDBG_STEP);
			continue;
		}

		best = c->fraction;
		*outnx = nx;
		*outnz = nz;
		g_ClipLastBlock[0] = c->point[0]; g_ClipLastBlock[1] = c->point[1]; g_ClipLastBlock[2] = c->point[2]; g_ClipLastBlock[3] = c->normal[1];
		clipDbgMark(c->point, CLIPDBG_BLOCK);
	}

	if (best > 1.0f) {
		return 1.0f;
	}

	// Stop just short of it
	best -= 0.5f / len;

	if (best < 0.0f) best = 0.0f;

	if (g_ClipDebug) {
		g_ClipDbgBlockFrame = g_Vars.lvframe60;
		g_ClipDbgBlockFrac = best;
	}

	return best;
}

/**
 * bwalkCalculateNewPosition, after the game found nothing in the way: a
 * drawn wall without collision tiles (or above the top of the game's wall
 * tiles, e.g. when jumping) stops the move. The body is a capsule from
 * CLIP_WALL_LOW above the feet (lower bumps are stepped over) to the top of
 * the head, so slopes, rock faces and corners stop it where they're drawn.
 * The wall is reported to the game as its own obstacle edge (with how much
 * of the move was possible), so the game's usual sliding along walls takes
 * over.
 */
/**
 * Whether a body standing on the ground at x/z (the same sphere drop as the
 * ground check) would be inside something facing down: from above step
 * height to just under the top of the head, a little inside its sides.
 * Crouched it's shorter, so it fits further in under things.
 */
static s32 clipBodyUnderside(struct player *player, f32 x, f32 z, f32 base, f32 height, f32 walktop, f32 *outgy)
{
	static pdphysContact contacts[CLIP_MAXCONTACTS];
	f32 gy, low, top, half, r;
	f32 a[3], b[3];
	s32 num, k;

	if (!clipBodyGround(x, z, player->bond2.radius * 0.6f, base, base + CLIP_STEP, walktop, walktop, base - 200.0f, &gy)) {
		return false;
	}

	gy += player->bondonground;
	*outgy = gy;

	low = gy + CLIP_STEP + 2.0f;
	top = gy + height - 6.0f;
	r = player->bond2.radius * 0.8f;

	if (top <= low + 4.0f) {
		return false;
	}

	half = (top - low) * 0.5f;

	if (r > half) {
		r = half;
	}

	a[0] = x; a[1] = low + half; a[2] = z;
	b[0] = x; b[1] = a[1] + 1.0f; b[2] = z;

	num = pdphysSweepCapsuleContacts(a, b, r, half - r, 1u << CLIPCHECK_GROUP_CEILINGS, contacts, CLIP_MAXCONTACTS);

	for (k = 0; k < num; k++) {
		if (contacts[k].fraction <= 0.0f && contacts[k].normal[1] < -0.2f && !clipLadderIgnores(contacts[k].point)) {
			return true;
		}
	}

	return false;
}

/**
 * Whether a body at x/z (feet as given) would be inside a drawn wall or
 * underside: a capsule from above step height to just under the top of the
 * head, a little thinner than the body. The sideways sweep stops the body
 * short of walls; this catches what it can miss (walls leaning in or out,
 * met at an angle by the slide along another wall).
 *
 * It starts above a stair step. Up to the highest ledge the body can step
 * onto, only slanted surfaces count: the upright edge of a ledge being walked
 * up to isn't a wall (from a 41 high recess, Defection's lift slots, the top
 * of its edge counted and the body could never step out), but the foot of a
 * slanted wall is (walking into it climbed the body up and over it).
 */
static s32 clipBodyInsideWall(f32 x, f32 z, f32 feet, f32 height, f32 radius)
{
	static pdphysContact contacts[CLIP_MAXCONTACTS];
	f32 low = feet + CLIP_STEP + 2.0f;
	f32 ledgetop = feet + CLIP_STEP_WALKABLE + 2.0f;
	f32 top = feet + height - 6.0f;
	f32 a[3], b[3], half, r = radius * 0.85f;
	s32 num, k;

	if (top <= low + 4.0f) {
		return false;
	}

	half = (top - low) * 0.5f;

	if (r > half) {
		r = half;
	}

	a[0] = x; a[1] = low + half; a[2] = z;
	b[0] = x; b[1] = a[1] + 1.0f; b[2] = z;

	num = pdphysSweepCapsuleContacts(a, b, r, half - r, CLIP_WALL_GROUPS | (1u << CLIPCHECK_GROUP_CEILINGS), contacts, CLIP_MAXCONTACTS);

	for (k = 0; k < num; k++) {
		if (contacts[k].fraction <= 0.0f && !clipLadderIgnores(contacts[k].point)) {
			if (contacts[k].point[1] < ledgetop
					&& (fabsf(contacts[k].normal[1]) < 0.1f || contacts[k].normal[1] >= CLIP_FLOOR_MINUP)) {
				continue; // a ledge's upright edge, or the top of it
			}

			return true;
		}
	}

	return false;
}

s32 clipcheckPlayerMoveBlocked(struct coord *from, struct coord *to, f32 ymax)
{
	struct player *player = g_Vars.currentplayer;
	f32 feet, low, top, radius;
	f32 best, nx = 0.0f, nz = 0.0f;

	if (!g_ClipRenderWalls || !g_ClipReady || CLIP_PLAYERMODE == CLIPMODE_GAME || player == NULL || player->prop == NULL) {
		return false;
	}

	feet = player->vv_manground;

	// Generated only: nothing else stops low obstacles, so the body starts
	// just above what can be stepped onto
	low = feet + (CLIP_PLAYERMODE == CLIPMODE_GENERATED ? CLIP_STEP + 2.0f : CLIP_WALL_LOW);

	if (CLIP_PLAYERMODE == CLIPMODE_GENERATED) {
		f32 dx = to->x - from->x;
		f32 dz = to->z - from->z;
		f32 move = sqrtf(dx * dx + dz * dz);

		if (move > CLIP_MAXMOVEREACH) {
			move = CLIP_MAXMOVEREACH;
		}

		// The whole body: nothing else keeps it out of the walls. Steps
		// count from where the body really stands (the feet lag behind
		// on the way up), and a walkable slope may rise by the move.
		feet = clipPlayerStepBase(player);
		low = feet + CLIP_STEP + 2.0f;
		top = ymax + 2.0f;
		radius = player->bond2.radius;
		g_ClipBodyStepTop = CLIP_STEP_WALKABLE + move;
	} else {
		top = ymax - 4.0f;
		radius = player->bond2.radius - CLIP_BODY_INSET;
		g_ClipBodyStepTop = CLIP_WALL_LOW + 24.0f;
	}

	g_ClipBodyFeet = feet;

	if (top < low + 10.0f) {
		low = top - 10.0f; // crouched very low
	}

	{
		f32 dx = to->x - from->x, dz = to->z - from->z;

		f32 move = sqrtf(dx * dx + dz * dz);

		clipQueryPlayerRooms(move + player->bond2.radius + 8.0f, move + player->bond2.radius + 60.0f, feet - 60.0f, ymax + 60.0f);
		best = clipcheckBodyFraction(from, to, low, top, radius, &nx, &nz);

		// Generated only: walking up a ramp or slope that runs under
		// something (a deck, a sloping underside), the ground lifts the body
		// and the sideways sweep doesn't see the head going up into it. A
		// move that would leave the body standing inside something facing
		// down stops, unless it's already inside here (then any move is
		// fine, so it can always get out), or the move goes down.
		if (best >= 1.0f && CLIP_PLAYERMODE == CLIPMODE_GENERATED && move > 0.01f) {
			f32 height = ymax - player->vv_manground;
			f32 wt = clipPlayerWalkTop(player, feet, move);
			f32 gyto = 0.0f, gyhere = 0.0f;

			if (clipBodyUnderside(player, to->x, to->z, feet, height, wt, &gyto)
					&& gyto >= feet - 2.0f
					&& !clipBodyUnderside(player, from->x, from->z, feet, height, wt, &gyhere)) {
				best = 0.0f;
				nx = -dx / move;
				nz = -dz / move;
				clipDbgMark(&to->x, CLIPDBG_CEILING);
			}
		}

		// Generated only: never into a wall the sweep missed (a wall leaning
		// over or away, met at an angle). Already inside one here, any move
		// is fine so it can always get out.
		if (best >= 1.0f && CLIP_PLAYERMODE == CLIPMODE_GENERATED && move > 0.01f) {
			f32 height = ymax - player->vv_manground;
			f32 wt = clipPlayerWalkTop(player, feet, move);
			f32 tofeet = feet, gy;

			// Where the body will stand there: up a slope beside a wall
			// leaning in, the ground lifts it into the wall
			if (clipBodyGround(to->x, to->z, player->bond2.radius * 0.6f, feet, feet + CLIP_STEP, wt, wt, feet - 200.0f, &gy)
					&& gy + player->bondonground > tofeet) {
				tofeet = gy + player->bondonground;
			}

			// (The second test is for the ground rising under a leaning wall:
			// the body would climb it. A ledge's upright edge doesn't count
			// in it, so stepping out of Defection's lift pits still works.)
			if ((clipBodyInsideWall(to->x, to->z, tofeet, height, radius)
						|| (tofeet > feet + 1.0f
							&& clipBodyInsideWall(to->x, to->z, feet, height, radius)))
					&& !clipBodyInsideWall(from->x, from->z, feet, height, radius)) {
				best = 0.0f;
				nx = -dx / move;
				nz = -dz / move;
				clipDbgMark(&to->x, CLIPDBG_BLOCK);

				if (clipLog()) {
					sysLogPrintf(LOG_NOTE, "CLIP: move into a wall stopped at (%.0f %.0f)", to->x, to->z);
				}
			}
		}

		clipQueryAllRooms();
	}

	if (best >= 1.0f) {
		return false;
	}

	if (clipLog()) {
		static s32 lastframe = -1000;

		if (g_Vars.lvframe60 - lastframe >= 15) {
			lastframe = g_Vars.lvframe60;
			sysLogPrintf(LOG_NOTE, "CLIP: move (%.0f %.0f)->(%.0f %.0f) blocked at %.2f, push (%.2f %.2f), feet %.1f ground %.1f, sweep contact (%.0f %.1f %.0f) n.y %.2f (or a destination check if best is 0)",
					from->x, from->z, to->x, to->z, best, nx, nz, player->vv_manground, player->vv_ground,
					g_ClipLastBlock[0], g_ClipLastBlock[1], g_ClipLastBlock[2], g_ClipLastBlock[3]);
			g_ClipLastBlock[3] = 99.0f;
		}
	}

	// The wall as the game's obstacle edge: a long line along it, so the
	// sliding code never takes it for a corner
	{
		struct coord v1, v2;
		f32 hx = from->x + (to->x - from->x) * best;
		f32 hz = from->z + (to->z - from->z) * best;

		v1.x = hx - nz * 500.0f;
		v1.y = from->y;
		v1.z = hz + nx * 500.0f;
		v2.x = hx + nz * 500.0f;
		v2.y = from->y;
		v2.z = hz - nx * 500.0f;

		cdSetObstacleVtxPropFlt(&v1, &v2, NULL, best);
	}

	if (clipLog()) {
		sysLogPrintf(LOG_NOTE, "CLIP: drawn wall stops the player at %.2f of the move (n %.2f %.2f)", best, nx, nz);
	}

	return true;
}

PD_CONSTRUCTOR static void clipcheckConfigInit(void)
{
	configRegisterInt("Game.ClipPlayerMode", &g_ClipPlayerMode, 0, 2);
	configRegisterInt("Game.ClipRenderFloors", &g_ClipRenderFloors, 0, 1);
	configRegisterInt("Game.ClipRenderCeilings", &g_ClipRenderCeilings, 0, 1);
	configRegisterInt("Game.ClipRenderWalls", &g_ClipRenderWalls, 0, 1);
	configRegisterInt("Game.ClipDebug", &g_ClipDebug, 0, 3);
	configRegisterFloat("Game.ClipMaxCatchFall", &g_ClipMaxCatchFall, 0.0f, 100000.0f);
}

#endif // PD_CLIPCHECK
