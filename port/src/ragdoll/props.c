/**
 * MOD: ragdoll physics - objects, doors and lifts in the physics world: their
 * shapes, keeping them in sync, bodies pushing objects, breaking glass and
 * blocking doors. Part of the ragdoll module (port/src/ragdoll.c).
 */

#include "rd_internal.h"

#ifdef PD_PHYSICS

struct rdpropsnap *g_RdPropSnaps = NULL;
static s32 g_RdNumPropSnaps = 0;
#define RD_MAXPROPPIECES 16
#define RD_PROPRANGE     250.0f // how far around a ragdoll to look for props

struct ragdollprop g_RagdollProps[RD_MAXPROPS];
s32 g_RagdollNumProps = 0;

struct propgeo {
	pdphysPiece pieces[RD_MAXPROPPIECES];
	f32 verts[RD_MAXPROPPIECES][PDPHYS_MAX_PIECE_VERTS * 3];
	s32 numpieces;
};

#define RD_PROP_MINTHICK 14.0f // thin props (glass panes, railings) are made at least this thick

/**
 * A thin upright piece (a glass pane, a railing panel) is a few units thick
 * at most: a body pushed against it could get a part through, ending up
 * split across it. Its outline seen from above is widened until it's at
 * least RD_PROP_MINTHICK across (each point copied a little to either side).
 */
static void rdFattenPiece(pdphysPiece *piece, f32 *v)
{
	s32 n = piece->numverts;
	f32 thick = 1e9f;
	f32 d;
	s32 i, j, k;

	if (n < 3 || n * 4 > PDPHYS_MAX_PIECE_VERTS) {
		return;
	}

	// Narrowest width of the outline: over every direction between two points
	for (i = 0; i < n; i++) {
		for (j = i + 1; j < n; j++) {
			f32 ex = v[j * 3 + 0] - v[i * 3 + 0];
			f32 ez = v[j * 3 + 2] - v[i * 3 + 2];
			f32 len = sqrtf(ex * ex + ez * ez);
			f32 lo = 1e9f, hi = -1e9f;

			if (len < 1.0f) {
				continue;
			}

			for (k = 0; k < n; k++) {
				f32 t = ((v[k * 3 + 0] - v[i * 3 + 0]) * -ez + (v[k * 3 + 2] - v[i * 3 + 2]) * ex) / len;

				if (t < lo) lo = t;
				if (t > hi) hi = t;
			}

			if (hi - lo < thick) {
				thick = hi - lo;
			}
		}
	}

	if (thick >= RD_PROP_MINTHICK || thick > 1e8f) {
		return;
	}

	d = (RD_PROP_MINTHICK - thick) * 0.5f;

	for (i = n - 1; i >= 0; i--) {
		f32 x = v[i * 3 + 0], y = v[i * 3 + 1], z = v[i * 3 + 2];

		for (k = 0; k < 4; k++) {
			f32 *o = &v[(i * 4 + k) * 3];

			rdVecSet(o, x + (k == 0 ? d : k == 1 ? -d : 0.0f), y, z + (k == 2 ? d : k == 3 ? -d : 0.0f));
		}
	}

	piece->numverts = n * 4;
}

// Converts a prop's collision geometry into convex pieces
static bool ragdollGetPropGeometry(struct prop *prop, struct propgeo *pg)
{
	u8 *start = NULL;
	u8 *end = NULL;
	struct geo *geo;
	s32 i;

	pg->numpieces = 0;

	if (!propUpdateGeometry(prop, &start, &end) || start == NULL || end == NULL) {
		// Glass without collision of its own (the game uses the room's wall
		// behind it): its model box, so bodies don't fall through
		if (prop->type == PROPTYPE_OBJ && prop->obj
				&& (prop->obj->type == OBJTYPE_GLASS || prop->obj->type == OBJTYPE_TINTEDGLASS)) {
			f32 corner[8][3];

			if (rdObjBboxCorners(prop->obj, corner)) {
				pdphysPiece *piece = &pg->pieces[0];

				memcpy(pg->verts[0], corner, sizeof(corner));
				piece->verts = pg->verts[0];
				piece->numverts = 8;
				piece->thickness = 0.0f;
				rdFattenPiece(piece, pg->verts[0]);
				pg->numpieces = 1;
				return true;
			}
		}

		return false;
	}

	geo = (struct geo *) start;

	for (; (u8 *) geo < end && pg->numpieces < RD_MAXPROPPIECES; geo = rdGeoNext(geo)) {
		pdphysPiece *piece = &pg->pieces[pg->numpieces];
		f32 *v = pg->verts[pg->numpieces];
		s32 n = geo->numvertices;

		piece->verts = v;
		piece->numverts = 0;
		piece->thickness = 0.0f;

		if (geo->type == GEOTYPE_TILE_I) {
			struct geotilei *tile = (struct geotilei *) geo;

			for (i = 0; i < n && i < 64; i++) {
				v[i * 3 + 0] = tile->vertices[i][0];
				v[i * 3 + 1] = tile->vertices[i][1];
				v[i * 3 + 2] = tile->vertices[i][2];
			}

			piece->numverts = i;
			piece->thickness = 4.0f;
		} else if (geo->type == GEOTYPE_TILE_F) {
			struct geotilef *tile = (struct geotilef *) geo;

			for (i = 0; i < n && i < 64; i++) {
				v[i * 3 + 0] = tile->vertices[i].x;
				v[i * 3 + 1] = tile->vertices[i].y;
				v[i * 3 + 2] = tile->vertices[i].z;
			}

			piece->numverts = i;
			piece->thickness = 4.0f;
		} else if (geo->type == GEOTYPE_BLOCK) {
			struct geoblock *block = (struct geoblock *) geo;

			RD_CLAMPHI(n, 8);

			if (block->ymax > block->ymin + 0.5f) {
				for (i = 0; i < n; i++) {
					v[i * 6 + 0] = block->vertices[i][0];
					v[i * 6 + 1] = block->ymin;
					v[i * 6 + 2] = block->vertices[i][1];
					v[i * 6 + 3] = block->vertices[i][0];
					v[i * 6 + 4] = block->ymax;
					v[i * 6 + 5] = block->vertices[i][1];
				}

				piece->numverts = n * 2;
				rdFattenPiece(piece, v);
			}

		} else if (geo->type == GEOTYPE_CYL) {
			struct geocyl *cyl = (struct geocyl *) geo;

			if (cyl->ymax > cyl->ymin + 0.5f && cyl->radius > 0.5f) {
				for (i = 0; i < 8; i++) {
					f32 angle = i * (M_BADTAU / 8.0f);
					v[i * 6 + 0] = cyl->x + sinf(angle) * cyl->radius;
					v[i * 6 + 1] = cyl->ymin;
					v[i * 6 + 2] = cyl->z + cosf(angle) * cyl->radius;
					v[i * 6 + 3] = v[i * 6 + 0];
					v[i * 6 + 4] = cyl->ymax;
					v[i * 6 + 5] = v[i * 6 + 2];
				}

				piece->numverts = 16;
			}

		} else {
			break; // unknown type: the rest can't be walked
		}

		if (piece->numverts >= 3) {
			pg->numpieces++;
		}
	}

	return pg->numpieces > 0;
}


/**
 * Objects as their model's triangles instead of the game's collision boxes:
 * the boxes are what the player bumps into (a whole table is one block), so
 * bodies couldn't get under tables or onto couches. Only objects the game
 * gives collision to are built (if the player can walk through it, so can a
 * body); lifts, doors and glass keep the game's own geometry.
 * The model is read at rest: each display list's vertices are in the space
 * of the matrix it loads, placed at that node's rest position in the model,
 * then rotated and scaled by realrot and moved like the object.
 * Toggled-off parts (broken variants) are skipped, and of LOD branches only
 * the closest one is used.
 */
#define RD_PROPMESH_MAXVERTS 6144
#define RD_PROPMESH_MAXTRIS  4096
#define RD_PROPMESH_MINTRIS  4

struct propmesh {
	f32 verts[RD_PROPMESH_MAXVERTS * 3];
	s32 indices[RD_PROPMESH_MAXTRIS * 3];
	s32 numverts;
	s32 numtris;
	bool full;
};

struct rdmeshctx {
	struct model *model;
	struct defaultobj *obj;
	struct propmesh *pm;
	f32 scale;
};

static void rdMeshAddVert(struct rdmeshctx *ctx, const f32 *local, s32 *outindex)
{
	struct propmesh *pm = ctx->pm;
	struct defaultobj *obj = ctx->obj;
	f32 l[3];
	s32 k;

	if (pm->numverts >= RD_PROPMESH_MAXVERTS) {
		pm->full = true;
		*outindex = -1;
		return;
	}

	rdVecScale(local, ctx->scale, l);

	for (k = 0; k < 3; k++) {
		pm->verts[pm->numverts * 3 + k] = obj->prop->pos.f[k] + obj->realrot[0][k] * l[0] + obj->realrot[1][k] * l[1] + obj->realrot[2][k] * l[2];
	}

	*outindex = pm->numverts++;
}

/**
 * Rest position of a node relative to the model's root. The root's own
 * position isn't part of it: an object's root matrix is the object's
 * (crates have their setup offset there, e.g. 750,50,50, but are drawn
 * around the prop position).
 */
static void rdMeshNodeOffset(struct rdmeshctx *ctx, struct modelnode *node, f32 *out)
{
	struct modelnode *root = ctx->model->definition->rootnode;
	struct coord pos;
	struct coord rootpos;

	modelNodeGetModelRelativePosition(ctx->model, node, &pos);
	modelNodeGetPosition(ctx->model, root, &rootpos);

	rdVecSet(out, pos.x - rootpos.x, pos.y - rootpos.y, pos.z - rootpos.z);
}

static void rdMeshParseGdl(struct rdmeshctx *ctx, struct modelnode *dlnode, Gfx *gdl, Vtx *vertices, s32 numvertices)
{
	struct model *model = ctx->model;
	struct propmesh *pm = ctx->pm;
	s32 slots[16];
	f32 offset[3];
	s32 n, i;

	if (gdl == NULL || vertices == NULL) {
		return;
	}

	// Until the list loads a matrix, its vertices are in the space of the
	// nearest positioned ancestor
	rdMeshNodeOffset(ctx, dlnode, offset);

	for (i = 0; i < 16; i++) {
		slots[i] = -1;
	}

	for (n = 0; n < 8192 && !pm->full; n++, gdl++) {
		s8 cmd = (s8) gdl->dma.cmd; // the GBI's opcodes are signed

		if (cmd == (s8) G_ENDDL || cmd == (s8) G_DL) {
			break;
		}

		if (cmd == (s8) G_MTX) {
			s32 idx = (s32)((UNSEGADDR(gdl->words.w1) & 0xffffff) / sizeof(Mtxf));
			struct modelnode *mnode = modelFindNodeByMtxIndex(model, idx);

			if (mnode) {
				rdMeshNodeOffset(ctx, mnode, offset);
			}
		} else if (cmd == (s8) G_VTX) {
			s32 start = (s32)((UNSEGADDR(gdl->words.w1) & 0xffffff) / sizeof(Vtx));
			u32 b1 = gdl->bytes[GFX_W0_BYTE(1)];
			s32 count = (s32)(b1 >> 4) + 1;
			s32 v0 = (s32)(b1 & 0xf);
			s32 v;

			for (v = 0; v < count && v0 + v < 16; v++) {
				s32 vi = start + v;

				if (vi < 0 || vi >= numvertices) {
					slots[v0 + v] = -1;
				} else {
					f32 local[3];

					local[0] = vertices[vi].x + offset[0];
					local[1] = vertices[vi].y + offset[1];
					local[2] = vertices[vi].z + offset[2];
					rdMeshAddVert(ctx, local, &slots[v0 + v]);
				}
			}
		} else if (cmd == (s8) G_TRI1 || cmd == (s8) G_TRI4) {
			s32 tris[4][3];
			s32 numtris = 0;
			s32 t;

			if (cmd == (s8) G_TRI1) {
				tris[0][0] = gdl->tri.tri.v[GFX_TRI_VTX(0)] / 10;
				tris[0][1] = gdl->tri.tri.v[GFX_TRI_VTX(1)] / 10;
				tris[0][2] = gdl->tri.tri.v[GFX_TRI_VTX(2)] / 10;
				numtris = 1;
			} else {
				tris[0][0] = gdl->tri4.x1; tris[0][1] = gdl->tri4.y1; tris[0][2] = gdl->tri4.z1;
				tris[1][0] = gdl->tri4.x2; tris[1][1] = gdl->tri4.y2; tris[1][2] = gdl->tri4.z2;
				tris[2][0] = gdl->tri4.x3; tris[2][1] = gdl->tri4.y3; tris[2][2] = gdl->tri4.z3;
				tris[3][0] = gdl->tri4.x4; tris[3][1] = gdl->tri4.y4; tris[3][2] = gdl->tri4.z4;
				numtris = 4;
			}

			for (t = 0; t < numtris; t++) {
				s32 a = tris[t][0], b = tris[t][1], c = tris[t][2];

				if (cmd == (s8) G_TRI4 && a == 0 && b == 0 && c == 0) {
					continue;
				}

				if (a < 0 || a >= 16 || b < 0 || b >= 16 || c < 0 || c >= 16
						|| slots[a] < 0 || slots[b] < 0 || slots[c] < 0) {
					continue;
				}

				if (pm->numtris >= RD_PROPMESH_MAXTRIS) {
					pm->full = true;
					break;
				}

				pm->indices[pm->numtris * 3 + 0] = slots[a];
				pm->indices[pm->numtris * 3 + 1] = slots[b];
				pm->indices[pm->numtris * 3 + 2] = slots[c];
				pm->numtris++;
			}
		}
	}
}

// The model's display lists use segmented addresses relative to their colour table
static Gfx *rdMeshGdlPtr(Gfx *gdl, struct modelrodata_dl *rodata, bool fromdef)
{
	if (gdl == NULL) {
		return NULL;
	}

	if (((uintptr_t) gdl & 1) || fromdef) {
		return (Gfx *)((uintptr_t) rodata->colours + ((uintptr_t) UNSEGADDR(gdl) & 0xffffff));
	}

	return gdl;
}

static void rdMeshWalk(struct rdmeshctx *ctx, struct modelnode *node, s32 depth)
{
	struct model *model = ctx->model;

	for (; node && depth < 32 && !ctx->pm->full; node = node->next) {
		struct modelnode *child = node->child;
		u32 type = node->type & 0xff;

		if (type == MODELNODETYPE_DL) {
			struct modelrodata_dl *rodata = &node->rodata->dl;
			struct modelrwdata_dl *rwdata = (struct modelrwdata_dl *) modelGetNodeRwData(model, node);

			if (rwdata && rwdata->gdl) {
				Vtx *vertices = rwdata->vertices ? rwdata->vertices : rodata->vertices;

				rdMeshParseGdl(ctx, node, rdMeshGdlPtr(rwdata->gdl, rodata, rwdata->gdl == rodata->opagdl), vertices, rodata->numvertices);

				if (rodata->xlugdl) {
					rdMeshParseGdl(ctx, node, rdMeshGdlPtr(rodata->xlugdl, rodata, true), vertices, rodata->numvertices);
				}
			}
		} else if (type == MODELNODETYPE_TOGGLE) {
			struct modelrwdata_toggle *rw = (struct modelrwdata_toggle *) modelGetNodeRwData(model, node);
			child = rw && rw->visible ? node->rodata->toggle.target : NULL;
		} else if (type == MODELNODETYPE_DISTANCE) {
			child = node->rodata->distance.near <= 0.0f ? node->rodata->distance.target : NULL;
		} else if (type == MODELNODETYPE_HEADSPOT) {
			child = NULL;
		}

		if (child) {
			rdMeshWalk(ctx, child, depth + 1);
		}
	}
}

/**
 * Office chairs' backrests (MODEL_DD_CHAIR: Defection, Investigation...) are
 * drawn as a single panel, so a body could fold through it. Each backrest
 * triangle is extruded into a slab RD_BACKREST_THICK deep, behind the panel
 * (away from the seat), so the front stays where it is and a guard sitting
 * in the chair isn't in it. Backrest triangles: upright for the chair (its
 * own up axis, so a chair that's been turned works too) and reaching above
 * the seat (the prop's position is at seat height; the stem and the seat
 * stay below it).
 */
#define RD_BACKREST_THICK 12.0f

static bool rdObjHasThinBackrest(struct defaultobj *obj)
{
	return obj->modelnum == MODEL_DD_CHAIR;
}

static void rdThickenBackrest(struct propmesh *pm, struct defaultobj *obj)
{
	f32 *pos = obj->prop->pos.f;
	f32 up[3];
	f32 len;
	s32 numtris = pm->numtris;
	s32 t, k;

	rdVecCopy(obj->realrot[1], up);
	len = rdVecLen(up);

	if (len < 0.0001f) {
		return;
	}

	rdVecDiv(up, len);

	for (t = 0; t < numtris; t++) {
		s32 idx[3] = { pm->indices[t * 3 + 0], pm->indices[t * 3 + 1], pm->indices[t * 3 + 2] };
		f32 *a = &pm->verts[idx[0] * 3];
		f32 *b = &pm->verts[idx[1] * 3];
		f32 *c = &pm->verts[idx[2] * 3];
		f32 e1[3], e2[3], n[3], cen[3], away[3];
		f32 nl, nup, top, side;
		s32 base;

		for (k = 0; k < 3; k++) {
			e1[k] = b[k] - a[k];
			e2[k] = c[k] - a[k];
			cen[k] = (a[k] + b[k] + c[k]) / 3.0f;
		}

		n[0] = e1[1] * e2[2] - e1[2] * e2[1];
		n[1] = e1[2] * e2[0] - e1[0] * e2[2];
		n[2] = e1[0] * e2[1] - e1[1] * e2[0];
		nl = rdVecLen(n);

		if (nl < 0.0001f) {
			continue;
		}

		for (k = 0; k < 3; k++) {
			n[k] /= nl;
		}

		nup = rdVecDot(n, up);

		if (nup > 0.35f || nup < -0.35f) {
			continue; // not upright
		}

		top = -1e9f;

		for (k = 0; k < 3; k++) {
			f32 *v = &pm->verts[idx[k] * 3];
			f32 h = (v[0] - pos[0]) * up[0] + (v[1] - pos[1]) * up[1] + (v[2] - pos[2]) * up[2];

			if (h > top) {
				top = h;
			}
		}

		if (top < 1.0f) {
			continue; // the stem or the seat, not the backrest
		}

		// Behind the panel: along its normal (made level), away from the seat
		for (k = 0; k < 3; k++) {
			away[k] = n[k] - up[k] * nup;
		}

		nl = rdVecLen(away);

		if (nl < 0.0001f) {
			continue;
		}

		side = (cen[0] - pos[0]) * away[0] + (cen[1] - pos[1]) * away[1] + (cen[2] - pos[2]) * away[2];

		for (k = 0; k < 3; k++) {
			away[k] *= (side < 0.0f ? -RD_BACKREST_THICK : RD_BACKREST_THICK) / nl;
		}

		if (pm->numverts + 3 > RD_PROPMESH_MAXVERTS || pm->numtris + 7 > RD_PROPMESH_MAXTRIS) {
			return;
		}

		// A prism: the panel triangle, the same moved back, and its three sides
		base = pm->numverts;

		for (k = 0; k < 3; k++) {
			f32 *v = &pm->verts[idx[k] * 3];
			pm->verts[(base + k) * 3 + 0] = v[0] + away[0];
			pm->verts[(base + k) * 3 + 1] = v[1] + away[1];
			pm->verts[(base + k) * 3 + 2] = v[2] + away[2];
		}

		pm->numverts += 3;

		pm->indices[pm->numtris * 3 + 0] = base + 0;
		pm->indices[pm->numtris * 3 + 1] = base + 2;
		pm->indices[pm->numtris * 3 + 2] = base + 1;
		pm->numtris++;

		for (k = 0; k < 3; k++) {
			s32 i0 = idx[k], i1 = idx[(k + 1) % 3];
			s32 j0 = base + k, j1 = base + (k + 1) % 3;

			pm->indices[pm->numtris * 3 + 0] = i0;
			pm->indices[pm->numtris * 3 + 1] = j0;
			pm->indices[pm->numtris * 3 + 2] = i1;
			pm->numtris++;
			pm->indices[pm->numtris * 3 + 0] = i1;
			pm->indices[pm->numtris * 3 + 1] = j0;
			pm->indices[pm->numtris * 3 + 2] = j1;
			pm->numtris++;
		}
	}
}

static bool ragdollGetPropModelMesh(struct prop *prop, struct propmesh *pm)
{
	struct defaultobj *obj;
	struct rdmeshctx ctx;
	u8 *start = NULL;
	u8 *end = NULL;

	pm->numverts = 0;
	pm->numtris = 0;
	pm->full = false;

	if (prop->type != PROPTYPE_OBJ || (obj = prop->obj) == NULL || obj->model == NULL
			|| obj->model->definition == NULL || obj->model->definition->rootnode == NULL
			|| obj->type == OBJTYPE_LIFT || obj->type == OBJTYPE_GLASS || obj->type == OBJTYPE_TINTEDGLASS
			|| obj->type == OBJTYPE_DOOR) {
		return false;
	}

	// Only what the game itself treats as solid
	if (!propUpdateGeometry(prop, &start, &end) || start == NULL || end == NULL) {
		return false;
	}

	ctx.model = obj->model;
	ctx.obj = obj;
	ctx.pm = pm;
	ctx.scale = 1.0f; // realrot already has the object's scale in it (as the game's objGetRotatedLocal*ByMtx3)

	rdMeshWalk(&ctx, obj->model->definition->rootnode, 0);

	if (rdObjHasThinBackrest(obj) && !pm->full) {
		rdThickenBackrest(pm, obj);
	}

	return pm->numtris >= RD_PROPMESH_MINTRIS;
}

/**
 * MOD: clipping checks - an object's model as world triangles, for objects
 * that stand in for level walls without collision of their own
 * (clipcheck.c). Returns the number of triangles (0 if none or too many).
 */
s32 ragdollObjModelTris(struct prop *prop, f32 *verts, s32 maxverts, s32 *indices, s32 maxtris, s32 *outnumverts)
{
	static struct propmesh pm;
	struct defaultobj *obj;
	struct rdmeshctx ctx;

	*outnumverts = 0;

	if (prop == NULL || prop->type != PROPTYPE_OBJ || (obj = prop->obj) == NULL || obj->model == NULL
			|| obj->model->definition == NULL || obj->model->definition->rootnode == NULL) {
		return 0;
	}

	pm.numverts = 0;
	pm.numtris = 0;
	pm.full = false;

	ctx.model = obj->model;
	ctx.obj = obj;
	ctx.pm = &pm;
	ctx.scale = 1.0f;

	rdMeshWalk(&ctx, obj->model->definition->rootnode, 0);

	if (pm.numtris <= 0 || pm.numverts > maxverts || pm.numtris > maxtris) {
		return 0;
	}

	memcpy(verts, pm.verts, pm.numverts * 3 * sizeof(f32));
	memcpy(indices, pm.indices, pm.numtris * 3 * sizeof(s32));
	*outnumverts = pm.numverts;

	return pm.numtris;
}

static u32 ragdollPropMeshSignature(struct propmesh *pm, f32 *origin)
{
	u32 sig = 2166136261u ^ 0x5a5a5a5au;
	s32 i;

	rdVecCopy(pm->verts, origin);

	sig = (sig ^ (u32) pm->numtris) * 16777619u;

	for (i = 0; i < pm->numverts * 3; i++) {
		s32 q = (s32) floorf((pm->verts[i] - origin[i % 3]) * 2.0f + 0.5f);
		sig = (sig ^ (u32) q) * 16777619u;
	}

	return sig;
}

// Shape signature: all vertices relative to the first one, quantised
static u32 ragdollPropSignature(struct propgeo *pg, f32 *origin)
{
	u32 sig = 2166136261u;
	s32 p, i;

	rdVecCopy(pg->pieces[0].verts, origin);

	for (p = 0; p < pg->numpieces; p++) {
		sig = (sig ^ (u32) pg->pieces[p].numverts) * 16777619u;

		for (i = 0; i < pg->pieces[p].numverts * 3; i++) {
			s32 q = (s32) floorf((pg->pieces[p].verts[i] - origin[i % 3]) * 2.0f + 0.5f);
			sig = (sig ^ (u32) q) * 16777619u;
		}
	}

	return sig;
}

static void ragdollPropRelease(struct ragdollprop *rp)
{
	if (rp->prop) {
		pdphysPropDestroy(rp->handle);
		rp->prop = NULL;
		rp->handle = -1;
		g_RagdollNumProps--;
	}
}

void ragdollPropsReleaseAll(void)
{
	s32 i;

	for (i = 0; i < RD_MAXPROPS; i++) {
		ragdollPropRelease(&g_RagdollProps[i]);
	}

	g_RagdollNumProps = 0;
}

static f32 g_RdPropVelScale = 1.0f; // speed given to the prop being synced (ragdollSyncPropGeo)
static bool g_RdPropLiving = false; // the prop being synced is a living character (ragdollSyncPropGeo)

// The slot this prop has (NULL if none); *freeslot a free one, if wanted
static struct ragdollprop *rdPropFind(struct prop *prop, struct ragdollprop **freeslot)
{
	s32 i;

	if (freeslot) {
		*freeslot = NULL;
	}

	for (i = 0; i < RD_MAXPROPS; i++) {
		if (g_RagdollProps[i].prop == prop) {
			return &g_RagdollProps[i];
		}

		if (freeslot && !*freeslot && !g_RagdollProps[i].prop) {
			*freeslot = &g_RagdollProps[i];
		}
	}

	return NULL;
}

// The slot of the physics prop with this handle (NULL if none, or ph < 0)
struct ragdollprop *ragdollPropByHandle(s32 ph)
{
	s32 i;

	for (i = 0; i < RD_MAXPROPS && ph >= 0; i++) {
		if (g_RagdollProps[i].prop && g_RagdollProps[i].handle == ph) {
			return &g_RagdollProps[i];
		}
	}

	return NULL;
}

/**
 * A new physics body for a prop (handle already made): the slot takes it.
 * A body riding a lift touches its walls and rails, and the player standing
 * over a body brushes it: that doesn't end its death animation.
 */
static void rdPropBind(struct ragdollprop *slot, struct prop *prop, u32 sig, const f32 *origin)
{
	if ((prop->type == PROPTYPE_OBJ && prop->obj && prop->obj->type == OBJTYPE_LIFT) || prop->type == PROPTYPE_PLAYER) {
		pdphysPropSetNoBlock(slot->handle, true);
	}

	if (g_RdPropLiving) {
		pdphysPropSetLiving(slot->handle);
	}

	slot->prop = prop;
	slot->sig = sig;
	rdVecCopy(origin, slot->origin);
	slot->seen = true;
	slot->lastimpact60 = 0;
	slot->nbuild = 0;
	g_RagdollNumProps++;
}

static void ragdollSyncPropGeo(struct prop *prop, struct propgeo *pgp, f32 dt);
static void ragdollSyncPropMesh(struct prop *prop, struct propmesh *pm, f32 dt);
static struct propmesh g_RdPropMesh;

static void ragdollSyncProp(struct prop *prop, f32 dt)
{
	static struct propgeo pg;
	struct ragdollprop *rp = rdPropFind(prop, NULL);

	if (rp && rp->seen) {
		return; // already handled this tick (near more than one ragdoll)
	}

	// What the player walks through, bodies fall through too (except glass
	// without collision of its own: the game relies on the wall behind it)
	if (prop->type == PROPTYPE_OBJ && prop->obj && !propIsOfCdType(prop, CDTYPE_ALL)
			&& prop->obj->type != OBJTYPE_GLASS && prop->obj->type != OBJTYPE_TINTEDGLASS) {
		if (rp) {
			ragdollPropRelease(rp);
		}

		return;
	}

	if (ragdollGetPropModelMesh(prop, &g_RdPropMesh)) {
		ragdollSyncPropMesh(prop, &g_RdPropMesh, dt);
		return;
	}

	if (!ragdollGetPropGeometry(prop, &pg)) {
		if (rp) {
			ragdollPropRelease(rp);
		}

		return;
	}

	ragdollSyncPropGeo(prop, &pg, dt);
}

/**
 * Living chrs and players as kinematic cylinders. Starts a little above the
 * floor so feet slide over flat bodies. With Ragdoll.RagdollChrPush on, walking
 * into a corpse pushes it; off, walking over it doesn't touch it at all
 * (pdphysSetLivingPassThrough) - but a body thrown or falling onto someone
 * still hits them either way.
 */
#define RD_CHRPUSH_SCALE 0.01f // living characters stepping on bodies: 1% of their speed, a faint nudge

static void ragdollSyncLivingChr(struct prop *prop, f32 ground, f32 height, f32 radius, f32 dt)
{
	static struct propgeo pg;
	f32 *v = pg.verts[0];
	f32 ymin = ground + 12.0f;
	f32 ymax = ground + height;
	s32 i;

	RD_CLAMPLO(ymax, ymin + 20.0f);

	for (i = 0; i < 8; i++) {
		f32 angle = i * (M_BADTAU / 8.0f);
		v[i * 6 + 0] = prop->pos.x + sinf(angle) * radius;
		v[i * 6 + 1] = ymin;
		v[i * 6 + 2] = prop->pos.z + cosf(angle) * radius;
		v[i * 6 + 3] = v[i * 6 + 0];
		v[i * 6 + 4] = ymax;
		v[i * 6 + 5] = v[i * 6 + 2];
	}

	pg.pieces[0].verts = v;
	pg.pieces[0].numverts = 16;
	pg.pieces[0].thickness = 0.0f;
	pg.numpieces = 1;

	g_RdPropVelScale = RD_CHRPUSH_SCALE;
	g_RdPropLiving = true;
	ragdollSyncPropGeo(prop, &pg, dt);
	g_RdPropLiving = false;
	g_RdPropVelScale = 1.0f;
}

/**
 * Doors as they swing: the game rebuilds a door's geometry turned, which
 * changed the physics shape and so rebuilt it in place every tick - a
 * swinging door teleported through bodies instead of pushing them. If the
 * geometry is the shape it was built from turned about the vertical (and
 * moved), the body is turned and moved instead, with velocity, so it shoves
 * what's in its way.
 */
static u32 rdPropLayout(struct propgeo *pg, s32 *outcount)
{
	u32 sig = 2166136261u;
	s32 p;
	s32 count = 0;

	for (p = 0; p < pg->numpieces; p++) {
		sig = (sig ^ (u32) pg->pieces[p].numverts) * 16777619u;
		count += pg->pieces[p].numverts;
	}

	*outcount = count;

	return sig;
}

static void rdPropStoreBuild(struct ragdollprop *rp, struct propgeo *pg, const f32 *origin)
{
	s32 count;
	s32 p, i, k = 0;

	rp->nbuild = 0;
	rp->layout = rdPropLayout(pg, &count);

	if (rp->prop->type != PROPTYPE_DOOR || count <= 0 || count > RD_PROPFIT_MAXVERTS) {
		return;
	}

	rp->buildcentre[0] = rp->buildcentre[1] = rp->buildcentre[2] = 0.0f;

	for (p = 0; p < pg->numpieces; p++) {
		for (i = 0; i < pg->pieces[p].numverts * 3; i++) {
			rp->build[k] = pg->pieces[p].verts[i];
			rp->buildcentre[k % 3] += rp->build[k];
			k++;
		}
	}

	for (i = 0; i < 3; i++) {
		rp->buildcentre[i] /= count;
		rp->buildorigin[i] = origin[i];
	}

	rp->nbuild = count;
}

static bool rdPropFollowTurn(struct ragdollprop *rp, struct propgeo *pg, f32 dt)
{
	f32 centre[3] = {0, 0, 0};
	f32 sum_sin = 0.0f;
	f32 sum_cos = 0.0f;
	f32 angle, s, c;
	f32 pos[3];
	f32 rot[4];
	s32 count;
	s32 p, i, k;

	if (rp->nbuild <= 0 || rdPropLayout(pg, &count) != rp->layout || count != rp->nbuild) {
		return false;
	}

	k = 0;

	for (p = 0; p < pg->numpieces; p++) {
		for (i = 0; i < pg->pieces[p].numverts * 3; i++) {
			centre[k % 3] += pg->pieces[p].verts[i];
			k++;
		}
	}

	for (i = 0; i < 3; i++) {
		centre[i] /= count;
	}

	// The turn about y that best takes the built shape to this one
	k = 0;

	for (p = 0; p < pg->numpieces; p++) {
		for (i = 0; i < pg->pieces[p].numverts; i++, k++) {
			f32 ax = rp->build[k * 3 + 0] - rp->buildcentre[0];
			f32 az = rp->build[k * 3 + 2] - rp->buildcentre[2];
			f32 bx = pg->pieces[p].verts[i * 3 + 0] - centre[0];
			f32 bz = pg->pieces[p].verts[i * 3 + 2] - centre[2];

			sum_sin += az * bx - ax * bz;
			sum_cos += ax * bx + az * bz;
		}
	}

	angle = atan2f(sum_sin, sum_cos);

	// atan2f here is the game's: 0..2pi
	if (angle > M_PI) {
		angle -= 2.0f * M_PI;
	}

	s = sinf(angle);
	c = cosf(angle);

	// Every vertex must fit (else it changed shape: rebuild it)
	k = 0;

	for (p = 0; p < pg->numpieces; p++) {
		for (i = 0; i < pg->pieces[p].numverts; i++, k++) {
			f32 ax = rp->build[k * 3 + 0] - rp->buildcentre[0];
			f32 ay = rp->build[k * 3 + 1] - rp->buildcentre[1];
			f32 az = rp->build[k * 3 + 2] - rp->buildcentre[2];
			f32 ex = centre[0] + ax * c + az * s - pg->pieces[p].verts[i * 3 + 0];
			f32 ey = centre[1] + ay - pg->pieces[p].verts[i * 3 + 1];
			f32 ez = centre[2] - ax * s + az * c - pg->pieces[p].verts[i * 3 + 2];

			if (ex * ex + ey * ey + ez * ez > 1.5f * 1.5f) {
				return false;
			}
		}
	}

	// Where the body's origin goes: the built origin, turned about the
	// shape's centre and moved with it
	{
		f32 ox = rp->buildorigin[0] - rp->buildcentre[0];
		f32 oz = rp->buildorigin[2] - rp->buildcentre[2];

		pos[0] = centre[0] + ox * c + oz * s;
		pos[1] = centre[1] + rp->buildorigin[1] - rp->buildcentre[1];
		pos[2] = centre[2] - ox * s + oz * c;
	}

	rdVecSet(rot, 0.0f, sinf(angle * 0.5f), 0.0f);
	rot[3] = cosf(angle * 0.5f);

	pdphysPropMoveRot(rp->handle, pos, rot, dt);
	rp->seen = true;

	return true;
}

static void ragdollSyncPropGeo(struct prop *prop, struct propgeo *pg_, f32 dt)
{
	struct ragdollprop *freeslot;
	struct ragdollprop *rp = rdPropFind(prop, &freeslot);
	f32 origin[3];
	u32 sig;

	if (rp && rp->seen) {
		return; // already handled this tick (near more than one ragdoll)
	}

	sig = ragdollPropSignature(pg_, origin);

	if (rp && rp->sig == sig) {
		// Same shape: follow the prop. Done every tick, even when it hasn't
		// moved, because a kinematic body keeps its last velocity otherwise.
		pdphysPropMoveScaled(rp->handle, origin, dt, g_RdPropVelScale);
		rdVecCopy(origin, rp->origin);

		rp->seen = true;
		return;
	}

	// A door turning: turn the body (pushes what's in the way)
	if (rp && rdPropFollowTurn(rp, pg_, dt)) {
		return;
	}

	// New prop, or its shape changed: (re)build it
	if (rp) {
		ragdollPropRelease(rp);
		freeslot = rp;
	}

	if (!freeslot) {
		return;
	}

	freeslot->handle = pdphysPropCreate(pg_->pieces, pg_->numpieces, origin);

	if (freeslot->handle < 0) {
		return;
	}

	rdPropBind(freeslot, prop, sig, origin);
	rdPropStoreBuild(freeslot, pg_, origin);
}

static void ragdollSyncPropMesh(struct prop *prop, struct propmesh *pm, f32 dt)
{
	struct ragdollprop *freeslot;
	struct ragdollprop *rp = rdPropFind(prop, &freeslot);
	f32 origin[3];
	u32 sig;

	if (rp && rp->seen) {
		return;
	}

	sig = ragdollPropMeshSignature(pm, origin);

	if (rp && rp->sig == sig) {
		pdphysPropMoveScaled(rp->handle, origin, dt, g_RdPropVelScale);
		rdVecCopy(origin, rp->origin);
		rp->seen = true;
		return;
	}

	if (rp) {
		ragdollPropRelease(rp);
		freeslot = rp;
	}

	if (!freeslot) {
		return;
	}

	freeslot->handle = pdphysPropCreateMesh(pm->verts, pm->numverts, pm->indices, pm->numtris, origin);

	if (freeslot->handle < 0) {
		return;
	}

	rdPropBind(freeslot, prop, sig, origin);

	if (g_RdPropMeshLogged < 40) {
		g_RdPropMeshLogged++;
		sysLogPrintf(LOG_NOTE, "PHYSICS: object %d (type %d) solid as its model: %d triangles%s",
				(s32) (prop - g_Vars.props), prop->obj->type, pm->numtris, pm->full ? " (truncated)" : "");
	}
}

/**
 * Called each tick before stepping: makes sure every door/lift/object near a
 * simulating ragdoll exists in Jolt and is where the game says it is.
 */
/**
 * The doors and objects within reach of a place a body (or a cut piece of
 * one) is: in its rooms and the rooms next to them
 */
static void ragdollSyncPropsAround(struct coord *at, RoomNum *atrooms, bool liftsonly, f32 dt, s16 *propnums)
{
	struct coord lower;
	struct coord upper;
	RoomNum rooms[8];
	s32 j;

	lower.x = at->x - RD_PROPRANGE;
	lower.y = at->y - RD_PROPRANGE;
	lower.z = at->z - RD_PROPRANGE;
	upper.x = at->x + RD_PROPRANGE;
	upper.y = at->y + RD_PROPRANGE;
	upper.z = at->z + RD_PROPRANGE;

	// Start from the ragdoll's rooms and add neighbours within range
	roomsCopy(atrooms, rooms);
	rooms[7] = -1;
	bgFindEnteredRooms(&lower, &upper, rooms, 7, true);

	roomGetProps(rooms, propnums, 256);

	for (j = 0; propnums[j] >= 0; j++) {
		struct prop *prop = &g_Vars.props[propnums[j]];

		if ((prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_DOOR) && prop->obj
				&& (!liftsonly || (prop->type == PROPTYPE_OBJ && prop->obj->type == OBJTYPE_LIFT))) {
			// Rooms can be big; skip props that are clearly out of reach
			// (prop->pos is roughly the centre, so allow for large objects)
			if (rdDistSqCoord(prop->pos.f, at) < RD_SQ(RD_PROPRANGE + 400.0f)) {
				ragdollSyncProp(prop, dt); // once a tick, however many bodies and pieces are near
			}
		}
	}
}

void ragdollSyncProps(f32 dt)
{
	RD_CRASHCTX("syncprops", -1, -1);
	s16 propnums[256];
	s32 i, j;
	bool any = false;

	for (i = 0; i < RD_MAXPROPS; i++) {
		g_RagdollProps[i].seen = false;
	}

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];
		bool liftsonly;
		s32 k;

		if (rd->state != RDSTATE_SIMULATING) {
			continue;
		}

		// Sleeping corpses don't need their surroundings refreshed, except
		// lifts: a lift must already be under a body when it starts moving
		liftsonly = rd->settled && pdphysRagdollIsAsleep(rd->handle);
		any = true;

		ragdollSyncPropsAround(&rd->prop->pos, rd->prop->rooms, liftsonly, dt, propnums);

		// MOD: ragdoll physics - and around each cut piece away from the
		// body: a piece carried off by the player (or thrown) went through
		// the doors and objects there
		for (k = 1; k < rd->numparts; k++) {
			if ((rd->severed & (1u << k)) && rd->piecerooms[k][0] > 0) {
				struct coord at;

				rdVecToCoord(rd->world[k].m[3], &at);

				if (rdDistSqCoord(rd->world[k].m[3], &rd->prop->pos) > RD_SQ(RD_PROPRANGE * 0.5f)) {
					ragdollSyncPropsAround(&at, rd->piecerooms[k], liftsonly, dt, propnums);
				}
			}
		}
	}

	// Living chrs and players near any physical body (awake or asleep).
	// With Ragdoll.RagdollChrPush off, walking over bodies doesn't touch them
	// (only a body flying at someone hits them).
	pdphysSetLivingPassThrough(!g_RagdollChrPush);
	pdphysSetSlopeGrip(g_RagdollSlopeGrip);

	{
		s32 numslots = g_NumChrSlots;

		for (i = 0; i < numslots + PLAYERCOUNT(); i++) {
			struct chrdata *chr;
			struct prop *prop;
			f32 ground;
			f32 height;
			f32 radius;
			bool near = false;

			if (i < numslots) {
				chr = &g_ChrSlots[i];
				prop = chr->prop;

				if (chr->chrnum < 0 || prop == NULL || prop->type != PROPTYPE_CHR
						|| chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD
						|| chr->actiontype == ACT_DRUGGEDDROP || chr->actiontype == ACT_DRUGGEDKO
						|| (chr->chrflags & CHRCFLAG_HIDDEN) || ragdollFindByChr(chr)) {
					continue;
				}

				ground = chr->manground;
				height = chr->height > 30.0f ? chr->height : 160.0f;
				radius = chr->radius > 5.0f ? chr->radius * 0.8f : 20.0f;
			} else {
				struct player *player = g_Vars.players[i - numslots];

				if (player == NULL || player->prop == NULL || player->isdead) {
					continue;
				}

				prop = player->prop;
				ground = player->vv_manground;
				height = player->vv_headheight > 30.0f ? player->vv_headheight : 170.0f;
				radius = 20.0f;
			}

			for (j = 0; j < RD_MAXRAGDOLLS; j++) {
				struct ragdoll *rd = &g_Ragdolls[j];

				if (rd->state == RDSTATE_SIMULATING) {
					s32 k;

					// The body, and (MOD: ragdoll physics) each cut piece:
					// a head or arm thrown at someone far from the rest of
					// the body went through them, and didn't stagger them
					for (k = 0; k < rd->numparts && !near; k++) {
						const f32 *at;
						f32 dx, dy, dz;

						if (k == 0) {
							at = rd->prop->pos.f;
						} else if (rd->severed & (1u << k)) {
							at = rd->world[k].m[3];
						} else {
							continue;
						}

						dx = at[0] - prop->pos.x;
						dz = at[2] - prop->pos.z;
						dy = at[1] - ground;

						near = dx * dx + dz * dz < 160.0f * 160.0f && dy > -60.0f && dy < height + 60.0f;
					}

					if (near) {
						break;
					}
				}
			}

			if (near) {
				ragdollSyncLivingChr(prop, ground, height, radius, g_RagdollChrPush ? dt : 0.0f);
				any = true;
			}
		}
	}

	// Drop props no ragdoll is near any more
	if (g_RagdollNumProps > 0) {
		for (i = 0; i < RD_MAXPROPS; i++) {
			if (g_RagdollProps[i].prop && (!any || !g_RagdollProps[i].seen)) {
				ragdollPropRelease(&g_RagdollProps[i]);
			}
		}
	}
}

/**
 * Lifts. Bodies on a lift ride it with physics: the lift is a kinematic body
 * that carries them (it's kept in the physics world even while they sleep,
 * see ragdollSyncProps, so it never appears under them mid-ride). A moving
 * lift wakes the bodies on it (ragdollWakeNear) and marks them (liftnear60)
 * so the floor pressing on them doesn't count as falls (no thuds/splats).
 * (Freezing riders and moving them with the lift was tried and looked broken:
 * bodies that weren't quite at rest got frozen mid-tumble.)
 */
static void ragdollCarryOnLift(struct prop *lift, const f32 *d, const f32 *dv, const f32 *vel)
{
	static struct propgeo pg;
	f32 lo[3] = { 1e9f, 1e9f, 1e9f };
	f32 hi[3] = { -1e9f, -1e9f, -1e9f };
	s32 i, p, k;

	if (!ragdollGetPropGeometry(lift, &pg)) {
		return;
	}

	for (p = 0; p < pg.numpieces; p++) {
		for (i = 0; i < pg.pieces[p].numverts; i++) {
			for (k = 0; k < 3; k++) {
				f32 c = pg.pieces[p].verts[i * 3 + k] - d[k];
				lo[k] = fminf(lo[k], c);
				hi[k] = fmaxf(hi[k], c);
			}
		}
	}

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];
		f32 *pelvis = rd->world[0].m[3];

		if (rd->fellout || rd->prop == NULL || rd->state == RDSTATE_FREE) {
			continue;
		}

		if (pelvis[0] < lo[0] - 30.0f || pelvis[0] > hi[0] + 30.0f
				|| pelvis[2] < lo[2] - 30.0f || pelvis[2] > hi[2] + 30.0f
				|| pelvis[1] < lo[1] - 60.0f || pelvis[1] > hi[1] + 160.0f) {
			continue;
		}

		rd->liftnear60 = g_Vars.lvframe60 > 0 ? g_Vars.lvframe60 : 1;
		rdVecCopy(vel, rd->liftvel);

		// Animation blend: the death animation plays on the lift, so its path
		// (and the floor it expects) moves with the lift
		if (rd->blending) {
			if (rd->bliftframe == 0) {
				sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d blend: carried by lift (prop %d), moving %.1f %.1f %.1f",
						RD_RDCHRNUM(rd), (s32)(lift - g_Vars.props), d[0], d[1], d[2]);
			}

			rd->liftoff[0] += d[0];
			rd->liftoff[1] += d[2];
			rd->offset[1] += d[1];
			rd->blendfloor += d[1];
			rd->bliftframe = g_Vars.lvframe60 > 0 ? g_Vars.lvframe60 : 1;
		}

		// The lift sped up or slowed down: so does everything on it, straight
		// away. Contacts alone let bodies lag behind a lift picking up speed
		// and land on it again (bouncing); this keeps their speed relative to
		// the lift, so the physics only sees the body lying still.
		if (dv && rd->state == RDSTATE_SIMULATING && rd->handle >= 0 && !rd->grabbed
				&& fabsf(dv[0]) + fabsf(dv[1]) + fabsf(dv[2]) > 0.01f) {
			pdphysRagdollAddVelocity(rd->handle, -1, dv);
		}
	}
}

/**
 * Sleeping and frozen bodies don't keep their surroundings in the physics
 * world (ragdollSyncProps skips them), so a lift starting to move, a table
 * the player pushes or a door swinging left them floating where the
 * support used to be. Every object and door that moved this tick wakes the
 * bodies near it; once awake, the sync puts the mover back in and the body
 * rides or falls with it.
 */
static void ragdollWakeNear(struct prop *mover)
{
	const f32 range = RD_PROPRANGE + 200.0f;
	f32 zero[3] = {0, 0, 0};
	s32 i;

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];
		f32 dx, dy, dz;

		if (rd->fellout || rd->prop == NULL || rd->riding == mover) {
			continue;
		}

		if (rd->state == RDSTATE_SIMULATING) {
			if (!(rd->settled && pdphysRagdollIsAsleep(rd->handle))) {
				continue; // awake: already synced with what's around it
			}
		} else if (rd->state != RDSTATE_FROZEN) {
			continue;
		}

		dx = rd->prop->pos.x - mover->pos.x;
		dy = rd->prop->pos.y - mover->pos.y;
		dz = rd->prop->pos.z - mover->pos.z;

		if (dx * dx + dz * dz > range * range || dy > range || dy < -range) {
			continue;
		}

		if (rd->state == RDSTATE_FROZEN) {
			if (!ragdollWakeFrozen(rd)) {
				continue;
			}
		} else {
			pdphysRagdollAddVelocity(rd->handle, -1, zero); // wakes every part
		}

		if (rd->propwake60 <= 0 || g_Vars.lvframe60 - rd->propwake60 > 300) {
			sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d woken by a moving %s (prop %d, obj type %d)",
					RD_RDCHRNUM(rd), mover->type == PROPTYPE_DOOR ? "door" : "object",
					(s32) (mover - g_Vars.props), mover->obj ? mover->obj->type : -1);
		}

		rd->propwake60 = g_Vars.lvframe60 > 0 ? g_Vars.lvframe60 : 1;
	}
}

void ragdollWakeOnMovedProps(void)
{
	RD_CRASHCTX("wakeonmovedprops", -1, -1);
	struct prop *prop;
	bool anysleeping = false;
	s32 i;

	if (g_Vars.props == NULL || g_Vars.maxprops <= 0) {
		return;
	}

	if (g_RdPropSnaps == NULL || g_RdNumPropSnaps != g_Vars.maxprops) {
		free(g_RdPropSnaps);
		g_RdNumPropSnaps = g_Vars.maxprops;
		g_RdPropSnaps = calloc(g_RdNumPropSnaps, sizeof(*g_RdPropSnaps));

		if (g_RdPropSnaps == NULL) {
			g_RdNumPropSnaps = 0;
			return;
		}
	}

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];

		if (!rd->fellout && (rd->state == RDSTATE_FROZEN
					|| (rd->state == RDSTATE_SIMULATING && rd->settled && pdphysRagdollIsAsleep(rd->handle)))) {
			anysleeping = true;
			break;
		}
	}

	// Snapshots are kept up to date even with no sleeping body, so a lift
	// that was already moving when a body falls asleep is noticed next tick
	for (prop = g_Vars.activeprops; prop && prop != g_Vars.pausedprops; prop = prop->next) {
		struct rdpropsnap *snap;
		struct defaultobj *obj;
		f32 cur[6];
		bool moved = false;
		s32 idx = prop - g_Vars.props;
		s32 k;

		if ((prop->type != PROPTYPE_OBJ && prop->type != PROPTYPE_DOOR) || prop->obj == NULL || prop->parent) {
			continue;
		}

		if (idx < 0 || idx >= g_RdNumPropSnaps) {
			continue;
		}

		obj = prop->obj;
		rdCoordToVec(&prop->pos, cur);
		cur[3] = obj->realrot[0][0];
		cur[4] = obj->realrot[0][2];
		cur[5] = prop->type == PROPTYPE_DOOR ? prop->door->frac : 0.0f;

		snap = &g_RdPropSnaps[idx];

		if (snap->valid && prop->type == PROPTYPE_OBJ && obj->type == OBJTYPE_LIFT) {
			f32 d[3] = { cur[0] - snap->v[0], cur[1] - snap->v[1], cur[2] - snap->v[2] };
			f32 dt = g_Vars.lvupdate60freal / 60.0f;
			f32 vel[3] = { 0, 0, 0 };
			f32 dv[3];

			if (dt > 0.0f) {
				for (k = 0; k < 3; k++) {
					vel[k] = d[k] / dt;
				}
			}

			for (k = 0; k < 3; k++) {
				dv[k] = vel[k] - snap->liftvel[k];
				snap->liftvel[k] = vel[k];
			}

			if (fabsf(d[0]) + fabsf(d[1]) + fabsf(d[2]) > 0.01f || fabsf(dv[0]) + fabsf(dv[1]) + fabsf(dv[2]) > 0.01f) {
				ragdollCarryOnLift(prop, d, dv, vel);
			}
		} else if (!snap->valid) {
			snap->liftvel[0] = snap->liftvel[1] = snap->liftvel[2] = 0.0f;
		}

		if (snap->valid) {
			moved = fabsf(cur[0] - snap->v[0]) + fabsf(cur[1] - snap->v[1]) + fabsf(cur[2] - snap->v[2]) > 0.5f
				|| fabsf(cur[3] - snap->v[3]) + fabsf(cur[4] - snap->v[4]) > 0.002f
				|| fabsf(cur[5] - snap->v[5]) > 0.001f;
		}

		for (k = 0; k < 6; k++) {
			snap->v[k] = cur[k];
		}

		snap->valid = true;

		if (moved && anysleeping) {
			ragdollWakeNear(prop);
		}
	}

}

/**
 * Bodies running into objects: pushable objects (chairs, crates, tables) are
 * shoved along, and glass a body hits hard enough (Ragdoll.RagdollGlassBreakSpeed)
 * shatters. Bulletproof glass (Maian SOS's medlab windows) and glass embedded
 * in the level are left to explosions, as in the game.
 */
#define RD_BODYPUSH_MINSPEED  60.0f  // slower contacts (a body resting on something) don't push
#define RD_BODYPUSH_TRANSFER  0.6f   // of the body's speed into the object it hits
#define RD_BODYPUSH_MAXSPEED  10.0f  // units per tick

void ragdollHandlePropImpacts(void)
{
	pdphysPropImpact impacts[64];
	s32 n = pdphysTakePropImpacts(impacts, 64);
	s32 i;

	for (i = 0; i < n; i++) {
		pdphysPropImpact *pi = &impacts[i];
		struct ragdollprop *rp = ragdollPropByHandle(pi->prop);
		struct prop *prop;
		struct defaultobj *obj;

		if (rp && (rp->prop->type == PROPTYPE_CHR || rp->prop->type == PROPTYPE_PLAYER)) {
			ragdollBodyHitsActor(rp, pi);
			continue;
		}

		if (rp == NULL || (prop = rp->prop) == NULL || prop->type != PROPTYPE_OBJ || (obj = prop->obj) == NULL) {
			continue;
		}

		if (rp->lastimpact60 != 0 && g_Vars.lvframe60 - rp->lastimpact60 < 6 && g_Vars.lvframe60 >= rp->lastimpact60) {
			continue;
		}

		if (obj->type == OBJTYPE_GLASS || obj->type == OBJTYPE_TINTEDGLASS) {
			if ((obj->flags & (OBJFLAG_INVINCIBLE | OBJFLAG_01000000))
					|| (obj->flags2 & (OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_BULLETPROOF))) {
				// Only explosions break this glass
				if (pi->speed >= g_RagdollGlassBreakSpeed && (rp->lastimpact60 == 0 || g_Vars.lvframe60 - rp->lastimpact60 >= 60)) {
					rp->lastimpact60 = RD_FRAMESTAMP();
					sysLogPrintf(LOG_NOTE, "RAGDOLL: a body hit glass (object %d) at %.0f u/s: bulletproof, holds", (s32) (prop - g_Vars.props), pi->speed);
				}
			} else if (pi->speed >= g_RagdollGlassBreakSpeed && (obj->hidden2 & OBJH2FLAG_DESTROYED) == 0) {
				struct coord pos;

				rdVecToCoord(pi->pos, &pos);
				rp->lastimpact60 = RD_FRAMESTAMP();
				sysLogPrintf(LOG_NOTE, "RAGDOLL: a body hit glass (object %d) at %.0f u/s: broken", (s32) (prop - g_Vars.props), pi->speed);
				objDamage(obj, 100.0f, &pos, WEAPON_NONE, g_Vars.currentplayernum);
			}

			continue;
		}

		if (pi->speed >= RD_BODYPUSH_MINSPEED && rdObjGrabbable(prop)
				&& !rdObjIsHeld(prop)) {
			f32 len = sqrtf(pi->dir[0] * pi->dir[0] + pi->dir[2] * pi->dir[2]);
			struct coord speed;
			f32 amount;

			if (len < 0.2f) {
				continue; // landing on top of it: no sideways push
			}

			amount = pi->speed * RD_BODYPUSH_TRANSFER / 60.0f * len;

			RD_CLAMPHI(amount, RD_BODYPUSH_MAXSPEED);

			speed.x = pi->dir[0] / len * amount;
			speed.y = 0.0f;
			speed.z = pi->dir[2] / len * amount;

			rp->lastimpact60 = RD_FRAMESTAMP();
			objApplyMomentum(obj, &speed, 0.0f, true, false);
		}
	}
}

/**
 * A door closing on a body: the door shoves the body along as it moves (it's
 * solid to bodies). A part it pushes stays clear of it; one whose centre ends
 * up inside the door is pinned (against the frame, a wall or the other door)
 * and can't be pushed, so the door stops there (the game's own blocking: it
 * keeps trying, and closes once the body is out of the way). Called by
 * doorsCalcFrac with the door's geometry where it's moving to.
 */
#define RD_DOORBLOCK_MARGIN 0.0f

// Is pos inside a piece of prop geometry (its outline from above and its
// height range), or within margin of it?
static bool rdPointInPiece(const pdphysPiece *piece, const f32 *pos, f32 margin)
{
	f32 hull[PDPHYS_MAX_PIECE_VERTS][2];
	f32 ymin = 1e9f, ymax = -1e9f;
	f32 reach = margin + piece->thickness * 0.5f;
	s32 n = piece->numverts;
	s32 numhull = 0;
	s32 start = 0;
	s32 cur, i;
	bool inside;

	if (n <= 0) {
		return false;
	}

	for (i = 0; i < n; i++) {
		const f32 *v = &piece->verts[i * 3];

		if (v[1] < ymin) {
			ymin = v[1];
		}

		if (v[1] > ymax) {
			ymax = v[1];
		}

		if (v[0] < piece->verts[start * 3] || (v[0] == piece->verts[start * 3] && v[2] < piece->verts[start * 3 + 2])) {
			start = i;
		}
	}

	if (pos[1] < ymin - reach || pos[1] > ymax + reach) {
		return false;
	}

	// Outline from above: gift wrapping
	cur = start;

	do {
		s32 next = (cur + 1) % n;

		hull[numhull][0] = piece->verts[cur * 3 + 0];
		hull[numhull][1] = piece->verts[cur * 3 + 2];
		numhull++;

		for (i = 0; i < n; i++) {
			f32 ax = piece->verts[next * 3 + 0] - piece->verts[cur * 3 + 0];
			f32 az = piece->verts[next * 3 + 2] - piece->verts[cur * 3 + 2];
			f32 bx = piece->verts[i * 3 + 0] - piece->verts[cur * 3 + 0];
			f32 bz = piece->verts[i * 3 + 2] - piece->verts[cur * 3 + 2];
			f32 cross = ax * bz - az * bx;

			if (cross < -0.001f || (cross <= 0.001f && bx * bx + bz * bz > ax * ax + az * az)) {
				next = i;
			}
		}

		cur = next;
	} while (cur != start && numhull < PDPHYS_MAX_PIECE_VERTS);

	// Inside (the hull winds one way: same side of every edge)?
	inside = numhull >= 3;

	for (i = 0; i < numhull && inside; i++) {
		f32 *a = hull[i];
		f32 *b = hull[(i + 1) % numhull];
		f32 cross = (b[0] - a[0]) * (pos[2] - a[1]) - (b[1] - a[1]) * (pos[0] - a[0]);

		if (cross < 0.0f) {
			inside = false;
		}
	}

	if (!inside) {
		// Maybe the other winding
		inside = numhull >= 3;

		for (i = 0; i < numhull && inside; i++) {
			f32 *a = hull[i];
			f32 *b = hull[(i + 1) % numhull];
			f32 cross = (b[0] - a[0]) * (pos[2] - a[1]) - (b[1] - a[1]) * (pos[0] - a[0]);

			if (cross > 0.0f) {
				inside = false;
			}
		}
	}

	if (inside) {
		return true;
	}

	// Close to its outline
	for (i = 0; i < numhull; i++) {
		f32 *a = hull[i];
		f32 *b = hull[(i + 1) % numhull];
		f32 ex = b[0] - a[0];
		f32 ez = b[1] - a[1];
		f32 l2 = ex * ex + ez * ez;
		f32 t = l2 > 0.0f ? ((pos[0] - a[0]) * ex + (pos[2] - a[1]) * ez) / l2 : 0.0f;
		f32 dx, dz;

		if (t < 0.0f) {
			t = 0.0f;
		} else if (t > 1.0f) {
			t = 1.0f;
		}

		dx = pos[0] - (a[0] + ex * t);
		dz = pos[2] - (a[1] + ez * t);

		if (dx * dx + dz * dz <= reach * reach) {
			return true;
		}
	}

	return false;
}

s32 ragdollBlocksDoor(struct doorobj *door)
{
	static struct propgeo pg;
	struct prop *prop;
	s32 i, p;

	if (!g_RagdollsEnabled || door == NULL || (prop = door->base.prop) == NULL) {
		return false;
	}

	if (!ragdollGetPropGeometry(prop, &pg)) {
		return false;
	}

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];
		f32 dx, dz;

		if (rd->state == RDSTATE_FREE || rd->prop == NULL || rd->fellout) {
			continue;
		}

		dx = rd->world[0].m[3][0] - prop->pos.x;
		dz = rd->world[0].m[3][2] - prop->pos.z;

		if (dx * dx + dz * dz > 600.0f * 600.0f) {
			continue;
		}

		for (p = 0; p < rd->numparts; p++) {
			// Points along the part's bone (its joint, the middle, the next
			// joint, and a hand or foot hanging off it): testing only the
			// joint missed a shin or forearm pinned against the frame
			f32 pts[4][3];
			s32 npts = 0;
			s32 c = rd->skel->child[p];
			s32 k, q;

			if (rd->bodyindex[p] < 0) {
				continue;
			}

			rdVecCopy(rd->world[p].m[3], pts[npts++]);

			if (c >= 0 && c < rd->numparts) {
				for (k = 0; k < 3; k++) {
					pts[npts][k] = (rd->world[p].m[3][k] + rd->world[c].m[3][k]) * 0.5f;
				}

				npts++;
				rdVecCopy(rd->world[c].m[3], pts[npts++]);

				// A hand or foot (no body of its own, moves with this part)
				if (!rd->skel->hasbody[c] && rd->skel->human && rd->torso > 0.0f) {
					if (p == 11 || p == 12) {
						// The foot: forward from the ankle along its own Z
						for (k = 0; k < 3; k++) {
							pts[npts][k] = rd->world[c].m[3][k] + rd->world[c].m[2][k] * rd->torso * 0.30f
								- rd->world[c].m[1][k] * rd->torso * 0.10f;
						}
					} else {
						// The hand: on along the forearm
						f32 bone[3];
						f32 len;

						rdVecSub(rd->world[c].m[3], rd->world[p].m[3], bone);
						len = rdVecLen(bone);

						if (len < 0.01f) {
							len = 0.01f; // (degenerate bone: the point is just the wrist)
						}

						for (k = 0; k < 3; k++) {
							pts[npts][k] = rd->world[c].m[3][k] + bone[k] / len * rd->torso * 0.18f;
						}
					}

					npts++;
				}
			}

			for (q = 0; q < npts; q++) {
				for (k = 0; k < pg.numpieces; k++) {
					if (rdPointInPiece(&pg.pieces[k], pts[q], RD_DOORBLOCK_MARGIN)) {
						static s32 lastlog60 = -1000;

						if (g_Vars.lvframe60 - lastlog60 > 120 || g_Vars.lvframe60 < lastlog60) {
							lastlog60 = g_Vars.lvframe60;
							sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d is in the way of door %d (part %d%s): it can't close",
									RD_RDCHRNUM(rd), (s32) (prop - g_Vars.props), p,
									q == 0 ? "" : q == 1 ? ", middle" : q == 2 ? ", far end" : ", hand/foot");
						}

						return true;
					}
				}
			}
		}
	}

	return false;
}

/**
 * doorsCalcFrac, each time a body blocks a door (the group's main door): a
 * door kept from closing for RD_DOORSTUCK_TIME opens again, and its
 * auto-close tries again later.
 */
#define RD_DOORSTUCK_TIME  120 // 60 Hz frames (2 s)
#define RD_DOORSTUCK_GAP   15  // longer than this without a block: a new block
#define RD_DOORSTUCK_SLOTS 16

static struct {
	struct doorobj *door;
	s32 since60; // first frame of this block
	s32 last60;  // last frame it was blocked
} g_RdDoorStuck[RD_DOORSTUCK_SLOTS];

s32 ragdollDoorStuckOnBody(struct doorobj *door)
{
	s32 now = RD_FRAMESTAMP();
	s32 slot = -1;
	s32 freeslot = -1;
	s32 i;

	if (!g_RagdollsEnabled || door == NULL) {
		return false;
	}

	for (i = 0; i < RD_DOORSTUCK_SLOTS; i++) {
		// Stale (a new level restarts the frame count, or not blocked lately)
		if (g_RdDoorStuck[i].door
				&& (g_RdDoorStuck[i].last60 > now || now - g_RdDoorStuck[i].last60 > RD_DOORSTUCK_GAP)) {
			g_RdDoorStuck[i].door = NULL;
		}

		if (g_RdDoorStuck[i].door == door) {
			slot = i;
		} else if (g_RdDoorStuck[i].door == NULL && freeslot < 0) {
			freeslot = i;
		}
	}

	if (slot < 0) {
		if (freeslot < 0) {
			return false;
		}

		slot = freeslot;
		g_RdDoorStuck[slot].door = door;
		g_RdDoorStuck[slot].since60 = now;
	}

	g_RdDoorStuck[slot].last60 = now;

	if (now - g_RdDoorStuck[slot].since60 < RD_DOORSTUCK_TIME) {
		return false;
	}

	g_RdDoorStuck[slot].door = NULL;

	sysLogPrintf(LOG_NOTE, "RAGDOLL: door %d held open by a body for %.1f s: opening it again",
			door->base.prop ? (s32) (door->base.prop - g_Vars.props) : -1, RD_DOORSTUCK_TIME / 60.0f);

	return true;
}

#endif // PD_PHYSICS
