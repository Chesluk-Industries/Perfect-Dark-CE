/**
 * MOD: ragdoll physics - dismemberment: cutting parts off, the display list
 * seams, the caps over the cuts, blood. Part of the ragdoll module
 * (port/src/ragdoll.c).
 */

#include "rd_internal.h"

#ifdef PD_PHYSICS

struct rdvolley g_RdVolley;

/* ---- dismemberment ---- */


#define RD_CUTCOLOUR_LIFT 1.35f // cap colour = blood droplet colour x this

/**
 * Next node in a walk of a chr's model tree.
 *
 * Chrs using the same body share one model definition, and the game re-links
 * its HEADSPOT (which head), DISTANCE (detail level) and TOGGLE nodes for
 * each chr as it walks it. Following node->child without doing the same
 * walks into whichever head was drawn last - possibly another chr's - and
 * reads this chr's head data with that head's indexes (crash). So do what
 * the game does, and don't go into a headspot that has no head.
 */
struct modelnode *ragdollWalkNext(struct model *model, struct modelnode *node)
{
	bool descend = true;

	switch (node->type & 0xff) {
	case MODELNODETYPE_DISTANCE:
		modelApplyDistanceRelations(model, node);
		break;
	case MODELNODETYPE_TOGGLE:
		modelApplyToggleRelations(model, node);
		break;
	case MODELNODETYPE_HEADSPOT:
		{
			struct modelrwdata_headspot *rw = modelGetNodeRwData(model, node);

			if (rw && rw->headmodeldef) {
				modelApplyHeadRelations(model, node);
			} else {
				descend = false;
			}
		}
		break;
	}

	if (descend && node->child) {
		return node->child;
	}

	while (node) {
		if (node->next) {
			return node->next;
		}

		node = node->parent;
	}

	return NULL;
}

// Link the whole tree for this chr (before modelFindNodeByMtxIndex etc.)
void ragdollApplyRelations(struct model *model)
{
	struct modelnode *node = model->definition->rootnode;

	while (node) {
		node = ragdollWalkNext(model, node);
	}
}

// A DL node's list: the modeldef's own is a segmented address relative to its
// colour table (see chr.c's disfigurement code)
static Gfx *rdResolveGdl(const struct modelrodata_dl *rodata, Gfx *gdl)
{
	if (((uintptr_t) gdl & 1) || gdl == rodata->opagdl) {
		return (Gfx *)((uintptr_t) rodata->colours + ((uintptr_t) UNSEGADDR(gdl) & 0xffffff));
	}

	return gdl;
}

// The skeleton part a node is drawn with (nodes that aren't parts belong to the part above them)
static s32 ragdollPartOfNode(struct ragdoll *rd, struct modelnode *node)
{
	while (node) {
		u32 type = node->type & 0xff;

		if (type == MODELNODETYPE_CHRINFO) {
			return 0;
		}

		if (type == MODELNODETYPE_POSITION) {
			s32 part = node->rodata->position.part;

			if (rd->skel->human) {
				return part >= 0 && part < rd->numparts ? part : -1;
			}

			part = rdPartOfNode(rd->skel, part);

			if (part > 0) {
				return part;
			}
		}

		node = node->parent;
	}

	return -1;
}

// The piece a part ended up on: its nearest cut ancestor (or itself), 0 = main body
s32 ragdollPieceOf(struct ragdoll *rd, s32 part)
{
	while (part > 0) {
		if (rd->severed & (1u << part)) {
			return part;
		}

		part = rd->skel->parent[part];
	}

	return 0;
}

static struct modelnode *ragdollPartNode(struct model *model, s32 mtxindex)
{
	return modelFindNodeByMtxIndex(model, mtxindex);
}

/**
 * Puts the model's display lists back the way they were (shared modeldef
 * data / the vtxstore copies the game made) and frees our copies.
 */
void ragdollRestoreSeams(struct ragdoll *rd)
{
	RD_CRASHCTX("restoreseams", RD_RDCHRNUM(rd), -1);
	s32 i;

	for (i = 0; i < rd->numpatches; i++) {
		struct rddlpatch *p = &rd->patches[i];

		if (p->rwdata->gdl == p->gdl) {
			p->rwdata->gdl = p->origgdl;
		}

		if (p->rwdata->vertices == p->vtx) {
			p->rwdata->vertices = p->origvtx;
		}

		free(p->gdl);
		free(p->vtx);
	}

	free(rd->patches);
	rd->patches = NULL;
	rd->numpatches = 0;
}

/**
 * PD's chr meshes are stitched at the joints: some vertices of a body part's
 * display list are drawn with the neighbouring part's matrix, so elbows and
 * knees bend smoothly. Once a joint is cut, those vertices would stretch all
 * the way to the other piece. This gives every display list that references
 * a matrix on another piece its own copy, where such matrix loads are
 * replaced by the list's own part matrix and the affected vertices are moved
 * into that part's space (at their rest position), so each piece keeps its
 * own end of the seam. The open ends are covered by the red caps.
 */
static void ragdollRebuildSeams(struct ragdoll *rd)
{
	RD_CRASHCTX("rebuildseams", RD_RDCHRNUM(rd), -1);
	struct model *model = rd->model;
	struct modelnode *node;
	s32 maxpatches = 64;
	s32 patched = 0;
	s32 skipped = 0;

	ragdollRestoreSeams(rd);

	if (rd->severed == 0) {
		return;
	}

	rd->patches = calloc(maxpatches, sizeof(struct rddlpatch));

	if (!rd->patches) {
		return;
	}

	ragdollApplyRelations(model);
	node = model->definition->rootnode;

	while (node) {
		if ((node->type & 0xff) == MODELNODETYPE_DL) {
			struct modelrodata_dl *rodata = &node->rodata->dl;
			struct modelrwdata_dl *rwdata = (struct modelrwdata_dl *) modelGetNodeRwData(model, node);
			s32 owner = ragdollPartOfNode(rd, node->parent);
			struct modelnode *ownernode = owner >= 0 ? ragdollPartNode(model, rd->mtxindex[owner]) : NULL;

			if (rwdata && rwdata->gdl && rwdata->vertices && owner >= 0 && ownernode) {
				s32 ownerpiece = ragdollPieceOf(rd, owner);
				bool need = false;
				bool bad = false;
				bool ended = false;
				s32 n = 0;
				Gfx *g;
				Gfx *src = rwdata->gdl;

				src = rdResolveGdl(rodata, src);

				// Pass 1: does this list touch another piece? (and how long is it)
				for (g = src; n < 8192; g++, n++) {
					s32 op = (s8) g->bytes[GFX_W0_BYTE(0)];

					if (op == (s8) G_ENDDL) {
						n++;
						ended = true;
						break;
					}

					if (op == (s8) G_DL) {
						bad = true;
					} else if (op == (s8) G_MTX) {
						s32 idx = (s32)((UNSEGADDR(g->words.w1) & 0xffffff) / sizeof(Mtxf));
						s32 mpart = ragdollPartOfNode(rd, modelFindNodeByMtxIndex(model, idx));

						if (mpart >= 0) {
							s32 mpiece = ragdollPieceOf(rd, mpart);

							if (mpiece != ownerpiece || (mpiece == mpart && mpart != 0 && idx != rd->mtxindex[mpart])) {
								need = true;
							}
						}
					}
				}

				if (need && (bad || !ended)) {
					skipped++;
				} else if (need && rd->numpatches < maxpatches) {
					struct rddlpatch *p = &rd->patches[rd->numpatches];
					s32 numverts = rodata->numvertices;
					u8 *moved = calloc(numverts > 0 ? numverts : 1, 1);
					struct coord ownerpos;
					s32 delta[3] = {0, 0, 0};
					s32 i;

					p->rwdata = rwdata;
					p->origgdl = rwdata->gdl;
					p->origvtx = rwdata->vertices;
					p->gdl = malloc(n * sizeof(Gfx));
					p->vtx = malloc((numverts > 0 ? numverts : 1) * sizeof(Vtx));

					if (!p->gdl || !p->vtx || !moved) {
						free(p->gdl);
						free(p->vtx);
						free(moved);
					} else {
						memcpy(p->gdl, src, n * sizeof(Gfx));
						memcpy(p->vtx, rwdata->vertices, numverts * sizeof(Vtx));
						modelNodeGetModelRelativePosition(model, ownernode, &ownerpos);

						// Pass 2: retarget matrix loads and move their vertices
						for (i = 0; i < n; i++) {
							Gfx *c = &p->gdl[i];
							s32 op = (s8) c->bytes[GFX_W0_BYTE(0)];

							if (op == (s8) G_MTX) {
								uintptr_t w1 = c->words.w1;
								s32 idx = (s32)((UNSEGADDR(w1) & 0xffffff) / sizeof(Mtxf));
								struct modelnode *mnode = modelFindNodeByMtxIndex(model, idx);
								s32 mpart = ragdollPartOfNode(rd, mnode);
								s32 newidx = -1;

								delta[0] = delta[1] = delta[2] = 0;

								if (mpart >= 0) {
									s32 mpiece = ragdollPieceOf(rd, mpart);

									if (mpiece != ownerpiece) {
										// Another piece: use our own part's matrix
										struct coord mpos;

										modelNodeGetModelRelativePosition(model, mnode, &mpos);
										rdIVecSet(delta, (s32) floorf(mpos.x - ownerpos.x + 0.5f), (s32) floorf(mpos.y - ownerpos.y + 0.5f), (s32) floorf(mpos.z - ownerpos.z + 0.5f));
										newidx = rd->mtxindex[owner];
									} else if (mpiece == mpart && mpart != 0 && idx != rd->mtxindex[mpart]) {
										// The cut part's own half-bend matrices: straighten
										newidx = rd->mtxindex[mpart];
									}
								}

								if (newidx >= 0) {
									uintptr_t raw = UNSEGADDR(w1);
									raw = (raw & ~(uintptr_t) 0xffffff) | (uintptr_t)(newidx * sizeof(Mtxf));
									c->words.w1 = raw | (w1 & 1);
								}
							} else if (op == (s8) G_VTX && (delta[0] || delta[1] || delta[2])) {
								u8 *ptr = (u8 *) &c->words.w0;
								s32 start = (s32)((UNSEGADDR(c->words.w1) & 0xffffff) / sizeof(Vtx));
								s32 count = (u32) ptr[GFX_W0_BYTE(1)] / 16 + 1;
								s32 v;

								for (v = start; v < start + count && v < numverts; v++) {
									if (v >= 0 && !moved[v]) {
										moved[v] = 1;
										p->vtx[v].x += delta[0];
										p->vtx[v].y += delta[1];
										p->vtx[v].z += delta[2];
									}
								}
							}
						}

						free(moved);
						rwdata->gdl = p->gdl;
						rwdata->vertices = p->vtx;
						rd->numpatches++;
						patched++;
					}
				}
			}
		}

		node = ragdollWalkNext(model, node);
	}

	if (skipped) {
		sysLogPrintf(LOG_WARNING, "RAGDOLL: chr %d: %d display lists couldn't be separated at the cut", rd->chr->chrnum, skipped);
	}
}

#define RD_WELD 2.0f     // model units (grid cell)
#define RD_WELD_TOL 1.5f // model units: vertices this close are the same

// A mesh vertex as drawn: the matrix it's loaded with and its coordinates
struct rdmvtx {
	s32 id;      // welded rest position, -1 = none
	s16 mtx;
	s16 v[3];
};

struct rdmedge {
	s32 a, b;    // welded ids, a < b
	s32 piece;
	s32 owner;   // the part whose display list it's in
	s32 joint;   // non-human: an open end of its part's mesh, paired with the joint it's at (-1 = none)
	struct rdmvtx va, vb;
};

struct rdmeshbuild {
	s32 (*cell)[3];   // welded rest positions: grid cell
	f32 (*fpos)[3];   // ... and actual position (model units)
	s32 numverts;
	s32 maxverts;
	s32 *hash;
	s32 hashsize;
	struct rdmedge *edges;
	s32 numedges;
	s32 maxedges;
};

static u32 rdCellHash(const s32 *q, s32 size)
{
	return ((u32) q[0] * 73856093u ^ (u32) q[1] * 19349663u ^ (u32) q[2] * 83492791u) & (size - 1);
}

// Welds positions within RD_WELD_TOL of each other (checks neighbouring
// cells too, so nearly equal values on either side of a cell border merge)
static s32 rdMeshVertex(struct rdmeshbuild *mb, f32 x, f32 y, f32 z)
{
	s32 q[3];
	s32 dx, dy, dz;
	u32 h;
	s32 i;

	rdIVecSet(q, (s32) floorf(x / RD_WELD), (s32) floorf(y / RD_WELD), (s32) floorf(z / RD_WELD));

	for (dx = -1; dx <= 1; dx++) {
		for (dy = -1; dy <= 1; dy++) {
			for (dz = -1; dz <= 1; dz++) {
				s32 c[3] = { q[0] + dx, q[1] + dy, q[2] + dz };

				h = rdCellHash(c, mb->hashsize);

				for (i = 0; i < mb->hashsize; i++) {
					s32 slot = mb->hash[(h + i) & (mb->hashsize - 1)];

					if (slot < 0) {
						break;
					}

					if (mb->cell[slot][0] == c[0] && mb->cell[slot][1] == c[1] && mb->cell[slot][2] == c[2]
							&& fabsf(mb->fpos[slot][0] - x) <= RD_WELD_TOL
							&& fabsf(mb->fpos[slot][1] - y) <= RD_WELD_TOL
							&& fabsf(mb->fpos[slot][2] - z) <= RD_WELD_TOL) {
						return slot;
					}
				}
			}
		}
	}

	if (mb->numverts >= mb->maxverts) {
		return -1;
	}

	h = rdCellHash(q, mb->hashsize);

	for (i = 0; i < mb->hashsize; i++) {
		if (mb->hash[(h + i) & (mb->hashsize - 1)] < 0) {
			break;
		}
	}

	if (i >= mb->hashsize) {
		return -1;
	}

	rdIVecCopy(q, mb->cell[mb->numverts]);
	rdVecSet(mb->fpos[mb->numverts], x, y, z);
	mb->hash[(h + i) & (mb->hashsize - 1)] = mb->numverts;

	return mb->numverts++;
}

static void rdMeshEdge(struct rdmeshbuild *mb, struct rdmvtx *x, struct rdmvtx *y, s32 piece, s32 owner)
{
	struct rdmedge *e;

	if (x->id < 0 || y->id < 0 || x->id == y->id || mb->numedges >= mb->maxedges) {
		return;
	}

	e = &mb->edges[mb->numedges++];
	e->piece = piece;
	e->owner = owner;
	e->joint = -1;

	if (x->id < y->id) {
		e->a = x->id; e->b = y->id; e->va = *x; e->vb = *y;
	} else {
		e->a = y->id; e->b = x->id; e->va = *y; e->vb = *x;
	}
}

static int rdEdgeCmp(const void *x, const void *y)
{
	const struct rdmedge *a = x;
	const struct rdmedge *b = y;

	if (a->a != b->a) return a->a - b->a;
	if (a->b != b->b) return a->b - b->b;
	return a->piece - b->piece;
}

static void ragdollRelPos(struct ragdoll *rd, s32 part, f32 *out);

static s32 rdFind(s32 *uf, s32 x)
{
	while (uf[x] != x) {
		uf[x] = uf[uf[x]];
		x = uf[x];
	}

	return x;
}

void ragdollFreeCaps(struct ragdoll *rd)
{
	RD_CRASHCTX("freecaps", RD_RDCHRNUM(rd), -1);
	s32 i;

	for (i = 0; i < rd->numcaps; i++) {
		free(rd->caps[i].edges);
		rd->caps[i].edges = NULL;
	}

	rd->numcaps = 0;
}

static void ragdollRelPos(struct ragdoll *rd, s32 part, f32 *out)
{
	struct modelnode *node = modelFindNodeByMtxIndex(rd->model, rd->mtxindex[part]);
	struct coord c = {0, 0, 0};

	if (node) {
		modelNodeGetModelRelativePosition(rd->model, node, &c);
	}

	rdCoordToVec(&c, out);
}

/**
 * How thick the body is where 'part' is cut off (units). Human: a share of
 * the torso length per joint. Others: a bit thicker than the part's shape.
 */
static f32 rdCutRadius(struct ragdoll *rd, s32 part)
{
	if (rd->skel->human) {
		return rd->skel->cutradius[part] * rd->torso;
	}

	return rd->skel->cutradius[part] * (rd->partradius[part] > 0.0f ? rd->partradius[part] : rd->torso * 0.2f) * 1.15f;
}

// Fallback when the mesh gives no border: a small disc at the joint
static void ragdollAddDiscCap(struct ragdoll *rd, s32 cut, bool childside)
{
	RD_CRASHCTX("adddisccap", RD_RDCHRNUM(rd), cut);
	struct rdcap *cap;
	f32 pc[3], pp[3], n[3];
	f32 u[3], v[3];
	f32 r = rdCutRadius(rd, cut) * 0.7f / rd->scale;
	s32 parent = rd->skel->parent[cut];
	s32 frame = childside ? cut : parent;
	f32 pf[3];
	s32 k;

	if (rd->numcaps >= RD_MAXCAPS) {
		return;
	}

	ragdollRelPos(rd, cut, pc);
	ragdollRelPos(rd, parent, pp);
	ragdollRelPos(rd, frame, pf);

	for (k = 0; k < 3; k++) {
		n[k] = childside ? pp[k] - pc[k] : pc[k] - pp[k];
	}

	rdVecNorm(n);

	cap = &rd->caps[rd->numcaps];
	cap->edges = malloc(8 * sizeof(*cap->edges));

	if (!cap->edges) {
		return;
	}

	rd->numcaps++;
	rd->numdisccaps++;
	cap->part = frame;
	cap->numedges = 8;

	for (k = 0; k < 3; k++) {
		cap->centre[k] = pc[k] - pf[k];
		cap->normal[k] = n[k];
	}

	if (fabsf(n[1]) < 0.9f) {
		u[0] = n[2]; u[1] = 0.0f; u[2] = -n[0];
	} else {
		u[0] = 0.0f; u[1] = -n[2]; u[2] = n[1];
	}

	rdVecNorm(u);
	rdVecCross(n, u, v);

	for (k = 0; k < 8; k++) {
		f32 a0 = k * (M_BADTAU / 8.0f);
		f32 a1 = (k + 1) * (M_BADTAU / 8.0f);
		s32 c;

		cap->edges[k][0].mtx = cap->edges[k][1].mtx = rd->mtxindex[frame];

		for (c = 0; c < 3; c++) {
			cap->edges[k][0].v[c] = (s16)(cap->centre[c] + (u[c] * cosf(a0) + v[c] * sinf(a0)) * r);
			cap->edges[k][1].v[c] = (s16)(cap->centre[c] + (u[c] * cosf(a1) + v[c] * sinf(a1)) * r);
		}
	}
}

/*
 * Cut caps built from the mesh itself. Every triangle of the model is
 * gathered with its rest-pose positions (welded) and the matrix/coordinates
 * each vertex is drawn with. For each piece, the border of a hole is made of
 * edges used once on that piece that either were shared with a triangle now
 * on another piece, or are open edges right at the cut (where the original
 * mesh overlapped instead of being stitched). Each border loop is closed with
 * a fan from its centre whose rim vertices use exactly the same matrices and
 * coordinates as the mesh, so the cap follows the edge of the hole.
 */
static void ragdollBuildCaps(struct ragdoll *rd)
{
	RD_CRASHCTX("buildcaps", RD_RDCHRNUM(rd), -1);
	struct model *model = rd->model;
	struct modelnode *node;
	struct rdmeshbuild mb;
	s32 *uf = NULL;
	bool covered[RD_NUMPARTS][2];
	f32 cutpos[RD_NUMPARTS][3];
	f32 cutreach[RD_NUMPARTS];
	s32 i;

	ragdollFreeCaps(rd);
	rd->numdisccaps = 0;
	memset(covered, 0, sizeof(covered));

	if (rd->severed == 0) {
		return;
	}

	for (i = 1; i < rd->numparts; i++) {
		if (rd->severed & (1u << i)) {
			ragdollRelPos(rd, i, cutpos[i]);
			cutreach[i] = rdCutRadius(rd, i) * 1.6f / rd->scale;
		}
	}

	memset(&mb, 0, sizeof(mb));
	mb.maxverts = 8192;
	mb.hashsize = 16384;
	mb.maxedges = 3 * 8192;
	mb.cell = malloc(mb.maxverts * sizeof(*mb.cell));
	mb.fpos = malloc(mb.maxverts * sizeof(*mb.fpos));
	mb.hash = malloc(mb.hashsize * sizeof(s32));
	mb.edges = malloc(mb.maxedges * sizeof(*mb.edges));

	if (!mb.cell || !mb.fpos || !mb.hash || !mb.edges) {
		goto done;
	}

	for (i = 0; i < mb.hashsize; i++) {
		mb.hash[i] = -1;
	}

	ragdollApplyRelations(model);
	node = model->definition->rootnode;

	while (node) {
		if ((node->type & 0xff) == MODELNODETYPE_DL) {
			struct modelrodata_dl *rodata = &node->rodata->dl;
			struct modelrwdata_dl *rwdata = (struct modelrwdata_dl *) modelGetNodeRwData(model, node);
			s32 owner = ragdollPartOfNode(rd, node->parent);

			if (rwdata && rwdata->gdl && rwdata->vertices && owner >= 0) {
				s32 piece = ragdollPieceOf(rd, owner);
				Gfx *g = rwdata->gdl;
				struct rdmvtx slots[16];
				f32 mpos[3] = {0, 0, 0};
				s32 curmtx = -1;
				s32 n;

				g = rdResolveGdl(rodata, g);

				for (i = 0; i < 16; i++) {
					slots[i].id = -1;
				}

				for (n = 0; n < 8192; n++, g++) {
					s32 op = (s8) g->bytes[GFX_W0_BYTE(0)];

					if (op == (s8) G_ENDDL || op == (s8) G_DL) {
						break;
					}

					if (op == (s8) G_MTX) {
						s32 idx = (s32)((UNSEGADDR(g->words.w1) & 0xffffff) / sizeof(Mtxf));
						struct modelnode *mnode = modelFindNodeByMtxIndex(model, idx);
						struct coord c = {0, 0, 0};

						if (mnode) {
							modelNodeGetModelRelativePosition(model, mnode, &c);
						}

						curmtx = idx;
						rdCoordToVec(&c, mpos);
					} else if (op == (s8) G_VTX) {
						u8 *ptr = (u8 *) &g->words.w0;
						s32 start = (s32)((UNSEGADDR(g->words.w1) & 0xffffff) / sizeof(Vtx));
						s32 count = (u32) ptr[GFX_W0_BYTE(1)] / 16 + 1;
						s32 v0 = ptr[GFX_W0_BYTE(1)] & 0xf;
						s32 k;

						for (k = 0; k < count && v0 + k < 16; k++) {
							s32 vi = start + k;
							struct rdmvtx *s = &slots[v0 + k];

							if (vi >= 0 && vi < rodata->numvertices && curmtx >= 0) {
								Vtx *vtx = &rwdata->vertices[vi];

								s->id = rdMeshVertex(&mb, vtx->x + mpos[0], vtx->y + mpos[1], vtx->z + mpos[2]);
								s->mtx = curmtx;
								s->v[0] = vtx->x;
								s->v[1] = vtx->y;
								s->v[2] = vtx->z;
							} else {
								s->id = -1;
							}
						}
					} else if (op == (s8) G_TRI4) {
						u32 w0 = (u32) g->words.w0;
						u32 w1 = (u32) g->words.w1;
						s32 t;

						for (t = 0; t < 4; t++) {
							s32 x = (w1 >> (t * 8)) & 0xf;
							s32 y = (w1 >> (t * 8 + 4)) & 0xf;
							s32 z = (w0 >> (t * 4)) & 0xf;

							if (x == 0 && y == 0 && z == 0) {
								continue;
							}

							rdMeshEdge(&mb, &slots[x], &slots[y], piece, owner);
							rdMeshEdge(&mb, &slots[y], &slots[z], piece, owner);
							rdMeshEdge(&mb, &slots[z], &slots[x], piece, owner);
						}
					} else if (op == (s8) G_TRI1) {
						u32 w1 = (u32) g->words.w1;
						s32 x = ((w1 >> 16) & 0xff) / 10;
						s32 y = ((w1 >> 8) & 0xff) / 10;
						s32 z = (w1 & 0xff) / 10;

						if (x < 16 && y < 16 && z < 16) {
							rdMeshEdge(&mb, &slots[x], &slots[y], piece, owner);
							rdMeshEdge(&mb, &slots[y], &slots[z], piece, owner);
							rdMeshEdge(&mb, &slots[z], &slots[x], piece, owner);
						}
					}
				}
			}
		}

		node = ragdollWalkNext(model, node);
	}

	qsort(mb.edges, mb.numedges, sizeof(*mb.edges), rdEdgeCmp);
	uf = malloc(mb.numverts * sizeof(s32) + 4);

	if (!uf) {
		goto done;
	}

	/*
	 * Non-human meshes aren't all stitched at the joints: the Skedar's tail
	 * is a separate tube pushed into a hole in the body. Cutting there
	 * leaves both meshes' own open ends, which no shared edge marks. So each
	 * part's mesh's open loops are found (edges in one of its triangles
	 * only), and each is paired with the nearest open loop of a part next to
	 * it (the tail's end and the hole it sits in): the pair's edges belong
	 * to the joint between those two parts, and are capped when that joint
	 * is cut, whichever pieces the parts are on by then.
	 */
	if (!rd->skel->human) {
		struct rdloop { s32 owner; s32 count; f32 c[3]; s32 joint; } loops[64];
		s32 numloops = 0;
		s32 *edgeloop = malloc(mb.numedges * sizeof(s32) + 4);
		s32 *rootloop = malloc(mb.numverts * sizeof(s32) + 4);
		s32 o;

		if (edgeloop && rootloop) {
			for (i = 0; i < mb.numedges; i++) {
				edgeloop[i] = -1;
			}

			for (o = 0; o < rd->numparts; o++) {
				s32 e;
				s32 first;

				for (i = 0; i < mb.numverts; i++) {
					uf[i] = i;
					rootloop[i] = -1;
				}

				// This part's open edges, joined into loops
				for (e = 0; e < mb.numedges; e = first) {
					s32 a = mb.edges[e].a;
					s32 b = mb.edges[e].b;
					s32 mine = 0;

					for (first = e; first < mb.numedges && mb.edges[first].a == a && mb.edges[first].b == b; first++) {
						mine += mb.edges[first].owner == o;
					}

					if (mine == 1) {
						uf[rdFind(uf, a)] = rdFind(uf, b);
					}
				}

				for (e = 0; e < mb.numedges; e = first) {
					s32 a = mb.edges[e].a;
					s32 b = mb.edges[e].b;
					s32 mine = 0;
					s32 which = -1;

					for (first = e; first < mb.numedges && mb.edges[first].a == a && mb.edges[first].b == b; first++) {
						if (mb.edges[first].owner == o) {
							mine++;
							which = first;
						}
					}

					if (mine == 1) {
						s32 root = rdFind(uf, a);
						s32 k;

						if (rootloop[root] < 0 && numloops < ARRAYCOUNT(loops)) {
							rootloop[root] = numloops;
							loops[numloops].owner = o;
							loops[numloops].count = 0;
							loops[numloops].c[0] = loops[numloops].c[1] = loops[numloops].c[2] = 0.0f;
							loops[numloops].joint = -1;
							numloops++;
						}

						if (rootloop[root] >= 0) {
							struct rdloop *lp = &loops[rootloop[root]];

							for (k = 0; k < 3; k++) {
								lp->c[k] += mb.fpos[a][k];
							}

							lp->count++;
							edgeloop[which] = rootloop[root];
						}
					}
				}
			}

			for (i = 0; i < numloops; i++) {
				s32 k;

				for (k = 0; k < 3; k++) {
					loops[i].c[k] /= loops[i].count;
				}
			}

			// Pair the loops up, one to one, nearest pairs first (the neck's
			// own seam pairs before the hole the tail sits in can take it):
			// each pair of loops of parts next to each other
			for (;;) {
				f32 best = 1e30f;
				s32 bi = -1, bm = -1;
				s32 m;

				for (i = 0; i < numloops; i++) {
					if (loops[i].count < 3 || loops[i].joint >= 0) {
						continue;
					}

					for (m = i + 1; m < numloops; m++) {
						s32 ol = loops[i].owner;
						s32 om = loops[m].owner;
						f32 d[3];

						if (loops[m].count < 3 || loops[m].joint >= 0) {
							continue;
						}

						if (!(om > 0 && rd->skel->parent[om] == ol) && !(ol > 0 && rd->skel->parent[ol] == om)) {
							continue; // not neighbours
						}

						rdVecSub(loops[m].c, loops[i].c, d);

						if (rdVecLen(d) < best) {
							best = rdVecLen(d);
							bi = i;
							bm = m;
						}
					}
				}

				if (bi < 0) {
					break;
				}

				loops[bi].joint = loops[bm].joint = (loops[bm].owner > 0 && rd->skel->parent[loops[bm].owner] == loops[bi].owner)
					? loops[bm].owner : loops[bi].owner;
			}

			for (i = 0; i < mb.numedges; i++) {
				if (edgeloop[i] >= 0) {
					mb.edges[i].joint = loops[edgeloop[i]].joint;
				}
			}
		}

		free(edgeloop);
		free(rootloop);
	}

	{
		s32 pieces[RD_NUMPARTS];
		s32 numpieces = 0;
		s32 p;

		pieces[numpieces++] = 0;

		for (p = 1; p < rd->numparts; p++) {
			if (rd->severed & (1u << p)) {
				pieces[numpieces++] = p;
			}
		}

		for (p = 0; p < numpieces; p++) {
			s32 piece = pieces[p];
			struct rdmedge **border = malloc(mb.numedges * sizeof(*border) + 8);
			s32 *bcut = malloc(mb.numedges * sizeof(s32) + 8); // the cut an open loop was taken for, -1 = nearest
			s32 numborder = 0;
			s32 e = 0;

			if (!border || !bcut) {
				free(border);
				free(bcut);
				continue;
			}


			while (e < mb.numedges) {
				s32 a = mb.edges[e].a;
				s32 b = mb.edges[e].b;
				struct rdmedge *mineedge = NULL;
				s32 mine = 0;
				s32 others = 0;

				while (e < mb.numedges && mb.edges[e].a == a && mb.edges[e].b == b) {
					if (mb.edges[e].piece == piece) {
						mine++;
						mineedge = &mb.edges[e];
					} else {
						others++;
					}

					e++;
				}

				if (mine == 1) {
					bool use = others > 0;

					s32 loopcut = -1;

					if (!rd->skel->human && mineedge->joint > 0) {
						// Non-human: an open end of a mesh paired (above) with
						// a joint: that cut's, if it's cut
						s32 j = mineedge->joint;

						if ((rd->severed & (1u << j))
								&& (ragdollPieceOf(rd, j) == piece || ragdollPieceOf(rd, rd->skel->parent[j]) == piece)) {
							use = true;
							loopcut = j;
						} else if (others == 0) {
							use = false;
						}
					}

					if (!use && rd->skel->human) {
						// Already-open edge: only if right at a cut on this piece's side
						s32 j;

						for (j = 1; j < rd->numparts && !use; j++) {
							if ((rd->severed & (1u << j))
									&& (ragdollPieceOf(rd, j) == piece || ragdollPieceOf(rd, rd->skel->parent[j]) == piece)) {
								s32 k;
								bool near = true;

								for (k = 0; k < 2; k++) {
									s32 id = k == 0 ? a : b;
									f32 d[3];

									rdVecSub(mb.fpos[id], cutpos[j], d);

									if (rdVecLen(d) > cutreach[j]) {
										near = false;
									}
								}

								use = near;
							}
						}
					}

					if (use) {
						bcut[numborder] = loopcut;
						border[numborder++] = mineedge;
					}
				}
			}

			// Each border edge belongs to the nearest cut on this piece's side,
			// so holes that touch (both hips on the pelvis) stay separate
			{
				s32 *bgroup = malloc(numborder * sizeof(s32) + 4);
				s32 g;

				if (!bgroup) {
					free(border);
					free(bcut);
					continue;
				}

				for (i = 0; i < numborder; i++) {
					f32 mid[3];
					f32 bestd = 1e30f;
					s32 j;

					mid[0] = (mb.fpos[border[i]->a][0] + mb.fpos[border[i]->b][0]) * 0.5f;
					mid[1] = (mb.fpos[border[i]->a][1] + mb.fpos[border[i]->b][1]) * 0.5f;
					mid[2] = (mb.fpos[border[i]->a][2] + mb.fpos[border[i]->b][2]) * 0.5f;
					bgroup[i] = bcut[i];

					for (j = 1; j < rd->numparts && bcut[i] < 0; j++) {
						if ((rd->severed & (1u << j))
								&& (ragdollPieceOf(rd, j) == piece || ragdollPieceOf(rd, rd->skel->parent[j]) == piece)) {
							f32 d[3];

							rdVecSub(mid, cutpos[j], d);

							if (rdVecLen(d) < bestd) {
								bestd = rdVecLen(d);
								bgroup[i] = j;
							}
						}
					}
				}

				for (g = 1; g < rd->numparts; g++) {
					if ((rd->severed & (1u << g)) == 0) {
						continue;
					}

			for (i = 0; i < mb.numverts; i++) {
				uf[i] = i;
			}

			for (i = 0; i < numborder; i++) {
				if (bgroup[i] == g) {
					uf[rdFind(uf, border[i]->a)] = rdFind(uf, border[i]->b);
				}
			}

			// One cap per border loop
			for (i = 0; i < numborder && rd->numcaps < RD_MAXCAPS; i++) {
				s32 root;

				if (bgroup[i] != g) {
					continue;
				}

				root = rdFind(uf, border[i]->a);
				f32 sum[3] = {0, 0, 0};
				s32 cnt = 0;
				s32 j;
				bool seen = false;
				f32 centre[3];
				s32 cut = -1;
				bool childside;
				s32 frame;
				f32 pf[3], pc[3], pp[3];
				struct rdcap *cap;
				f32 ringr = 0.0f;

				for (j = 0; j < i; j++) {
					if (bgroup[j] == g && rdFind(uf, border[j]->a) == root) {
						seen = true;
						break;
					}
				}

				if (seen) {
					continue;
				}

				for (j = i; j < numborder; j++) {
					if (bgroup[j] == g && rdFind(uf, border[j]->a) == root) {
						sum[0] += (mb.fpos[border[j]->a][0] + mb.fpos[border[j]->b][0]);
						sum[1] += (mb.fpos[border[j]->a][1] + mb.fpos[border[j]->b][1]);
						sum[2] += (mb.fpos[border[j]->a][2] + mb.fpos[border[j]->b][2]);
						cnt += 2;
					}
				}

				if (cnt < 6) {
					continue; // a stray edge, not a hole
				}

				centre[0] = sum[0] / cnt;
				centre[1] = sum[1] / cnt;
				centre[2] = sum[2] / cnt;

				cut = g;

				childside = ragdollPieceOf(rd, cut) == piece;
				frame = childside ? cut : rd->skel->parent[cut];

				if (ragdollPieceOf(rd, frame) != piece) {
					continue;
				}

				ragdollRelPos(rd, frame, pf);
				rdVecCopy(cutpos[cut], pc);
				ragdollRelPos(rd, rd->skel->parent[cut], pp);

				cap = &rd->caps[rd->numcaps];
				cap->edges = malloc((cnt / 2) * sizeof(*cap->edges));

				if (!cap->edges) {
					continue;
				}

				rd->numcaps++;
				covered[cut][childside ? 1 : 0] = true;
				cap->part = frame;
				cap->numedges = 0;

				for (j = 0; j < 3; j++) {
					cap->normal[j] = childside ? pp[j] - pc[j] : pc[j] - pp[j];
				}

				rdVecNorm(cap->normal);

				for (j = i; j < numborder; j++) {
					if (bgroup[j] == g && rdFind(uf, border[j]->a) == root) {
						s32 k;

						cap->edges[cap->numedges][0].mtx = border[j]->va.mtx;
						cap->edges[cap->numedges][0].v[0] = border[j]->va.v[0];
						cap->edges[cap->numedges][0].v[1] = border[j]->va.v[1];
						cap->edges[cap->numedges][0].v[2] = border[j]->va.v[2];
						cap->edges[cap->numedges][1].mtx = border[j]->vb.mtx;
						cap->edges[cap->numedges][1].v[0] = border[j]->vb.v[0];
						cap->edges[cap->numedges][1].v[1] = border[j]->vb.v[1];
						cap->edges[cap->numedges][1].v[2] = border[j]->vb.v[2];
						cap->numedges++;

						for (k = 0; k < 2; k++) {
							s32 id = k == 0 ? border[j]->a : border[j]->b;
							f32 d[3];

							rdVecSub(mb.fpos[id], centre, d);
							ringr += rdVecLen(d);
						}
					}
				}

				ringr /= cnt;

				// Slight bulge outwards so it reads as a surface, not a hole
				for (j = 0; j < 3; j++) {
					cap->centre[j] = centre[j] - pf[j] + cap->normal[j] * ringr * 0.2f;
				}
			}
				}

				free(bgroup);
			}

			free(border);
			free(bcut);
		}
	}

done:
	free(uf);
	free(mb.cell);
	free(mb.fpos);
	free(mb.hash);
	free(mb.edges);

	// Holes the mesh couldn't describe get a small disc
	for (i = 1; i < rd->numparts; i++) {
		if (rd->severed & (1u << i)) {
			if (!covered[i][1]) {
				ragdollAddDiscCap(rd, i, true);
			}

			if (!covered[i][0]) {
				ragdollAddDiscCap(rd, i, false);
			}
		}
	}
}

s32 ragdollHasCutPieces(struct chrdata *chr)
{
	struct ragdoll *rd = ragdollFindByChr(chr);

	return rd != NULL && rd->severed != 0;
}

/**
 * chrTestHit()'s quick check is a sphere around the model's root (pelvis).
 * Cut pieces get the same check around their own joint, using the model's
 * render matrices (same space as the root matrix the game uses).
 */
s32 ragdollShotNearCutPiece(struct chrdata *chr, struct coord *gunpos, struct coord *gundir, f32 radius)
{
	RD_CRASHCTX("shotnearcutpiece", RD_CHRNUM(chr), -1);
	struct ragdoll *rd = ragdollFindByChr(chr);
	s32 i;

	if (rd == NULL || rd->severed == 0 || rd->model->matrices == NULL) {
		return false;
	}

	for (i = 1; i < rd->numparts; i++) {
		if (rd->severed & (1u << i)) {
			s32 idx = rd->mtxindex[i];

			if (idx >= 0 && idx < rd->model->definition->nummatrices
					&& func0f06b39c(gunpos, gundir, (struct coord *) rd->model->matrices[idx].m[3], radius)) {
				return true;
			}
		}
	}

	return false;
}

/**
 * Cuts 'part' (and everything attached to it) off its parent. vel is an extra
 * push for the cut piece (units/s), may be NULL.
 */
static bool ragdollSever(struct ragdoll *rd, s32 part, const f32 *vel)
{
	RD_CRASHCTX("sever", RD_RDCHRNUM(rd), part);
	s32 k;

	if (part <= 0 || part >= rd->numparts || rd->skel->cutradius[part] <= 0.0f
			|| (rd->severed & (1u << part)) || rd->bodyindex[part] < 0) {
		return false;
	}

	if (!ragdollWakeFrozen(rd)) {
		return false;
	}

	rd->severed |= 1u << part;

	// A leg cut off while the death animation plays: it carries on for a
	// moment, then the body goes limp (64 frames)
	if (rd->blending && rd->legcut60 < 0 && part >= 9 && part <= 14) {
		rd->legcut60 = rd->age60;
	}
	// The blend carries on: cut pieces aren't steered by the animation
	// (ragdollBlendTick skips everything that isn't the main piece)
	pdphysRagdollDetach(rd->handle, rd->bodyindex[part]);

	for (k = 0; k < 8; k++) {
		rd->piecerooms[part][k] = -1;
	}

	// Separate the mesh at the cut, then cap both open ends
	ragdollRebuildSeams(rd);
	ragdollBuildCaps(rd);
	g_RagdollChangeSerial++;

	if (vel) {
		pdphysRagdollAddVelocity(rd->handle, rd->bodyindex[part], vel);
	}

	rd->bleed60 = 0;

	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d lost part %d (%d display lists patched, %d caps, %d of them fallback discs)", rd->chr->chrnum, part, rd->numpatches, rd->numcaps, rd->numdisccaps);

	return true;
}

// The joint to cut for a hit on this hitpart, or -1
static s32 ragdollCutPartForHit(struct ragdoll *rd, s32 hitpart, f32 damage)
{
	s32 sp = rdHitpartPart(rd, hitpart);

	if (hitpart < 0 || hitpart >= 20) {
		return -1;
	}

	if (sp <= 1) {
		// Torso/pelvis: only the heaviest hits split the body at the waist
		return g_RagdollDismemberTorso && damage >= 50.0f ? 1 : -1;
	}

	return sp;
}

/**
 * A shot that killed (or hit a corpse): strong enough weapons can take the
 * hit limb or head off.
 */
#define RD_DEADLYPUNCH_SPLITCHANCE 35   // % of Deadly Punch kills split at the waist (times the dismember chance)
#define RD_DEADLYPUNCH_SPLITDAMAGE 100.0f // as a FarSight hit: above any Damage Needed (max 10), and ragdollCutPartForHit splits the torso from 50

static bool g_RdMeleeAttack = false; // inside a player's melee attack (handTickAttack)

void ragdollMeleeAttackBegin(void)
{
	g_RdMeleeAttack = true;
}

void ragdollMeleeAttackEnd(void)
{
	g_RdMeleeAttack = false;
}

/**
 * The Reaper's blades on a body: the game's melee attack only hits the
 * living, so a corpse on the floor was never cut by grinding it. Any joint of
 * a body close to the spinning blades comes off, one at a time.
 */
void ragdollReaperGrind(void)
{
	static s32 lastgrind60[RD_MAXRAGDOLLS];
	struct player *player = g_Vars.currentplayer;
	s32 i;
	s32 p;

	if (!g_RagdollDismember || player == NULL || player->prop == NULL) {
		return;
	}

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];
		s32 best = -1;
		f32 bestdist = 30.0f * 30.0f;

		if (rd->state == RDSTATE_FREE || rd->chr == NULL || rd->chr->noblood) {
			continue;
		}

		if (lastgrind60[i] != 0 && g_Vars.lvframe60 - lastgrind60[i] < 10 && g_Vars.lvframe60 >= lastgrind60[i]) {
			continue;
		}

		for (p = 2; p < rd->numparts; p++) {
			f32 *m;
			f32 dx, dy, dz, along, d;

			if ((rd->severed & (1u << p)) || rd->bodyindex[p] < 0) {
				continue;
			}

			// Distance from the joint to the line from the camera out to
			// the blades' reach
			m = rd->world[p].m[3];
			dx = m[0] - player->cam_pos.x;
			dy = m[1] - player->cam_pos.y;
			dz = m[2] - player->cam_pos.z;
			along = dx * player->cam_look.x + dy * player->cam_look.y + dz * player->cam_look.z;

			if (along < 10.0f || along > 110.0f) {
				continue;
			}

			dx -= player->cam_look.x * along;
			dy -= player->cam_look.y * along;
			dz -= player->cam_look.z * along;
			d = dx * dx + dy * dy + dz * dz;

			if (d < bestdist) {
				bestdist = d;
				best = p;
			}
		}

		if (best >= 0) {
			f32 vel[3];

			rdVecSet(vel, player->cam_look.x * 150.0f, 120.0f, player->cam_look.z * 150.0f);

			if (ragdollSever(rd, best, vel)) {
				lastgrind60[i] = RD_FRAMESTAMP();
			}
		}
	}
}

void ragdollTryDismemberShot(struct ragdoll *rd, s32 hitpart, struct coord *dir, struct gset *gset)
{
	RD_CRASHCTX("trydismembershot", RD_RDCHRNUM(rd), hitpart);
	f32 damage;
	bool strong;
	bool laserstream;
	s32 part;
	f32 vel[3];

	if (!g_RagdollDismember || rd == NULL || gset == NULL || rd->chr->noblood) {
		return;
	}

	// The Laser's short-range stream does 0.1 damage, 60 times a second: it
	// cuts only with Weapon Damage Needed at its minimum (0.5), each hit with
	// a tenth of the chance (about one ordinary shot's chance every 1/6 s of
	// contact)
	laserstream = gset->weaponnum == WEAPON_LASER && gset->weaponfunc == FUNC_SECONDARY;

	if (laserstream && g_RagdollDismemberDamage > RD_DISMEMBER_MINDAMAGE + 0.05f) {
		return;
	}

	damage = gsetGetDamage(gset);

	// The tranquiliser's lethal injection and the crossbow's instant kill are
	// huge damage numbers, not blades or blasts: they only cut with Weapon
	// Damage Needed at its minimum (for fun)
	if ((gset->weaponnum == WEAPON_TRANQUILIZER || gset->weaponnum == WEAPON_CROSSBOW || gset->weaponnum == WEAPON_BOLT)
			&& g_RagdollDismemberDamage > RD_DISMEMBER_MINDAMAGE + 0.05f) {
		return;
	}

	// The shotgun: only a whole shot on one limb (ragdollShotgunVolleyEnd)
	if (gset->weaponnum == WEAPON_SHOTGUN) {
		return;
	}

	// Knocked out, not dead: nothing comes off
	if (rd->chr->actiontype == ACT_DRUGGEDDROP || rd->chr->actiontype == ACT_DRUGGEDKO
			|| rd->chr->actiontype == ACT_DRUGGEDCOMINGUP) {
		return;
	}

	// Deadly Punch: only the killing punch, and only a split at the waist
	// (RD_DEADLYPUNCH_SPLITCHANCE, times the dismember chance below); punches
	// on bodies would cut far too often
	if (cheatIsDeadlyPunch(gset)) {
		if (!g_RdDismemberAtDeath || !g_RagdollDismemberTorso
				|| (s32)(rngRandom() % 100) >= RD_DEADLYPUNCH_SPLITCHANCE) {
			return;
		}

		hitpart = HITPART_TORSO;
		damage = RD_DEADLYPUNCH_SPLITDAMAGE;
	}

	// The Reaper: only its spinning blades cut (the grinder, or the primary
	// spinning with an empty magazine), not its bullets
	if (gset->weaponnum == WEAPON_REAPER) {
		struct weaponfunc *func = gsetGetWeaponFunction(gset);

		if (!g_RdMeleeAttack && !(func && (func->type & 0xff) == INVENTORYFUNCTYPE_MELEE)) {
			return;
		}

		// Melee hits only say torso (standing), general-half (squatting) or
		// general (ducking): the blades take off something at that height
		if (hitpart == HITPART_TORSO || hitpart == HITPART_PELVIS) {
			static const s32 upper[] = { HITPART_HEAD, HITPART_LBICEP, HITPART_RBICEP, HITPART_LFOREARM, HITPART_RFOREARM };
			hitpart = upper[rngRandom() % ARRAYCOUNT(upper)];
		} else if (hitpart == HITPART_GENERALHALF) {
			static const s32 middle[] = { HITPART_LFOREARM, HITPART_RFOREARM, HITPART_LTHIGH, HITPART_RTHIGH };
			hitpart = middle[rngRandom() % ARRAYCOUNT(middle)];
		} else if (hitpart == HITPART_GENERAL) {
			static const s32 lower[] = { HITPART_LSHIN, HITPART_RSHIN, HITPART_LTHIGH, HITPART_RTHIGH };
			hitpart = lower[rngRandom() % ARRAYCOUNT(lower)];
		}
	}

	strong = damage >= g_RagdollDismemberDamage || gset->weaponnum == WEAPON_REAPER || laserstream;

	// (The Reaper's blades always cut: they're a grinder)
	if (!strong || (gset->weaponnum != WEAPON_REAPER
				&& (s32)(rngRandom() % (laserstream ? 1000 : 100)) >= g_RagdollDismemberChance)) {
		return;
	}

	part = ragdollCutPartForHit(rd, hitpart, damage);

	if (part < 0) {
		return;
	}

	rdVecSet(vel, dir ? dir->x * 350.0f * rd->pushscale : 0.0f, (dir ? dir->y * 350.0f * rd->pushscale : 0.0f) + 120.0f, dir ? dir->z * 350.0f * rd->pushscale : 0.0f);

	ragdollSever(rd, part, vel);
}

/**
 * prop.c, the player firing the shotgun: Begin, a Pellet before each of its
 * pellets, End after. chr.c reports each pellet that hits a chr.
 */
void ragdollShotgunVolleyBegin(void)
{
	memset(&g_RdVolley, 0, sizeof(g_RdVolley));
	g_RdVolley.active = true;
}

void ragdollShotgunPellet(void)
{
	if (g_RdVolley.active) {
		g_RdVolley.pellets++;
		g_RdVolley.pellethit = false;
	}
}

void ragdollShotgunPelletHit(struct chrdata *chr, s32 hitpart, struct coord *dir, struct gset *gset)
{
	struct rdvolley *v = &g_RdVolley;

	if (!v->active || v->pellethit || gset == NULL || gset->weaponnum != WEAPON_SHOTGUN) {
		return;
	}

	v->pellethit = true;

	if (v->hits == 0) {
		v->chr = chr;
	} else if (v->chr != chr) {
		v->chr = NULL; // pellets on more than one body
	}

	if (v->hits < RD_VOLLEY_MAXPELLETS) {
		v->hitparts[v->hits] = hitpart;
	}

	v->hits++;
	v->dir = *dir;
	v->gset = *gset;
}

void ragdollShotgunVolleyEnd(void)
{
	RD_CRASHCTX("shotgunvolley", -1, -1);
	struct rdvolley *v = &g_RdVolley;
	struct ragdoll *rd;
	s32 target = -1;
	s32 i;
	f32 vel[3];

	if (!v->active) {
		return;
	}

	v->active = false;

	// Every pellet hit, all on the same body, which is now dead
	if (!g_RagdollDismember || v->chr == NULL || v->pellets <= 0 || v->hits != v->pellets
			|| v->hits > RD_VOLLEY_MAXPELLETS || v->chr->noblood) {
		return;
	}

	rd = ragdollFindByChr(v->chr);

	if (rd == NULL) {
		return;
	}

	// ... and on one limb: the piece nearest the body that was hit, with
	// every other pellet on it or further out the same limb
	for (i = 0; i < v->hits; i++) {
		s32 hp = v->hitparts[i];
		s32 sp;

		if (hp < 0 || hp >= 20) {
			return;
		}

		sp = rdHitpartPart(rd, hp);

		if (sp <= 1) {
			return; // torso or pelvis
		}

		if (target < 0) {
			target = sp;
		} else if (sp != target) {
			s32 a;

			// sp further out from target: fine
			for (a = sp; a > 0 && a != target; a = rd->skel->parent[a]);

			if (a != target) {
				// target further out from sp: sp becomes the cut
				for (a = target; a > 0 && a != sp; a = rd->skel->parent[a]);

				if (a != sp) {
					return; // two different limbs
				}

				target = sp;
			}
		}
	}

	if (target <= 1 || (s32)(rngRandom() % 100) >= g_RagdollDismemberChance) {
		return;
	}

	rdVecSet(vel, v->dir.x * 350.0f * rd->pushscale, v->dir.y * 350.0f * rd->pushscale + 120.0f, v->dir.z * 350.0f * rd->pushscale);

	ragdollSever(rd, target, vel);
}

/**
 * Explosions: joints close to the blast can come off (up to two per blast).
 */
void ragdollTryDismemberBlast(struct ragdoll *rd, struct coord *centre, f32 radius)
{
	RD_CRASHCTX("trydismemberblast", RD_RDCHRNUM(rd), -1);
	static const s8 joints[] = { 2, 3, 4, 5, 6, 9, 10, 11, 12, 1 };
	s32 cut = 0;
	s32 i;

	if (!g_RagdollDismember || !g_RagdollDismemberExplosions || rd->chr->noblood || radius <= 0.0f) {
		return;
	}

	for (i = 0; i < ARRAYCOUNT(joints) && cut < 2; i++) {
		s32 part = joints[i];
		f32 d[3];
		f32 dist;
		f32 reach = part == 1 ? radius * 0.35f : radius * 0.6f;
		f32 chance;

		if (part == 1 && !g_RagdollDismemberTorso) {
			continue;
		}

		d[0] = rd->world[part].m[3][0] - centre->x;
		d[1] = rd->world[part].m[3][1] - centre->y;
		d[2] = rd->world[part].m[3][2] - centre->z;
		dist = rdVecLen(d);

		if (dist >= reach) {
			continue;
		}

		chance = g_RagdollDismemberChance * (1.0f - dist / reach);

		if ((f32)(rngRandom() % 100) < chance && ragdollSever(rd, part, NULL)) {
			cut++;
		}
	}
}

/**
 * Called from chrRender() after the chr's model (opaque pass): draws the red
 * caps over the cut ends.
 */
Gfx *ragdollRenderCaps(Gfx *gdl, struct chrdata *chr)
{
	RD_CRASHCTX("rendercaps", RD_CHRNUM(chr), -1);
	struct ragdoll *rd = ragdollFindByChr(chr);
	struct model *model;
	s32 nummtx;
	Col *colours;
	u32 alpha = 255;
	s32 i;

	if (rd == NULL || rd->numcaps == 0 || rd->model != chr->model || chr->model->matrices == NULL) {
		return gdl;
	}

	model = rd->model;
	nummtx = model->definition->nummatrices;

	// A fading corpse fades its caps with it (they used to stay solid)
	if (chr->actiontype == ACT_DEAD && (chr->act_dead.fadenow || chr->act_dead.fadetimer60 >= 0)) {
		alpha = chr->fadealpha;
	}

	if (alpha == 0) {
		return gdl;
	}

	gSPClearGeometryMode(gdl++, G_CULL_BOTH);
	gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH);
	texSelect(&gdl, NULL, 2, 0, 2, 1, NULL);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);

	if (alpha < 255) {
		gDPSetRenderMode(gdl++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
	} else {
		gDPSetRenderMode(gdl++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
	}

	colours = gfxAllocateColours(2);

	// Same colours as the blood droplets (chrGetBloodColour: red for humans,
	// green for Maians), slightly lifted because the caps are unlit
	{
		u32 blood[3];
		s32 k;

		chrGetBloodColour(chr->bodynum, NULL, blood);

		for (k = 0; k < 2; k++) {
			u32 c = blood[k == 0 ? 1 : 0];
			u32 r = ((c >> 24) & 0xff) * RD_CUTCOLOUR_LIFT;
			u32 g = ((c >> 16) & 0xff) * RD_CUTCOLOUR_LIFT;
			u32 b = ((c >> 8) & 0xff) * RD_CUTCOLOUR_LIFT;

			colours[k].word = PD_BE32((RD_MIN(r, 255) << 24) | (RD_MIN(g, 255) << 16) | (RD_MIN(b, 255) << 8) | alpha);
		}
	}

	gSPColor(gdl++, osVirtualToPhysical(colours), 2);

	for (i = 0; i < rd->numcaps; i++) {
		struct rdcap *cap = &rd->caps[i];
		s32 centremtx = rd->mtxindex[cap->part];
		s32 curmtx = -1;
		s32 e = 0;

		if (centremtx < 0 || centremtx >= nummtx) {
			continue;
		}

		// Fan from the centre. Rim vertices use the same matrices and
		// coordinates as the mesh around the hole, so the cap stays on its
		// edge however the limb bends. Up to 5 triangles (15 slots) per batch.
		while (e < cap->numedges) {
			s32 n = cap->numedges - e < 5 ? cap->numedges - e : 5;
			Vtx *vertices;

			// Out of display list or vertex space this frame: draw no more
			// caps rather than writing past the buffers
			if (gfxGetFreeGfx(gdl) < 64 || gfxGetFreeVtx() < 15 * sizeof(Vtx) + 64) {
				return gdl;
			}

			vertices = gfxAllocateVertices(n * 3);
			s32 t, k;

			for (t = 0; t < n; t++) {
				for (k = 0; k < 3; k++) {
					Vtx *vx = &vertices[t * 3 + k];
					s32 mtx;

					if (k == 0) {
						mtx = centremtx;
						vx->x = (s16) cap->centre[0];
						vx->y = (s16) cap->centre[1];
						vx->z = (s16) cap->centre[2];
						vx->colour = 0;
					} else {
						struct rdcapvtx *cv = &cap->edges[e + t][k - 1];

						mtx = cv->mtx >= 0 && cv->mtx < nummtx ? cv->mtx : centremtx;
						vx->x = cv->v[0];
						vx->y = cv->v[1];
						vx->z = cv->v[2];
						vx->colour = 4;
					}

					vx->flags = 0;
					vx->s = 0;
					vx->t = 0;

					if (mtx != curmtx) {
						gSPMatrix(gdl++, osVirtualToPhysical(&model->matrices[mtx]), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
						curmtx = mtx;
					}

					gSPVertex(gdl++, osVirtualToPhysical(vx), 1, t * 3 + k);
				}
			}

			if (n == 5) {
				gSPTri4(gdl++, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
				gSPTri1(gdl++, 12, 13, 14);
			} else {
				gSPTri4(gdl++,
						0, 1, 2,
						n > 1 ? 3 : 0, n > 1 ? 4 : 0, n > 1 ? 5 : 0,
						n > 2 ? 6 : 0, n > 2 ? 7 : 0, n > 2 ? 8 : 0,
						n > 3 ? 9 : 0, n > 3 ? 10 : 0, n > 3 ? 11 : 0);
			}

			e += n;
		}
	}

	return gdl;
}

/**
 * While the body is still moving, the cut ends spray blood.
 */
void ragdollBleed(struct ragdoll *rd)
{
	RD_CRASHCTX("bleed", RD_RDCHRNUM(rd), -1);
	static s32 budgetframe = -1;
	static s32 budget = 0;
	static s32 splatbudget = 0;
	bool dosplat;
	s32 i;

	if (rd->numcaps == 0 || rd->chr->noblood || rd->state != RDSTATE_SIMULATING) {
		return;
	}

	if (rd->prop == NULL || rd->prop->rooms[0] < 0) {
		return; // thrown outside every room: sparks/splats need a room
	}

	rd->bleed60 += g_Vars.lvupdate60;
	rd->splat60 += g_Vars.lvupdate60;

	if (rd->bleed60 < 4) {
		return;
	}

	rd->bleed60 = 0;

	if (pdphysRagdollIsAsleep(rd->handle)) {
		return;
	}

	// Keep the spark groups and decals for gunfire too
	if (budgetframe != g_Vars.lvframe60) {
		budgetframe = g_Vars.lvframe60;
		budget = 6;
		splatbudget = 2;
	}

	// Blood decals where the spray lands, every ~0.3 s per cut end
	dosplat = rd->splat60 >= 18;

	if (dosplat) {
		rd->splat60 = 0;
	}

	for (i = 0; i < rd->numcaps && budget > 0; i++) {
		struct rdcap *cap = &rd->caps[i];
		struct coord pos;
		struct coord dir;
		f32 local[3];
		f32 c[3];
		f32 n[3];

		rdVecScale(cap->centre, rd->scale, local);
		rdRotApply(&rd->world[cap->part], local, c);
		rdRotApply(&rd->world[cap->part], cap->normal, n);

		pos.x = rd->world[cap->part].m[3][0] + c[0];
		pos.y = rd->world[cap->part].m[3][1] + c[1];
		pos.z = rd->world[cap->part].m[3][2] + c[2];

		dir.x = n[0] + ((s32)(rngRandom() % 61) - 30) * 0.01f;
		dir.y = n[1] + ((s32)(rngRandom() % 61) - 30) * 0.01f;
		dir.z = n[2] + ((s32)(rngRandom() % 61) - 30) * 0.01f;

		sparksCreate(rd->prop->rooms[0], rd->prop, &pos, &dir, NULL, SPARKTYPE_BLOOD);
		budget--;

		if (dosplat && splatbudget > 0) {
			// The game's own blood splat code, aimed along the spray and
			// downwards (it finds the floor or wall within ~1.8 m)
			struct shotdata sd;
			f32 d[3];

			rdVecSet(d, dir.x * 0.6f, dir.y * 0.6f - 0.8f, dir.z * 0.6f);
			rdVecNorm(d);

			memset(&sd, 0, sizeof(sd));
			sd.gunpos3d = pos;
			sd.gunpos2d = pos;
			sd.gundir3d.x = sd.gundir2d.x = d[0];
			sd.gundir3d.y = sd.gundir2d.y = d[1];
			sd.gundir3d.z = sd.gundir2d.z = d[2];

			splatsCreate(1, 0.8f, rd->prop, &sd, &pos, &pos, false, 0, TICKS(50), rd->chr, 0);
			splatbudget--;
		}
	}

}

#endif // PD_PHYSICS
