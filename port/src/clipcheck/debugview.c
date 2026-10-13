/*
 * MOD: clipping checks - the debug view (Game.ClipDebug / Extended > Physics >
 * Collision View): what the checks in clipcheck.c see around the player.
 */

#ifdef PD_CLIPCHECK

#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "bss.h"
#include "types.h"
#include "clipcheck.h"
#include "lib/mtx.h"
#include "game/camera.h"
#include "game/gfxmemory.h"
#include "clip_internal.h"

// ---------------------------------------------------------------- debug view

#define CLIPDBG_RANGE    700.0f // surfaces this close to the player are drawn
#define CLIPDBG_VSCALE   4.0f   // vertex units per world unit
#define CLIPDBG_MARKTIME 60     // ticks a contact marker stays

static const u32 g_ClipDbgMarkColours[CLIPDBG_NUMKINDS] = {
	0xff2020ff, // block: red
	0xffffffd0, // covered by the game's collision: white
	0x20ffffff, // step: cyan
	0x20ff20ff, // drawn floor under the feet: green
	0x4080ffff, // ceiling: blue
};

struct clipdbgdraw {
	Gfx *gdl;
	f32 origin[3];
	f32 cam[3];
	f32 nearmin2, nearmax2; // squared distance band drawn in this pass
	f32 tris[5][3][3];
	s32 numtris;
	u32 colour;
	s32 total;
};

static void clipDbgFlush(struct clipdbgdraw *d)
{
	Vtx *v;
	Col *col;
	s32 i, k;

	if (d->numtris == 0) {
		return;
	}

	v = gfxAllocateVertices(d->numtris * 3);
	col = gfxAllocateColours(1);
	col[0].word = PD_BE32(d->colour);

	for (i = 0; i < d->numtris; i++) {
		for (k = 0; k < 3; k++) {
			Vtx *vv = &v[i * 3 + k];
			f32 *p = d->tris[i][k];
			f32 x = (p[0] - d->origin[0]) * CLIPDBG_VSCALE;
			f32 y = (p[1] - d->origin[1]) * CLIPDBG_VSCALE;
			f32 z = (p[2] - d->origin[2]) * CLIPDBG_VSCALE;

			if (x > 32000) x = 32000; if (x < -32000) x = -32000;
			if (y > 32000) y = 32000; if (y < -32000) y = -32000;
			if (z > 32000) z = 32000; if (z < -32000) z = -32000;

			vv->x = (s16) x;
			vv->y = (s16) y;
			vv->z = (s16) z;
			vv->flags = 0;
			vv->colour = 0;
			vv->s = 0;
			vv->t = 0;
		}
	}

	gSPColor(d->gdl++, osVirtualToPhysical(col), 1);
	gSPVertex(d->gdl++, osVirtualToPhysical(v), d->numtris * 3, 0);

	for (i = 0; i < d->numtris; i++) {
		gSPTri1(d->gdl++, i * 3, i * 3 + 1, i * 3 + 2);
	}

	d->numtris = 0;
}

static void clipDbgTri(struct clipdbgdraw *d, const f32 *a, const f32 *b, const f32 *c, u32 colour)
{
	const f32 *src[3] = { a, b, c };
	s32 k, j;

	if (d->total >= g_ClipDbgBudget) {
		return;
	}

	if (colour != d->colour || d->numtris >= 5) {
		clipDbgFlush(d);
		d->colour = colour;
	}

	// Pulled a little toward the camera so surfaces lying on the scenery show
	for (k = 0; k < 3; k++) {
		f32 dir[3];
		f32 len;

		for (j = 0; j < 3; j++) {
			dir[j] = d->cam[j] - src[k][j];
		}

		len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);

		for (j = 0; j < 3; j++) {
			d->tris[d->numtris][k][j] = src[k][j] + (len > 0.01f ? dir[j] / len * 1.5f : 0.0f);
		}
	}

	d->numtris++;
	d->total++;
}

// A thin camera-facing quad from a to b
static void clipDbgLine(struct clipdbgdraw *d, const f32 *a, const f32 *b, f32 width, u32 colour)
{
	f32 dir[3], view[3], side[3], p[4][3];
	f32 len;
	s32 j;

	for (j = 0; j < 3; j++) {
		dir[j] = b[j] - a[j];
		view[j] = (a[j] + b[j]) * 0.5f - d->cam[j];
	}

	side[0] = dir[1] * view[2] - dir[2] * view[1];
	side[1] = dir[2] * view[0] - dir[0] * view[2];
	side[2] = dir[0] * view[1] - dir[1] * view[0];
	len = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);

	if (len < 0.0001f) {
		return;
	}

	for (j = 0; j < 3; j++) {
		side[j] *= width * 0.5f / len;
		p[0][j] = a[j] - side[j];
		p[1][j] = a[j] + side[j];
		p[2][j] = b[j] + side[j];
		p[3][j] = b[j] - side[j];
	}

	clipDbgTri(d, p[0], p[1], p[2], colour);
	clipDbgTri(d, p[0], p[2], p[3], colour);
}

static void clipDbgMarker(struct clipdbgdraw *d, const f32 *pos, f32 size, u32 colour)
{
	f32 a[3], b[3];

	a[0] = pos[0] - size; a[1] = pos[1]; a[2] = pos[2];
	b[0] = pos[0] + size; b[1] = pos[1]; b[2] = pos[2];
	clipDbgLine(d, a, b, size * 0.6f, colour);
	a[0] = pos[0]; a[1] = pos[1] - size; a[2] = pos[2];
	b[0] = pos[0]; b[1] = pos[1] + size; b[2] = pos[2];
	clipDbgLine(d, a, b, size * 0.6f, colour);
	a[0] = pos[0]; a[1] = pos[1]; a[2] = pos[2] - size;
	b[0] = pos[0]; b[1] = pos[1]; b[2] = pos[2] + size;
	clipDbgLine(d, a, b, size * 0.6f, colour);
}

static s32 clipDbgNear(struct clipdbgdraw *d, const f32 *a, const f32 *b, const f32 *c)
{
	// Nearest point of the triangle's bounds (big triangles far from their
	// middle still show where they are)
	f32 minx = a[0] < b[0] ? (a[0] < c[0] ? a[0] : c[0]) : (b[0] < c[0] ? b[0] : c[0]);
	f32 maxx = a[0] > b[0] ? (a[0] > c[0] ? a[0] : c[0]) : (b[0] > c[0] ? b[0] : c[0]);
	f32 minz = a[2] < b[2] ? (a[2] < c[2] ? a[2] : c[2]) : (b[2] < c[2] ? b[2] : c[2]);
	f32 maxz = a[2] > b[2] ? (a[2] > c[2] ? a[2] : c[2]) : (b[2] > c[2] ? b[2] : c[2]);
	f32 x = d->origin[0] < minx ? minx - d->origin[0] : d->origin[0] > maxx ? d->origin[0] - maxx : 0.0f;
	f32 z = d->origin[2] < minz ? minz - d->origin[2] : d->origin[2] > maxz ? d->origin[2] - maxz : 0.0f;

	f32 d2 = x * x + z * z;

	return d2 >= d->nearmin2 && d2 < d->nearmax2;
}

// The game's own collision tiles of one room (as collision.c reads them)
static void clipDbgGameRoom(struct clipdbgdraw *d, s32 roomnum)
{
	u8 *start, *end;
	struct geo *geo;
	s32 i;

	if (g_TileFileData.u8 == NULL || g_TileRooms == NULL || roomnum <= 0 || roomnum >= g_TileNumRooms) {
		return;
	}

	start = g_TileFileData.u8 + g_TileRooms[roomnum];
	end = g_TileFileData.u8 + g_TileRooms[roomnum + 1];
	geo = (struct geo *) start;

	while ((u8 *) geo < end && d->total < g_ClipDbgBudget) {
		u32 colour = (geo->flags & GEOFLAG_WALL) ? 0xff9020a0 : 0xffe02060;

		if (geo->type == GEOTYPE_TILE_I) {
			struct geotilei *tile = (struct geotilei *) geo;
			f32 v0[3] = { tile->vertices[0][0], tile->vertices[0][1], tile->vertices[0][2] };

			for (i = 1; i < geo->numvertices - 1; i++) {
				f32 v1[3] = { tile->vertices[i][0], tile->vertices[i][1], tile->vertices[i][2] };
				f32 v2[3] = { tile->vertices[i + 1][0], tile->vertices[i + 1][1], tile->vertices[i + 1][2] };

				if (clipDbgNear(d, v0, v1, v2)) {
					clipDbgTri(d, v0, v1, v2, colour);
				}
			}

			geo = (struct geo *) ((u8 *) geo + (uintptr_t) (geo->numvertices - 0x40) * 6 + 0x18e);
		} else if (geo->type == GEOTYPE_TILE_F) {
			struct geotilef *tile = (struct geotilef *) geo;
			f32 v0[3] = { tile->vertices[0].x, tile->vertices[0].y, tile->vertices[0].z };

			for (i = 1; i < geo->numvertices - 1; i++) {
				f32 v1[3] = { tile->vertices[i].x, tile->vertices[i].y, tile->vertices[i].z };
				f32 v2[3] = { tile->vertices[i + 1].x, tile->vertices[i + 1].y, tile->vertices[i + 1].z };

				if (clipDbgNear(d, v0, v1, v2)) {
					clipDbgTri(d, v0, v1, v2, colour);
				}
			}

			geo = (struct geo *) ((u8 *) geo + (uintptr_t) (geo->numvertices - 0x40) * 12 + 0x310);
		} else if (geo->type == GEOTYPE_BLOCK) {
			struct geoblock *block = (struct geoblock *) geo;
			s32 n = geo->numvertices > 8 ? 8 : geo->numvertices;

			for (i = 0; i < n; i++) {
				s32 next = (i + 1) % n;
				f32 a[3] = { block->vertices[i][0], block->ymin, block->vertices[i][1] };
				f32 b[3] = { block->vertices[next][0], block->ymin, block->vertices[next][1] };
				f32 c[3] = { block->vertices[next][0], block->ymax, block->vertices[next][1] };
				f32 e[3] = { block->vertices[i][0], block->ymax, block->vertices[i][1] };

				if (clipDbgNear(d, a, b, c)) {
					clipDbgTri(d, a, b, c, 0xff9020a0);
					clipDbgTri(d, a, c, e, 0xff9020a0);
				}
			}

			geo = (struct geo *) ((u8 *) geo + sizeof(struct geoblock));
		} else if (geo->type == GEOTYPE_CYL) {
			geo = (struct geo *) ((u8 *) geo + sizeof(struct geocyl));
		} else {
			break;
		}
	}
}

static void clipDbgDrawnRoom(struct clipdbgdraw *d, s32 roomnum)
{
	static const u32 colours[5] = {
		0x20ff2050, // floors: green
		0x2080ff60, // ceilings: blue
		0xff202060, // walls: red
		0x20ffff70, // not solid, water: cyan
		0xff20ff70, // not solid, translucent without the game's collision (light): magenta
	};
	struct cliproomtris *rt;
	s32 t;

	if (g_ClipRoomTris == NULL || roomnum <= 0 || roomnum >= CLIP_MAXROOMS) {
		return;
	}

	rt = &g_ClipRoomTris[roomnum];

	for (t = 0; t < rt->numtris && d->total < g_ClipDbgBudget; t++) {
		f32 *v = &rt->verts[t * 9];

		if (clipDbgNear(d, v, v + 3, v + 6)) {
			clipDbgTri(d, v, v + 3, v + 6, colours[rt->kinds[t]]);
		}
	}

	// Glass panes made solid: as walls
	for (t = 0; t < g_ClipNumGlass && d->total < g_ClipDbgBudget; t++) {
		static const s32 quads[4][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
		struct clipglass *g = &g_ClipGlass[t];
		s32 q;

		if (!g->live || g->room != roomnum) {
			continue;
		}

		for (q = 0; q < 4; q++) {
			if (clipDbgNear(d, g->verts[quads[q][0]], g->verts[quads[q][1]], g->verts[quads[q][2]])) {
				clipDbgTri(d, g->verts[quads[q][0]], g->verts[quads[q][1]], g->verts[quads[q][2]], colours[2]);
				clipDbgTri(d, g->verts[quads[q][0]], g->verts[quads[q][2]], g->verts[quads[q][3]], colours[2]);
			}
		}
	}
}

/**
 * lv.c, with the other world effects: draws what the clipping checks see
 * around the player (Game.ClipDebug / Extended > Physics > Collision View):
 *   1: drawn surfaces (floors green, ceilings blue, walls red)
 *   2: the game's own collision tiles (floors yellow, walls orange)
 *   3: both
 * plus the body capsule used for walls (yellow, red while blocked) and the
 * last second's contacts: red blocked, white left to the game's collision,
 * cyan step edge, green drawn floor under the feet, blue ceiling.
 */
Gfx *clipcheckRenderDebug(Gfx *gdl)
{
	struct player *player = g_Vars.currentplayer;
	struct clipdbgdraw d;
	Mtxf *w2s;
	Mtxf m;
	Mtxf *mtx;
	struct coord pos;
	s32 i;

	if (!g_ClipDebug || !g_ClipReady || player == NULL || player->prop == NULL) {
		return gdl;
	}

	memset(&d, 0, sizeof(d));
	d.origin[0] = player->prop->pos.x;
	d.origin[1] = player->vv_manground;
	d.origin[2] = player->prop->pos.z;
	d.cam[0] = player->cam_pos.x;
	d.cam[1] = player->cam_pos.y;
	d.cam[2] = player->cam_pos.z;

	gDPPipeSync(gdl++);
	gSPClearGeometryMode(gdl++, G_CULL_BOTH | G_LIGHTING | G_TEXTURE_GEN | G_FOG);
	gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH | G_ZBUFFER);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetRenderMode(gdl++, G_RM_ZB_XLU_SURF, G_RM_ZB_XLU_SURF2);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetCombineLERP(gdl++,
			0, 0, 0, SHADE, 0, 0, 0, SHADE,
			0, 0, 0, SHADE, 0, 0, 0, SHADE);
	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);

	w2s = camGetWorldToScreenMtxf();
	pos.x = d.origin[0];
	pos.y = d.origin[1];
	pos.z = d.origin[2];
	mtx4LoadIdentity(&m);
	m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f / CLIPDBG_VSCALE;
	mtx4SetTranslation(&pos, &m);
	mtx00015be0(w2s, &m);
	mtx = gfxAllocateMatrix();
	mtxF2L(&m, mtx);
	gSPMatrix(gdl++, osVirtualToPhysical(mtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

	d.gdl = gdl;

	// Markers and the capsule first, so they're never cut by the budget
	for (i = 0; i < CLIPDBG_MAXMARKS; i++) {
		struct clipdbgmark *mark = &g_ClipDbgMarks[i];

		if (mark->frame > 0 && g_Vars.lvframe60 - mark->frame >= 0 && g_Vars.lvframe60 - mark->frame < CLIPDBG_MARKTIME) {
			clipDbgMarker(&d, mark->pos, 4.0f, g_ClipDbgMarkColours[mark->kind]);
		}
	}

	if (g_ClipDbgCapsule[3] > 0.0f) {
		u32 colour = g_Vars.lvframe60 - g_ClipDbgBlockFrame < 10 ? 0xff2020e0 : 0xffff20c0;
		f32 r = g_ClipDbgCapsule[2];
		s32 k;

		for (k = 0; k < 12; k++) {
			f32 a0 = k * M_BADTAU / 12.0f;
			f32 a1 = (k + 1) * M_BADTAU / 12.0f;
			f32 lo0[3] = { d.origin[0] + sinf(a0) * r, g_ClipDbgCapsule[0], d.origin[2] + cosf(a0) * r };
			f32 lo1[3] = { d.origin[0] + sinf(a1) * r, g_ClipDbgCapsule[0], d.origin[2] + cosf(a1) * r };
			f32 hi0[3] = { lo0[0], g_ClipDbgCapsule[1], lo0[2] };
			f32 hi1[3] = { lo1[0], g_ClipDbgCapsule[1], lo1[2] };

			clipDbgLine(&d, lo0, lo1, 1.5f, colour);
			clipDbgLine(&d, hi0, hi1, 1.5f, colour);

			if ((k % 3) == 0) {
				clipDbgLine(&d, lo0, hi0, 1.5f, colour);
			}
		}
	}

	// Surfaces of the rooms the checks use around the player (rooms drawn
	// in the same space but not connected here aren't shown, nor used)
	{
		RoomNum rooms[48];
		s32 n = clipPlayerRoomList(player->bond2.radius + 8.0f, CLIPDBG_RANGE, d.origin[1] - CLIPDBG_RANGE, d.origin[1] + CLIPDBG_RANGE, rooms, ARRAYCOUNT(rooms));

		s32 pass;

		// Nearest first, so the budget never leaves gaps right around the player
		for (pass = 0; pass < 2; pass++) {
			d.nearmin2 = pass == 0 ? 0.0f : 300.0f * 300.0f;
			d.nearmax2 = pass == 0 ? 300.0f * 300.0f : CLIPDBG_RANGE * CLIPDBG_RANGE;

			for (i = 0; i < n; i++) {
				if (g_ClipDebug & 2) {
					clipDbgGameRoom(&d, rooms[i]);
				}

				if (g_ClipDebug & 1) {
					clipDbgDrawnRoom(&d, rooms[i]);
				}
			}
		}
	}

	clipDbgFlush(&d);
	gdl = d.gdl;

	gDPPipeSync(gdl++);
	gSPSetGeometryMode(gdl++, G_CULL_BACK);

	return gdl;
}

#endif // PD_CLIPCHECK
