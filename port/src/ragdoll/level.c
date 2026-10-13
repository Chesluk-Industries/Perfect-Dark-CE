/**
 * MOD: ragdoll physics - level collision for the physics world: the game's
 * tiles, the drawn walls and ceilings, invisible walls cut down to what is
 * drawn. Part of the ragdoll module (port/src/ragdoll.c).
 */

#include "rd_internal.h"

#ifdef PD_PHYSICS

void mbReset(struct meshbuilder *mb)
{
	mb->numverts = 0;
	mb->numtris = 0;
}

void mbFree(struct meshbuilder *mb)
{
	free(mb->verts);
	free(mb->indices);
	memset(mb, 0, sizeof(*mb));
}

static s32 mbAddVert(struct meshbuilder *mb, f32 x, f32 y, f32 z)
{
	if (mb->numverts >= mb->maxverts) {
		mb->maxverts = mb->maxverts ? mb->maxverts * 2 : 1024;
		mb->verts = realloc(mb->verts, mb->maxverts * 3 * sizeof(f32));
	}

	mb->verts[mb->numverts * 3 + 0] = x;
	mb->verts[mb->numverts * 3 + 1] = y;
	mb->verts[mb->numverts * 3 + 2] = z;

	return mb->numverts++;
}

static void mbAddTri(struct meshbuilder *mb, s32 a, s32 b, s32 c)
{
	if (mb->numtris >= mb->maxtris) {
		mb->maxtris = mb->maxtris ? mb->maxtris * 2 : 1024;
		mb->indices = realloc(mb->indices, mb->maxtris * 3 * sizeof(s32));
	}

	mb->indices[mb->numtris * 3 + 0] = a;
	mb->indices[mb->numtris * 3 + 1] = b;
	mb->indices[mb->numtris * 3 + 2] = c;
	mb->numtris++;
}

// Fan-triangulates a convex polygon whose vertices were just added starting at 'first'
static void mbAddFan(struct meshbuilder *mb, s32 first, s32 count)
{
	s32 i;

	for (i = 1; i < count - 1; i++) {
		mbAddTri(mb, first, first + i, first + i + 1);
	}
}

// The triangles a G_TRI1/G_TRI4 command draws (vertex slots); 0 for other commands
static s32 rdGdlTris(const Gfx *iter, s32 tris[4][3])
{
	if (iter->dma.cmd == G_TRI1) {
		tris[0][0] = iter->tri.tri.v[GFX_TRI_VTX(0)] / 10;
		tris[0][1] = iter->tri.tri.v[GFX_TRI_VTX(1)] / 10;
		tris[0][2] = iter->tri.tri.v[GFX_TRI_VTX(2)] / 10;
		return 1;
	}

	if (iter->dma.cmd == G_TRI4) {
		tris[0][0] = iter->tri4.x1; tris[0][1] = iter->tri4.y1; tris[0][2] = iter->tri4.z1;
		tris[1][0] = iter->tri4.x2; tris[1][1] = iter->tri4.y2; tris[1][2] = iter->tri4.z2;
		tris[2][0] = iter->tri4.x3; tris[2][1] = iter->tri4.y3; tris[2][2] = iter->tri4.z3;
		tris[3][0] = iter->tri4.x4; tris[3][1] = iter->tri4.y4; tris[3][2] = iter->tri4.z4;
		return 4;
	}

	return 0;
}

/**
 * Converts one room's collision geometry (the same data the game's own
 * collision code in src/lib/collision.c walks) into triangles.
 * Size formulas for each geo type match stageParseTiles() in tilesreset.c.
 */
void ragdollBuildRoom(struct meshbuilder *mbfloor, struct meshbuilder *mbwall, s32 roomnum, struct geostats *stats)
{
	u8 *start = g_TileFileData.u8 + g_TileRooms[roomnum];
	u8 *end = g_TileFileData.u8 + g_TileRooms[roomnum + 1];
	struct geo *geo = (struct geo *) start;
	s32 i;

	for (; (u8 *) geo < end; geo = rdGeoNext(geo)) {
		// Walls go in their own group: the invisible ones are dropped later
		// once the room's render geometry is known (ragdollDropInvisibleWalls)
		struct meshbuilder *mb = (geo->flags & GEOFLAG_WALL) ? mbwall : mbfloor;

		if (geo->type == GEOTYPE_TILE_I) {
			struct geotilei *tile = (struct geotilei *) geo;
			s32 n = geo->numvertices;
			s32 first = mb->numverts;

			for (i = 0; i < n; i++) {
				mbAddVert(mb, tile->vertices[i][0], tile->vertices[i][1], tile->vertices[i][2]);
			}

			mbAddFan(mb, first, n);

			stats->tilei++;

			if (geo->flags & GEOFLAG_WALL) {
				stats->tileswall++;
			} else if (geo->flags & (GEOFLAG_FLOOR1 | GEOFLAG_FLOOR2)) {
				stats->tilesfloor++;
			} else {
				stats->tilesother++;
			}
		} else if (geo->type == GEOTYPE_TILE_F) {
			struct geotilef *tile = (struct geotilef *) geo;
			s32 n = geo->numvertices;
			s32 first = mb->numverts;

			for (i = 0; i < n; i++) {
				mbAddVert(mb, tile->vertices[i].x, tile->vertices[i].y, tile->vertices[i].z);
			}

			mbAddFan(mb, first, n);

			stats->tilef++;
		} else if (geo->type == GEOTYPE_BLOCK) {
			// Vertical prism: n x/z points, extruded from ymin to ymax
			struct geoblock *block = (struct geoblock *) geo;
			s32 n = geo->numvertices;
			s32 bottom = mb->numverts;
			s32 top;

			RD_CLAMPHI(n, 8);

			for (i = 0; i < n; i++) {
				mbAddVert(mb, block->vertices[i][0], block->ymin, block->vertices[i][1]);
			}

			top = mb->numverts;

			for (i = 0; i < n; i++) {
				mbAddVert(mb, block->vertices[i][0], block->ymax, block->vertices[i][1]);
			}

			for (i = 0; i < n; i++) {
				s32 next = (i + 1) % n;
				mbAddTri(mb, bottom + i, bottom + next, top + next);
				mbAddTri(mb, bottom + i, top + next, top + i);
			}

			mbAddFan(mb, bottom, n);
			mbAddFan(mb, top, n);

			stats->blocks++;
		} else if (geo->type == GEOTYPE_CYL) {
			// Not expected in level data; skipped for now
			stats->cyls++;
		} else {
			stats->unknown++;
			sysLogPrintf(LOG_WARNING, "PHYSICS: room %d has unknown geo type %d, skipping rest of room", roomnum, geo->type);
			break;
		}
	}
}

/**
 * Ceilings and walls from the render geometry. The collision tiles above only
 * have what the player can bump into: no ceilings (the game never needed
 * them) and no walls the player can't reach (behind ledges, across gaps,
 * high up), so bodies flew through those. They come from the room's render
 * geometry instead: every opaque triangle that isn't facing up (walls,
 * overhangs, ceilings). Up-facing render triangles are left to the collision
 * floors, so stairs stay the smooth ramps the game walks on.
 * That data is only in memory while a room is loaded (visible or recently
 * visible), so each room's geometry is added the first time it's seen loaded
 * and then kept until the stage ends.
 */
#define RD_CEIL_MINDOWN 0.35f // triangle normal.y at most -this counts as a ceiling (for the log)
#define RD_RENDER_MAXUP 0.35f // added when normal.y is below this (walls and ceilings; floors are steeper up)
#define RD_CEIL_ROOMSPERTICK 4
#define RD_VTXBATCHTYPE_OPA 0x01 // as in bg.c
#define RD_PASS_REACH  200.0f // translucent surfaces this close above a floor are within the player's reach
#define RD_PASS_BACKED 40.0f  // ... and solid for the player if collision is this close in front or behind

/**
 * Translucent render triangles the player walks through (light beams, holo
 * effects) mustn't stop bodies either: if the player can go through, so can
 * a body. A translucent triangle is passable when some point of it is
 * within the player's reach (a floor at most RD_PASS_REACH below it) and has
 * no collision of the game's own (floor/other or wall tiles) within
 * RD_PASS_BACKED in front of or behind it. Glass the player can't reach
 * (above an entrance) and glass with collision behind it stay solid.
 */
static bool rdRenderTriPassable(const f32 *a, const f32 *b, const f32 *c, const f32 *nrm)
{
	static const f32 weights[5][3] = {
		{ 0.334f, 0.333f, 0.333f },
		{ 0.7f, 0.15f, 0.15f },
		{ 0.15f, 0.7f, 0.15f },
		{ 0.15f, 0.15f, 0.7f },
		{ 0.45f, 0.45f, 0.1f },
	};
	s32 i, k;

	for (i = 0; i < ARRAYCOUNT(weights); i++) {
		f32 p[3], from[3], to[3], hit[3], hitn[3];

		for (k = 0; k < 3; k++) {
			p[k] = a[k] * weights[i][0] + b[k] * weights[i][1] + c[k] * weights[i][2];
		}

		// A floor within reach below?
		from[0] = p[0]; from[1] = p[1] + 5.0f; from[2] = p[2];
		to[0] = p[0]; to[1] = p[1] - RD_PASS_REACH; to[2] = p[2];

		if (!pdphysRaycastLevelGroups(from, to, 1u << RD_GROUP_FLOORS, hit, hitn) || hitn[1] < 0.7f) {
			continue;
		}

		// The game's own collision right at it?
		for (k = 0; k < 3; k++) {
			from[k] = p[k] + nrm[k] * RD_PASS_BACKED;
			to[k] = p[k] - nrm[k] * RD_PASS_BACKED;
		}

		if (pdphysRaycastLevelGroups(from, to, (1u << RD_GROUP_FLOORS) | (1u << RD_GROUP_WALLS), hit, hitn)) {
			continue;
		}

		return true;
	}

	return false;
}

static u8 *g_RdCeilingDone = NULL;
s32 g_RdCeilingRooms = 0;
s32 g_RdCeilingTris = 0;

static void ragdollAddRoomCeiling(struct meshbuilder *mb, s32 roomnum)
{
	struct vtxbatch *batch = g_Rooms[roomnum].vtxbatches;
	s32 numbatches = g_Rooms[roomnum].numvtxbatches;
	f32 slots[16][3];
	s32 numdown = 0;
	s32 numwall = 0;
	s32 numup = 0;
	s32 numpassable = 0;
	f32 sumdown = 0.0f;
	f32 sumup = 0.0f;
	s32 b;

	mbReset(mb);

	for (b = 0; b < numbatches; b++, batch++) {
		Gfx *iter;
		Vtx *vtx;
		s32 n;
		s32 k;

		if (batch->gdl == NULL) {
			continue;
		}

		// Translucent batches (glass, grilles) count too: glass the player
		// can't reach has no collision of its own, and bodies went through
		// it (e.g. above Defection's lobby entrance)

		// Same vertex fetch as bgTestHitInVtxBatch (the game's bullet tests)
		vtx = bgFindVerticesForGdl(roomnum, batch->gdl);

		if (vtx == NULL) {
			continue;
		}

		iter = &batch->gdl[batch->gbicmdindex];

		if (iter->dma.cmd != G_VTX) {
			continue;
		}

		vtx = (Vtx *)((UNSEGADDR(iter->words.w1) & 0xffffff) + (uintptr_t) vtx);
		n = (((u32) iter->bytes[GFX_W0_BYTE(1)] >> 4) & 0xf) + 1;

		for (k = 0; k < n && k < 16; k++) {
			slots[k][0] = g_BgRooms[roomnum].pos.x + vtx[k].x;
			slots[k][1] = g_BgRooms[roomnum].pos.y + vtx[k].y;
			slots[k][2] = g_BgRooms[roomnum].pos.z + vtx[k].z;
		}

		for (iter++; iter->dma.cmd != G_VTX && iter->dma.cmd != G_ENDDL; iter++) {
			s32 tris[4][3];
			s32 numtris = rdGdlTris(iter, tris);
			s32 t;


			for (t = 0; t < numtris; t++) {
				f32 *a, *bb, *c;
				f32 e1[3], e2[3], nrm[3];
				f32 len;

				if (tris[t][0] == 0 && tris[t][1] == 0 && tris[t][2] == 0) {
					continue;
				}

				if (tris[t][0] >= n || tris[t][1] >= n || tris[t][2] >= n) {
					continue;
				}

				a = slots[tris[t][0]];
				bb = slots[tris[t][1]];
				c = slots[tris[t][2]];
				rdVecSub(bb, a, e1);
				rdVecSub(c, a, e2);
				nrm[0] = e1[1] * e2[2] - e1[2] * e2[1];
				nrm[1] = e1[2] * e2[0] - e1[0] * e2[2];
				nrm[2] = e1[0] * e2[1] - e1[1] * e2[0];
				len = rdVecLen(nrm);

				if (len < 0.01f) {
					continue;
				}

				rdVecDiv(nrm, len);

				if (nrm[1] < RD_RENDER_MAXUP && batch->type != RD_VTXBATCHTYPE_OPA
						&& nrm[1] > -RD_CEIL_MINDOWN && rdRenderTriPassable(a, bb, c, nrm)) {
					numpassable++;
				} else if (nrm[1] < RD_RENDER_MAXUP) {
					s32 first = mb->numverts;

					mbAddVert(mb, a[0], a[1], a[2]);
					mbAddVert(mb, bb[0], bb[1], bb[2]);
					mbAddVert(mb, c[0], c[1], c[2]);
					mbAddTri(mb, first, first + 1, first + 2);

					if (nrm[1] <= -RD_CEIL_MINDOWN) {
						numdown++;
						sumdown += (a[1] + bb[1] + c[1]) / 3.0f;
					} else {
						numwall++;
					}
				} else {
					numup++;
					sumup += (a[1] + bb[1] + c[1]) / 3.0f;
				}
			}
		}
	}

	if (mb->numtris > 0) {
		pdphysLevelAddRoomGroup(roomnum, RD_GROUP_RENDER, mb->verts, mb->numverts, mb->indices, mb->numtris);
		g_RdCeilingTris += mb->numtris;
	}

	if (numpassable > 0) {
		sysLogPrintf(LOG_NOTE, "PHYSICS: room %d: %d translucent render tris the player can walk through left out (light beams)", roomnum, numpassable);
	}

	g_RdCeilingRooms++;

	if (g_RdCeilingRooms <= 3) {
		sysLogPrintf(LOG_NOTE, "PHYSICS: room %d render geometry: %d ceiling tris (avg y %.0f), %d wall tris added; %d up-facing skipped (avg y %.0f), room y %.0f..%.0f",
				roomnum, numdown, numdown ? sumdown / numdown : 0.0f, numwall, numup, numup ? sumup / numup : 0.0f,
				g_Rooms[roomnum].bbmin[1], g_Rooms[roomnum].bbmax[1]);
	}
}

/**
 * Invisible walls. The game puts collision walls where the player must not
 * go: tall walls above rooftop railings, window frames (even after the glass
 * is shot out), level edges. Bodies should fall past those. Once a room's
 * render geometry is loaded, each collision wall tile is checked against it:
 * points spread over the tile that have no rendered surface (opaque or glass)
 * within RD_INVIS_NEAR units are invisible; tiles that aren't almost entirely
 * drawn are cut down to the parts that are (rdKeepDrawnCells: a railing keeps
 * its height and no more, an invisible wall goes completely). The render walls added above still stop bodies at
 * everything that's drawn. Breakable windows count as openings: their glass
 * blocks bodies as an object until it's shot out.
 */
#define RD_INVIS_NEAR      40.0f // a rendered surface this close makes a point visible (railing bars, glass set back from the wall)
#define RD_INVIS_TIGHT     12.0f // ... this close along the wall (rdWallPointDrawn)
#define RD_INVIS_MAXOPENINGS 64
#define RD_INVIS_SOLID     0.97f // tiles with at least this much drawn on them are kept whole
#define RD_INVIS_CELL      64.0f
#define RD_INVIS_BUCKETS   4096
#define RD_INVIS_MAXCELLS  256   // triangles covering more cells than this go in the "big" list

struct rdtri {
	f32 v[3][3];
	f32 min[3];
	f32 max[3];
};

struct rdtrilist {
	struct rdtri *tris;
	s32 num;
	s32 max;
};

s32 g_RdInvisibleRooms = 0;
s32 g_RdInvisibleTiles = 0;
s32 g_RdInvisLowered = 0; // partly drawn walls cut down to what's drawn (railings)
static s16 *g_RdInvisDropped = NULL; // per room: walls dropped at the last check, -1 = not checked yet
static u32 *g_RdInvisSig = NULL;     // per room: signature of the walls built at the last check
static s32 g_RdRoomArraysCount = 0;   // entries in g_RdCeilingDone, g_RdInvisDropped and g_RdInvisSig

void rdFreeRoomArrays(void)
{
	free(g_RdCeilingDone);
	g_RdCeilingDone = NULL;
	free(g_RdInvisDropped);
	g_RdInvisDropped = NULL;
	free(g_RdInvisSig);
	g_RdInvisSig = NULL;
	g_RdRoomArraysCount = 0;
}

/**
 * The per-room arrays cover every room of the current stage. Grown when the
 * stage turns out to have more rooms than they were made for (ragdollStageLoad
 * runs before the game counts the new stage's rooms).
 */
bool rdEnsureRoomArrays(void)
{
	s32 want = g_Vars.roomcount;
	s32 old = g_RdRoomArraysCount;
	u8 *ceil;
	s16 *dropped;
	u32 *sig;
	s32 i;

	if (g_TileNumRooms > want) {
		want = g_TileNumRooms;
	}

	want++;

	if (want <= old && g_RdCeilingDone && g_RdInvisDropped && g_RdInvisSig) {
		return true;
	}

	ceil = realloc(g_RdCeilingDone, want * sizeof(u8));
	if (ceil) g_RdCeilingDone = ceil;
	dropped = realloc(g_RdInvisDropped, want * sizeof(s16));
	if (dropped) g_RdInvisDropped = dropped;
	sig = realloc(g_RdInvisSig, want * sizeof(u32));
	if (sig) g_RdInvisSig = sig;

	if (!ceil || !dropped || !sig) {
		rdFreeRoomArrays();
		return false;
	}

	for (i = old; i < want; i++) {
		g_RdCeilingDone[i] = 0;
		g_RdInvisDropped[i] = -1; // not checked
		g_RdInvisSig[i] = 0;
	}

	g_RdRoomArraysCount = want;

	return true;
}

/**
 * The 8 corners of an object's model bounding box in the world (at least 4
 * units thick on every axis, so flat glass makes a usable box).
 */
bool rdObjBboxCorners(struct defaultobj *obj, f32 (*out)[3])
{
	struct modelrodata_bbox *bbox;
	f32 lo[3], hi[3];
	f32 scale;
	s32 c, k;

	if (obj == NULL || obj->prop == NULL || obj->model == NULL) {
		return false;
	}

	bbox = objFindBboxRodata(obj);

	if (bbox == NULL) {
		return false;
	}

	scale = 1.0f; // realrot carries the object's scale (as objGetRotatedLocal*ByMtx3)
	lo[0] = bbox->xmin * scale; hi[0] = bbox->xmax * scale;
	lo[1] = bbox->ymin * scale; hi[1] = bbox->ymax * scale;
	lo[2] = bbox->zmin * scale; hi[2] = bbox->zmax * scale;

	for (k = 0; k < 3; k++) {
		if (hi[k] - lo[k] < 4.0f) {
			f32 mid = (hi[k] + lo[k]) * 0.5f;
			lo[k] = mid - 2.0f;
			hi[k] = mid + 2.0f;
		}
	}

	for (c = 0; c < 8; c++) {
		f32 l[3];

		l[0] = (c & 1) ? hi[0] : lo[0];
		l[1] = (c & 2) ? hi[1] : lo[1];
		l[2] = (c & 4) ? hi[2] : lo[2];

		// world = pos + realrot * local (as objGetRotatedLocal*ByMtx3)
		for (k = 0; k < 3; k++) {
			out[c][k] = obj->prop->pos.f[k] + obj->realrot[0][k] * l[0] + obj->realrot[1][k] * l[1] + obj->realrot[2][k] * l[2];
		}
	}

	return true;
}

static void rdTriListAdd(struct rdtrilist *list, const f32 *a, const f32 *b, const f32 *c)
{
	struct rdtri *t;
	s32 k;

	if (list->num >= list->max) {
		s32 newmax = list->max ? list->max * 2 : 1024;
		struct rdtri *newtris = realloc(list->tris, newmax * sizeof(struct rdtri));

		if (newtris == NULL) {
			return;
		}

		list->tris = newtris;
		list->max = newmax;
	}

	t = &list->tris[list->num++];

	for (k = 0; k < 3; k++) {
		t->v[0][k] = a[k];
		t->v[1][k] = b[k];
		t->v[2][k] = c[k];
		t->min[k] = fminf(a[k], fminf(b[k], c[k])) - RD_INVIS_NEAR;
		t->max[k] = fmaxf(a[k], fmaxf(b[k], c[k])) + RD_INVIS_NEAR;
	}
}

/**
 * Every rendered triangle of a loaded room (opaque and translucent, any
 * direction), parsed like ragdollAddRoomCeiling.
 */
static void rdGatherRenderTris(s32 roomnum, struct rdtrilist *list)
{
	struct vtxbatch *batch;
	f32 slots[16][3];
	s32 b;

	if (roomnum <= 0 || roomnum >= g_Vars.roomcount || !g_Rooms[roomnum].loaded240
			|| !g_Rooms[roomnum].gfxdata || !g_Rooms[roomnum].vtxbatches) {
		return;
	}

	batch = g_Rooms[roomnum].vtxbatches;

	for (b = 0; b < g_Rooms[roomnum].numvtxbatches; b++, batch++) {
		Gfx *iter;
		Vtx *vtx;
		s32 n, k;

		if (batch->gdl == NULL) {
			continue;
		}

		vtx = bgFindVerticesForGdl(roomnum, batch->gdl);

		if (vtx == NULL) {
			continue;
		}

		iter = &batch->gdl[batch->gbicmdindex];

		if (iter->dma.cmd != G_VTX) {
			continue;
		}

		vtx = (Vtx *)((UNSEGADDR(iter->words.w1) & 0xffffff) + (uintptr_t) vtx);
		n = (((u32) iter->bytes[GFX_W0_BYTE(1)] >> 4) & 0xf) + 1;

		for (k = 0; k < n && k < 16; k++) {
			slots[k][0] = g_BgRooms[roomnum].pos.x + vtx[k].x;
			slots[k][1] = g_BgRooms[roomnum].pos.y + vtx[k].y;
			slots[k][2] = g_BgRooms[roomnum].pos.z + vtx[k].z;
		}

		for (iter++; iter->dma.cmd != G_VTX && iter->dma.cmd != G_ENDDL; iter++) {
			s32 tris[4][3];
			s32 numtris = rdGdlTris(iter, tris);
			s32 t;


			for (t = 0; t < numtris; t++) {
				if (tris[t][0] == 0 && tris[t][1] == 0 && tris[t][2] == 0) {
					continue;
				}

				if (tris[t][0] >= n || tris[t][1] >= n || tris[t][2] >= n) {
					continue;
				}

				rdTriListAdd(list, slots[tris[t][0]], slots[tris[t][1]], slots[tris[t][2]]);
			}
		}
	}
}

/**
 * A room's render triangles, parsed once while it's loaded and kept for the
 * rest of the stage: when a room is checked again later, the rooms around it
 * may have been unloaded (out of view), and their surfaces still count.
 */
static struct rdtrilist *g_RdRoomTris = NULL; // per room; tris == NULL and max == 0: not parsed yet
static s32 g_RdRoomTrisCount = 0;

void rdFreeRoomTris(void)
{
	s32 i;

	if (g_RdRoomTris) {
		for (i = 0; i < g_RdRoomTrisCount; i++) {
			free(g_RdRoomTris[i].tris);
		}

		free(g_RdRoomTris);
	}

	g_RdRoomTris = NULL;
	g_RdRoomTrisCount = 0;
}

static void rdAddRoomTris(s32 roomnum, struct rdtrilist *list)
{
	struct rdtrilist *cached;
	s32 i;

	if (g_RdRoomTris == NULL) {
		g_RdRoomTrisCount = g_Vars.roomcount + 1;
		g_RdRoomTris = calloc(g_RdRoomTrisCount, sizeof(struct rdtrilist));
	}

	if (g_RdRoomTris == NULL || roomnum < 0 || roomnum >= g_RdRoomTrisCount) {
		rdGatherRenderTris(roomnum, list);
		return;
	}

	cached = &g_RdRoomTris[roomnum];

	if (cached->max == 0) {
		if (!g_Rooms[roomnum].loaded240 || !g_Rooms[roomnum].gfxdata || !g_Rooms[roomnum].vtxbatches) {
			return; // not seen yet
		}

		rdGatherRenderTris(roomnum, cached);

		if (cached->max == 0) {
			cached->max = -1; // parsed, nothing drawn
		}
	}

	for (i = 0; i < cached->num; i++) {
		if (list->num >= list->max) {
			s32 newmax = list->max ? list->max * 2 : 1024;
			struct rdtri *newtris = realloc(list->tris, newmax * sizeof(struct rdtri));

			if (newtris == NULL) {
				return;
			}

			list->tris = newtris;
			list->max = newmax;
		}

		list->tris[list->num++] = cached->tris[i];
	}
}

// Squared distance from p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5)
static f32 rdPointTriDistSq(const f32 *p, const f32 *a, const f32 *b, const f32 *c)
{
	f32 ab[3], ac[3], ap[3], bp[3], cp[3], q[3], d;
	f32 d1, d2, d3, d4, d5, d6, va, vb, vc, v, w, denom;
	s32 k;

	for (k = 0; k < 3; k++) {
		ab[k] = b[k] - a[k];
		ac[k] = c[k] - a[k];
		ap[k] = p[k] - a[k];
	}

	d1 = rdVecDot(ab, ap);
	d2 = rdVecDot(ac, ap);

	if (d1 <= 0.0f && d2 <= 0.0f) {
		for (k = 0; k < 3; k++) q[k] = a[k];
		goto done;
	}

	for (k = 0; k < 3; k++) bp[k] = p[k] - b[k];

	d3 = rdVecDot(ab, bp);
	d4 = rdVecDot(ac, bp);

	if (d3 >= 0.0f && d4 <= d3) {
		for (k = 0; k < 3; k++) q[k] = b[k];
		goto done;
	}

	vc = d1 * d4 - d3 * d2;

	if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
		v = d1 / (d1 - d3);
		for (k = 0; k < 3; k++) q[k] = a[k] + v * ab[k];
		goto done;
	}

	for (k = 0; k < 3; k++) cp[k] = p[k] - c[k];

	d5 = rdVecDot(ab, cp);
	d6 = rdVecDot(ac, cp);

	if (d6 >= 0.0f && d5 <= d6) {
		for (k = 0; k < 3; k++) q[k] = c[k];
		goto done;
	}

	vb = d5 * d2 - d1 * d6;

	if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
		w = d2 / (d2 - d6);
		for (k = 0; k < 3; k++) q[k] = a[k] + w * ac[k];
		goto done;
	}

	va = d3 * d6 - d5 * d4;

	if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
		w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		for (k = 0; k < 3; k++) q[k] = b[k] + w * (c[k] - b[k]);
		goto done;
	}

	denom = 1.0f / (va + vb + vc);
	v = vb * denom;
	w = vc * denom;

	for (k = 0; k < 3; k++) q[k] = a[k] + ab[k] * v + ac[k] * w;

done:
	d = 0.0f;

	for (k = 0; k < 3; k++) {
		d += (p[k] - q[k]) * (p[k] - q[k]);
	}

	return d;
}

/**
 * Spatial hash of the render triangles, so each sample point only looks at
 * the triangles around it.
 */
struct rdtrigrid {
	s32 *start;   // RD_INVIS_BUCKETS + 1
	s32 *items;
	s32 *big;
	s32 numbig;
};

static u32 rdInvisCellHash(s32 x, s32 y, s32 z)
{
	return ((u32) x * 73856093u ^ (u32) y * 19349663u ^ (u32) z * 83492791u) & (RD_INVIS_BUCKETS - 1);
}

static bool rdTriCells(const struct rdtri *t, s32 *lo, s32 *hi)
{
	s32 k;
	s32 cells = 1;

	for (k = 0; k < 3; k++) {
		lo[k] = (s32) floorf(t->min[k] / RD_INVIS_CELL);
		hi[k] = (s32) floorf(t->max[k] / RD_INVIS_CELL);
		cells *= hi[k] - lo[k] + 1;
	}

	return cells <= RD_INVIS_MAXCELLS;
}

static bool rdTriGridBuild(struct rdtrigrid *grid, const struct rdtrilist *list)
{
	s32 *counts = calloc(RD_INVIS_BUCKETS + 1, sizeof(s32));
	s32 total = 0;
	s32 pass, i, x, y, z;
	s32 lo[3], hi[3];

	memset(grid, 0, sizeof(*grid));

	if (counts == NULL) {
		return false;
	}

	grid->start = calloc(RD_INVIS_BUCKETS + 1, sizeof(s32));
	grid->big = malloc((list->num + 1) * sizeof(s32));

	if (grid->start == NULL || grid->big == NULL) {
		free(counts);
		return false;
	}

	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < list->num; i++) {
			if (!rdTriCells(&list->tris[i], lo, hi)) {
				if (pass == 0) {
					grid->big[grid->numbig++] = i;
				}

				continue;
			}

			for (x = lo[0]; x <= hi[0]; x++) {
				for (y = lo[1]; y <= hi[1]; y++) {
					for (z = lo[2]; z <= hi[2]; z++) {
						u32 h = rdInvisCellHash(x, y, z);

						if (pass == 0) {
							counts[h]++;
							total++;
						} else {
							grid->items[grid->start[h] + counts[h]++] = i;
						}
					}
				}
			}
		}

		if (pass == 0) {
			for (i = 0; i < RD_INVIS_BUCKETS; i++) {
				grid->start[i + 1] = grid->start[i] + counts[i];
				counts[i] = 0;
			}

			grid->items = malloc((total + 1) * sizeof(s32));

			if (grid->items == NULL) {
				free(counts);
				return false;
			}
		}
	}

	free(counts);
	return true;
}

static void rdTriGridFree(struct rdtrigrid *grid)
{
	free(grid->start);
	free(grid->items);
	free(grid->big);
	memset(grid, 0, sizeof(*grid));
}

// Breakable windows near the room being checked (world boxes): points in
// them are openings, whatever is drawn around them
static f32 g_RdInvisOpenings[RD_INVIS_MAXOPENINGS][6];
static s32 g_RdInvisNumOpenings = 0;

static bool rdPointNearRender(const struct rdtrigrid *grid, const struct rdtrilist *list, const f32 *p, f32 dist)
{
	const f32 maxsq = dist * dist;
	u32 h = rdInvisCellHash((s32) floorf(p[0] / RD_INVIS_CELL), (s32) floorf(p[1] / RD_INVIS_CELL), (s32) floorf(p[2] / RD_INVIS_CELL));
	s32 i;

	for (i = 0; i < g_RdInvisNumOpenings; i++) {
		const f32 *o = g_RdInvisOpenings[i];

		if (p[0] >= o[0] && p[0] <= o[3] && p[1] >= o[1] && p[1] <= o[4] && p[2] >= o[2] && p[2] <= o[5]) {
			return false;
		}
	}

	for (i = grid->start[h]; i < grid->start[h + 1]; i++) {
		const struct rdtri *t = &list->tris[grid->items[i]];

		if (p[0] >= t->min[0] && p[0] <= t->max[0] && p[1] >= t->min[1] && p[1] <= t->max[1]
				&& p[2] >= t->min[2] && p[2] <= t->max[2]
				&& rdPointTriDistSq(p, t->v[0], t->v[1], t->v[2]) <= maxsq) {
			return true;
		}
	}

	for (i = 0; i < grid->numbig; i++) {
		const struct rdtri *t = &list->tris[grid->big[i]];

		if (p[0] >= t->min[0] && p[0] <= t->max[0] && p[1] >= t->min[1] && p[1] <= t->max[1]
				&& p[2] >= t->min[2] && p[2] <= t->max[2]
				&& rdPointTriDistSq(p, t->v[0], t->v[1], t->v[2]) <= maxsq) {
			return true;
		}
	}

	return false;
}

/**
 * A rendered surface near point p of a wall with normal nrm: within
 * RD_INVIS_TIGHT units along the wall, but up to RD_INVIS_DEPTH units in
 * front of or behind it (glass set back in a frame). Measured like that, a
 * railing's top rail doesn't make the wall 40 units above it "drawn" too.
 * RD_INVIS_DEPTH must stay below RD_INVIS_NEAR (the margin triangles are
 * filed in the spatial hash with).
 */
#define RD_INVIS_DEPTH 36.0f

static bool rdWallPointDrawn(const struct rdtrilist *list, const s32 *cand, s32 numcand, const f32 *p, const f32 *nrm)
{
	// Distances along the normal count a third (squashing space along the
	// normal around p), so RD_INVIS_TIGHT reaches 3 * RD_INVIS_TIGHT = 36
	// units in front and behind
	const f32 squash = RD_INVIS_TIGHT / RD_INVIS_DEPTH - 1.0f;
	const f32 maxsq = RD_INVIS_TIGHT * RD_INVIS_TIGHT;
	s32 i, j, k;

	for (i = 0; i < g_RdInvisNumOpenings; i++) {
		const f32 *o = g_RdInvisOpenings[i];

		if (p[0] >= o[0] && p[0] <= o[3] && p[1] >= o[1] && p[1] <= o[4] && p[2] >= o[2] && p[2] <= o[5]) {
			return false;
		}
	}

	for (i = 0; i < numcand; i++) {
		const struct rdtri *t = &list->tris[cand[i]];
		f32 q[3][3];

		if (p[0] < t->min[0] || p[0] > t->max[0] || p[1] < t->min[1] || p[1] > t->max[1]
				|| p[2] < t->min[2] || p[2] > t->max[2]) {
			continue;
		}

		for (j = 0; j < 3; j++) {
			f32 d = (t->v[j][0] - p[0]) * nrm[0] + (t->v[j][1] - p[1]) * nrm[1] + (t->v[j][2] - p[2]) * nrm[2];

			for (k = 0; k < 3; k++) {
				q[j][k] = t->v[j][k] + nrm[k] * d * squash;
			}
		}

		if (rdPointTriDistSq(p, q[0], q[1], q[2]) <= maxsq) {
			// Most points near one triangle are near it again: look at it first next time
			if (i > 0) {
				s32 *c = (s32 *) cand;
				s32 tmp = c[0];
				c[0] = c[i];
				c[i] = tmp;
			}

			return true;
		}
	}

	return false;
}

// Sutherland-Hodgman in 2D: the part of polygon 'in' inside convex polygon 'clip'
static s32 rdClipPoly2D(f32 (*in)[2], s32 n, f32 (*clip)[2], s32 m, f32 sign, f32 (*out)[2])
{
	f32 buf[2][64][2];
	f32 (*src)[2] = in;
	s32 num = n;
	s32 e, i, cur = 0;

	for (e = 0; e < m && num > 0; e++) {
		const f32 *a = clip[e];
		const f32 *b = clip[(e + 1) % m];
		f32 (*dst)[2] = buf[cur];
		s32 out_n = 0;

		for (i = 0; i < num && out_n < 62; i++) {
			const f32 *p = src[i];
			const f32 *q = src[(i + 1) % num];
			f32 dp = sign * ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]));
			f32 dq = sign * ((b[0] - a[0]) * (q[1] - a[1]) - (b[1] - a[1]) * (q[0] - a[0]));

			if (dp >= 0.0f) {
				dst[out_n][0] = p[0];
				dst[out_n][1] = p[1];
				out_n++;
			}

			if ((dp >= 0.0f) != (dq >= 0.0f)) {
				f32 t = dp / (dp - dq);

				dst[out_n][0] = p[0] + (q[0] - p[0]) * t;
				dst[out_n][1] = p[1] + (q[1] - p[1]) * t;
				out_n++;
			}
		}

		src = dst;
		num = out_n;
		cur ^= 1;
	}

	for (i = 0; i < num; i++) {
		out[i][0] = src[i][0];
		out[i][1] = src[i][1];
	}

	return num;
}

static bool rdPointInPoly2D(f32 s, f32 t, f32 (*poly)[2], s32 n)
{
	bool inside = false;
	s32 i, j;

	for (i = 0, j = n - 1; i < n; j = i++) {
		if ((poly[i][1] > t) != (poly[j][1] > t)
				&& s < (poly[j][0] - poly[i][0]) * (t - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0]) {
			inside = !inside;
		}
	}

	return inside;
}

/**
 * Railings and partly drawn walls: keeps only the parts of a collision wall
 * that have something drawn on them. The wall is cut into cells
 * (RD_INVIS_CELLW wide, RD_INVIS_CELLH tall); a cell stays when at least half
 * of its sample points are drawn (rdWallPointDrawn). Cells left and right of
 * a kept cell stay too, so corners where the drawing and the collision don't
 * quite line up have no gaps. Kept cells are merged into rectangles (by row,
 * then rows with the same span) and cut to the tile's outline.
 * Returns the number of rectangles added.
 */
#define RD_INVIS_CELLW 30.0f
#define RD_INVIS_CELLH 10.0f
#define RD_INVIS_MAXCOLS 64
#define RD_INVIS_MAXROWS 64

static s32 rdKeepDrawnCells(const struct rdtrilist *list, f32 (*v)[3], s32 n, struct meshbuilder *mb)
{
	static s32 *cand = NULL;
	static s32 maxcand = 0;
	s32 numcand = 0;
	f32 lo[3], hi[3];
	static u8 cell[RD_INVIS_MAXROWS][RD_INVIS_MAXCOLS];
	static u8 kept[RD_INVIS_MAXROWS][RD_INVIS_MAXCOLS];
	f32 poly[64][2];
	f32 nrm[3] = { 0.0f, 0.0f, 0.0f };
	f32 u[3], w[3], len, area = 0.0f;
	f32 smin = 1e9f, smax = -1e9f, tmin = 1e9f, tmax = -1e9f;
	f32 cw, ch;
	s32 ncols, nrows, r, c, i, k, numrects = 0;
	s32 open0[RD_INVIS_MAXCOLS], open1[RD_INVIS_MAXCOLS], openrow[RD_INVIS_MAXCOLS];
	s32 numopen = 0;

	if (n < 3 || n > 64) {
		return 0;
	}

	// Newell normal
	for (i = 0; i < n; i++) {
		f32 *a = v[i];
		f32 *b = v[(i + 1) % n];

		nrm[0] += (a[1] - b[1]) * (a[2] + b[2]);
		nrm[1] += (a[2] - b[2]) * (a[0] + b[0]);
		nrm[2] += (a[0] - b[0]) * (a[1] + b[1]);
	}

	len = rdVecLen(nrm);

	if (len < 1e-3f) {
		return 0;
	}

	for (k = 0; k < 3; k++) {
		nrm[k] /= len;
	}

	// u: along the wall (horizontal), w: up the wall
	u[0] = nrm[2];
	u[1] = 0.0f;
	u[2] = -nrm[0];
	len = rdVecLen(u);

	if (len < 0.2f) {
		return 0; // not a wall
	}

	for (k = 0; k < 3; k++) {
		u[k] /= len;
	}

	w[0] = nrm[1] * u[2] - nrm[2] * u[1];
	w[1] = nrm[2] * u[0] - nrm[0] * u[2];
	w[2] = nrm[0] * u[1] - nrm[1] * u[0];

	if (w[1] < 0.0f) {
		for (k = 0; k < 3; k++) {
			w[k] = -w[k];
		}
	}

	for (i = 0; i < n; i++) {
		f32 d[3];

		rdVecSub(v[i], v[0], d);
		poly[i][0] = rdVecDot(d, u);
		poly[i][1] = rdVecDot(d, w);
		smin = fminf(smin, poly[i][0]);
		smax = fmaxf(smax, poly[i][0]);
		tmin = fminf(tmin, poly[i][1]);
		tmax = fmaxf(tmax, poly[i][1]);
	}

	for (i = 0; i < n; i++) {
		area += poly[i][0] * poly[(i + 1) % n][1] - poly[(i + 1) % n][0] * poly[i][1];
	}

	if (fabsf(area) < 1.0f) {
		return 0;
	}

	// The triangles that can be near this tile at all
	for (k = 0; k < 3; k++) {
		lo[k] = 1e9f;
		hi[k] = -1e9f;
	}

	for (i = 0; i < n; i++) {
		for (k = 0; k < 3; k++) {
			lo[k] = fminf(lo[k], v[i][k]);
			hi[k] = fmaxf(hi[k], v[i][k]);
		}
	}

	if (maxcand < list->num) {
		s32 *newcand = realloc(cand, list->num * sizeof(s32));

		if (newcand == NULL) {
			return 0;
		}

		cand = newcand;
		maxcand = list->num;
	}

	for (i = 0; i < list->num; i++) {
		const struct rdtri *t = &list->tris[i];

		if (t->max[0] >= lo[0] && t->min[0] <= hi[0] && t->max[1] >= lo[1] && t->min[1] <= hi[1]
				&& t->max[2] >= lo[2] && t->min[2] <= hi[2]) {
			cand[numcand++] = i;
		}
	}

	ncols = (s32) ceilf((smax - smin) / RD_INVIS_CELLW);
	nrows = (s32) ceilf((tmax - tmin) / RD_INVIS_CELLH);
	ncols = RD_CLAMP(ncols, 1, RD_INVIS_MAXCOLS);
	nrows = RD_CLAMP(nrows, 1, RD_INVIS_MAXROWS);
	cw = (smax - smin) / ncols;
	ch = (tmax - tmin) / nrows;

	for (r = 0; r < nrows; r++) {
		for (c = 0; c < ncols; c++) {
			s32 inside = 0, drawn = 0, sx, sy;

			for (sy = 0; sy < 2; sy++) {
				for (sx = 0; sx < 2; sx++) {
					f32 s = smin + (c + 0.25f + sx * 0.5f) * cw;
					f32 t = tmin + (r + 0.25f + sy * 0.5f) * ch;
					f32 p[3];

					if (!rdPointInPoly2D(s, t, poly, n)) {
						continue;
					}

					inside++;

					for (k = 0; k < 3; k++) {
						p[k] = v[0][k] + u[k] * s + w[k] * t;
					}

					if (rdWallPointDrawn(list, cand, numcand, p, nrm)) {
						drawn++;
					}
				}
			}

			cell[r][c] = inside > 0 && drawn * 2 >= inside;
		}
	}

	for (r = 0; r < nrows; r++) {
		for (c = 0; c < ncols; c++) {
			kept[r][c] = cell[r][c] || (c > 0 && cell[r][c - 1]) || (c + 1 < ncols && cell[r][c + 1]);
		}
	}

	// Merge into rectangles: runs along each row, extended up while the row
	// above has the same run
	for (r = 0; r <= nrows; r++) {
		s32 runs0[RD_INVIS_MAXCOLS], runs1[RD_INVIS_MAXCOLS];
		s32 numruns = 0;
		s32 newopen = 0;
		s32 n0[RD_INVIS_MAXCOLS], n1[RD_INVIS_MAXCOLS], nrow[RD_INVIS_MAXCOLS];

		if (r < nrows) {
			for (c = 0; c < ncols; c++) {
				if (kept[r][c] && (c == 0 || !kept[r][c - 1])) {
					runs0[numruns] = c;
				}

				if (kept[r][c] && (c + 1 == ncols || !kept[r][c + 1])) {
					runs1[numruns++] = c;
				}
			}
		}

		// Close open rectangles that don't continue into this row
		for (i = 0; i < numopen; i++) {
			bool cont = false;

			for (k = 0; k < numruns; k++) {
				if (runs0[k] == open0[i] && runs1[k] == open1[i]) {
					cont = true;
					break;
				}
			}

			if (cont) {
				n0[newopen] = open0[i];
				n1[newopen] = open1[i];
				nrow[newopen++] = openrow[i];
			} else {
				f32 rect[4][2];
				f32 clipped[64][2];
				s32 cn;

				rect[0][0] = smin + open0[i] * cw;       rect[0][1] = tmin + openrow[i] * ch;
				rect[1][0] = smin + (open1[i] + 1) * cw; rect[1][1] = tmin + openrow[i] * ch;
				rect[2][0] = smin + (open1[i] + 1) * cw; rect[2][1] = tmin + r * ch;
				rect[3][0] = smin + open0[i] * cw;       rect[3][1] = tmin + r * ch;

				cn = rdClipPoly2D(rect, 4, poly, n, area > 0.0f ? 1.0f : -1.0f, clipped);

				if (cn >= 3) {
					s32 first = mb->numverts;
					s32 j;

					for (j = 0; j < cn; j++) {
						mbAddVert(mb,
								v[0][0] + u[0] * clipped[j][0] + w[0] * clipped[j][1],
								v[0][1] + u[1] * clipped[j][0] + w[1] * clipped[j][1],
								v[0][2] + u[2] * clipped[j][0] + w[2] * clipped[j][1]);
					}

					mbAddFan(mb, first, cn);
					numrects++;
				}
			}
		}

		// Open rectangles for new runs
		for (k = 0; k < numruns; k++) {
			bool exists = false;

			for (i = 0; i < newopen; i++) {
				if (n0[i] == runs0[k] && n1[i] == runs1[k]) {
					exists = true;
					break;
				}
			}

			if (!exists && newopen < RD_INVIS_MAXCOLS) {
				n0[newopen] = runs0[k];
				n1[newopen] = runs1[k];
				nrow[newopen++] = r;
			}
		}

		numopen = newopen;

		for (i = 0; i < numopen; i++) {
			open0[i] = n0[i];
			open1[i] = n1[i];
			openrow[i] = nrow[i];
		}
	}

	return numrects;
}

/**
 * Fraction (by area) of a collision polygon that has a rendered surface on it.
 */
static f32 rdPolyVisibleFrac(const struct rdtrigrid *grid, const struct rdtrilist *list, f32 (*v)[3], s32 n)
{
	const s32 sub = 5; // 21 points per fan triangle
	f32 total = 0.0f;
	f32 visible = 0.0f;
	s32 i, k, sa, sb;

	for (i = 1; i + 1 < n; i++) {
		f32 *a = v[0], *b = v[i], *c = v[i + 1];
		f32 e1[3], e2[3], cr[3], area;

		rdVecSub(b, a, e1);
		rdVecSub(c, a, e2);
		cr[0] = e1[1] * e2[2] - e1[2] * e2[1];
		cr[1] = e1[2] * e2[0] - e1[0] * e2[2];
		cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
		area = rdVecLen(cr) * 0.5f;

		if (area < 1.0f) {
			continue;
		}

		for (sa = 0; sa <= sub; sa++) {
			for (sb = 0; sa + sb <= sub; sb++) {
				f32 wa = (sa + 0.33f) / (sub + 1.0f);
				f32 wb = (sb + 0.33f) / (sub + 1.0f);
				f32 wc = 1.0f - wa - wb;
				f32 p[3];

				if (wc < 0.0f) {
					continue;
				}

				for (k = 0; k < 3; k++) {
					p[k] = a[k] * wa + b[k] * wb + c[k] * wc;
				}

				total += area;

				if (rdPointNearRender(grid, list, p, RD_INVIS_NEAR)) {
					visible += area;
				}
			}
		}
	}

	return total > 0.0f ? visible / total : 1.0f;
}

static bool rdRoomsOverlap(s32 a, s32 b, f32 margin)
{
	s32 k;

	for (k = 0; k < 3; k++) {
		if (g_Rooms[a].bbmin[k] - margin > g_Rooms[b].bbmax[k] || g_Rooms[b].bbmin[k] - margin > g_Rooms[a].bbmax[k]) {
			return false;
		}
	}

	return true;
}

static void ragdollDropInvisibleWalls(s32 roomnum)
{
	static struct rdtrilist list;
	static struct meshbuilder mb;
	struct rdtrigrid grid;
	u8 *start, *end;
	struct geo *geo;
	s32 numwalls = 0;
	s32 dropped = 0;
	s32 i;
	u32 sig;
	u64 starttime = sysGetMicroseconds();

	if (roomnum < 0 || roomnum >= g_TileNumRooms || roomnum >= g_Vars.roomcount || g_RdInvisDropped == NULL
			|| !rdEnsureRoomArrays() || roomnum >= g_RdRoomArraysCount) {
		return;
	}

	// What's drawn on a room's walls can belong to any room around it
	// (the next room, a decoration room overlapping it), so every loaded
	// room whose bounds touch this one counts
	list.num = 0;
	g_RdInvisNumOpenings = 0;

	for (i = 1; i < g_Vars.roomcount; i++) {
		if (i == roomnum || rdRoomsOverlap(roomnum, i, 50.0f)) {
			rdAddRoomTris(i, &list);
		}
	}

	// Glass that can't be broken (or has no collision of its own) is an
	// object, not room geometry, and the game relies on the room's wall
	// behind it: it counts as drawn, so that wall stays. Breakable glass
	// doesn't, so a shot-out window lets bodies through.
	{
		static const s8 faces[6][4] = {
			{ 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 },
		};
		struct prop *pp;
		s32 lst;

		for (lst = 0; lst < 2; lst++) {
			for (pp = lst ? g_Vars.pausedprops : g_Vars.activeprops; pp && (lst || pp != g_Vars.pausedprops); pp = pp->next) {
				struct defaultobj *obj = pp->obj;
				u8 *gs = NULL, *ge = NULL;
				f32 corner[8][3];
				s32 f;

				if (pp->type != PROPTYPE_OBJ || obj == NULL
						|| (obj->type != OBJTYPE_GLASS && obj->type != OBJTYPE_TINTEDGLASS)) {
					continue;
				}

				if (pp->pos.x < g_Rooms[roomnum].bbmin[0] - 300.0f || pp->pos.x > g_Rooms[roomnum].bbmax[0] + 300.0f
						|| pp->pos.y < g_Rooms[roomnum].bbmin[1] - 300.0f || pp->pos.y > g_Rooms[roomnum].bbmax[1] + 300.0f
						|| pp->pos.z < g_Rooms[roomnum].bbmin[2] - 300.0f || pp->pos.z > g_Rooms[roomnum].bbmax[2] + 300.0f) {
					continue;
				}

				if (!rdObjBboxCorners(obj, corner)) {
					continue;
				}

				if ((obj->flags & OBJFLAG_INVINCIBLE) == 0 && objUpdateGeometry(pp, &gs, &ge) && gs && ge) {
					// Breakable and solid by itself: its pane is an opening
					// for the room's wall (the glass blocks until it's shot)
					if (g_RdInvisNumOpenings < RD_INVIS_MAXOPENINGS) {
						f32 *o = g_RdInvisOpenings[g_RdInvisNumOpenings++];
						s32 c, k;

						for (k = 0; k < 3; k++) {
							o[k] = 1e9f;
							o[k + 3] = -1e9f;
						}

						for (c = 0; c < 8; c++) {
							for (k = 0; k < 3; k++) {
								o[k] = fminf(o[k], corner[c][k] - RD_INVIS_TIGHT);
								o[k + 3] = fmaxf(o[k + 3], corner[c][k] + RD_INVIS_TIGHT);
							}
						}
					}

					continue;
				}

				for (f = 0; f < 6; f++) {
					rdTriListAdd(&list, corner[faces[f][0]], corner[faces[f][1]], corner[faces[f][2]]);
					rdTriListAdd(&list, corner[faces[f][0]], corner[faces[f][2]], corner[faces[f][3]]);
				}
			}
		}
	}

	if (list.num == 0 || !rdTriGridBuild(&grid, &list)) {
		return;
	}

	mbReset(&mb);
	start = g_TileFileData.u8 + g_TileRooms[roomnum];
	end = g_TileFileData.u8 + g_TileRooms[roomnum + 1];
	geo = (struct geo *) start;

	for (; geo && (u8 *) geo < end; geo = rdGeoNext(geo)) {
		f32 v[64][3];
		s32 n = 0;
		bool wall = (geo->flags & GEOFLAG_WALL) != 0;

		if (geo->type == GEOTYPE_TILE_I) {
			struct geotilei *tile = (struct geotilei *) geo;

			for (n = 0; n < geo->numvertices && n < 64; n++) {
				v[n][0] = tile->vertices[n][0];
				v[n][1] = tile->vertices[n][1];
				v[n][2] = tile->vertices[n][2];
			}
		} else if (geo->type == GEOTYPE_TILE_F) {
			struct geotilef *tile = (struct geotilef *) geo;

			for (n = 0; n < geo->numvertices && n < 64; n++) {
				rdCoordToVec(&tile->vertices[n], v[n]);
			}
		} else {
			continue; // (blocks, cylinders; an unknown type ends the walk)
		}

		if (!wall || n < 3) {
			continue;
		}

		numwalls++;

		if (rdPolyVisibleFrac(&grid, &list, v, n) < RD_INVIS_SOLID) {
			// Partly drawn (a railing, a wall that's open above, a window):
			// keep only the drawn parts
			dropped++;

			if (rdKeepDrawnCells(&list, v, n, &mb) > 0) {
				g_RdInvisLowered++;
			}
		} else {
			s32 first = mb.numverts;

			for (i = 0; i < n; i++) {
				mbAddVert(&mb, v[i][0], v[i][1], v[i][2]);
			}

			mbAddFan(&mb, first, n);
		}
	}

	rdTriGridFree(&grid);

	// Rebuilt whenever the answer changed (a room loaded later can show
	// that more of a wall is drawn)
	sig = (u32) mb.numtris * 2654435761u;

	for (i = 0; i < mb.numverts * 3; i++) {
		sig = (sig ^ (u32) (s32) mb.verts[i]) * 16777619u;
	}

	if (dropped != RD_MAX(g_RdInvisDropped[roomnum], 0)
			|| (g_RdInvisSig && dropped > 0 && sig != g_RdInvisSig[roomnum])) {
		pdphysLevelRemoveRoomGroup(roomnum, RD_GROUP_WALLS);

		if (mb.numtris > 0) {
			pdphysLevelAddRoomGroup(roomnum, RD_GROUP_WALLS, mb.verts, mb.numverts, mb.indices, mb.numtris);
		}

		g_RdInvisibleTiles += dropped - RD_MAX(g_RdInvisDropped[roomnum], 0);

		if (g_RdInvisDropped[roomnum] <= 0) {
			g_RdInvisibleRooms++;
		}

		sysLogPrintf(LOG_NOTE, "PHYSICS: room %d: %d of %d collision walls not fully drawn (invisible walls, railings; %d render tris, %.1f ms); stage total %d in %d rooms, %d cut down to what's drawn",
				roomnum, dropped, numwalls, list.num, (sysGetMicroseconds() - starttime) / 1000.0f, g_RdInvisibleTiles, g_RdInvisibleRooms, g_RdInvisLowered);
	}

	g_RdInvisDropped[roomnum] = dropped;

	if (g_RdInvisSig) {
		g_RdInvisSig[roomnum] = sig;
	}
}

void ragdollUpdateCeilings(void)
{
	static struct meshbuilder mb;
	s32 done = 0;
	s32 r;

	if (g_RdCeilingDone == NULL || !rdEnsureRoomArrays()) {
		return;
	}

	for (r = 1; r < g_Vars.roomcount && done < RD_CEIL_ROOMSPERTICK; r++) {
		if (!g_RdCeilingDone[r] && g_Rooms[r].loaded240 && g_Rooms[r].gfxdata && g_Rooms[r].vtxbatches) {
			g_RdCeilingDone[r] = 1;
			ragdollAddRoomCeiling(&mb, r);
			ragdollDropInvisibleWalls(r);

			// Rooms around it that dropped walls get checked again with
			// this room's surfaces included
			if (g_RdInvisDropped) {
				s32 q;

				for (q = 1; q < g_Vars.roomcount && q < g_TileNumRooms; q++) {
					if (q != r && g_RdInvisDropped[q] > 0 && rdRoomsOverlap(q, r, 50.0f)) {
						ragdollDropInvisibleWalls(q);
					}
				}
			}

			done++;
		}
	}
}

/**
 * The game's floor tiles of a room as triangles (9 floats each), for the
 * generated collision to fill in floors it has no drawn surface for
 * (clipcheck.c). The array is reused by the next call.
 */
s32 ragdollGetRoomGameFloors(s32 roomnum, const f32 **outverts)
{
	static struct meshbuilder mb, mbwall;
	static f32 *tris = NULL;
	static s32 maxtris = 0;
	struct geostats stats;
	s32 t;

	*outverts = NULL;

	if (g_TileFileData.u8 == NULL || g_TileRooms == NULL || roomnum <= 0 || roomnum >= g_TileNumRooms) {
		return 0;
	}

	memset(&stats, 0, sizeof(stats));
	mbReset(&mb);
	mbReset(&mbwall);
	ragdollBuildRoom(&mb, &mbwall, roomnum, &stats);

	if (mb.numtris <= 0) {
		return 0;
	}

	if (mb.numtris > maxtris) {
		f32 *n = realloc(tris, mb.numtris * 9 * sizeof(f32));

		if (n == NULL) {
			return 0;
		}

		tris = n;
		maxtris = mb.numtris;
	}

	for (t = 0; t < mb.numtris; t++) {
		s32 k;

		for (k = 0; k < 3; k++) {
			memcpy(&tris[t * 9 + k * 3], &mb.verts[mb.indices[t * 3 + k] * 3], sizeof(f32) * 3);
		}
	}

	*outverts = tris;

	return mb.numtris;
}

#endif // PD_PHYSICS
