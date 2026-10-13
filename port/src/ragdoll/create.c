/**
 * MOD: ragdoll physics - making a ragdoll's physics body: the pose at death,
 * the part shapes from the hit boxes, the joints. Part of the ragdoll module
 * (port/src/ragdoll.c).
 */

#include "rd_internal.h"

#ifdef PD_PHYSICS

/**
 * Computes the chr's current pose in world space by running the game's own
 * matrix code with an identity camera into a scratch buffer, then extracts
 * each skeleton part's joint frame. Also works out which hitpart belongs to
 * which part. Returns false if the model isn't a standard human skeleton.
 */
bool ragdollCapturePose(struct ragdoll *rd, struct chrdata *chr)
{
	RD_CRASHCTX("capturepose", RD_CHRNUM(chr), -1);
	struct model *model = chr->model;
	struct modeldef *def = model->definition;
	struct modelrenderdata renderdata = {NULL, true, 3};
	struct modelnode *node;
	s32 mtxindex[RD_NUMPARTS];
	s32 parentpart[RD_NUMPARTS];
	s32 hitpartpart[20];
	s32 extramtx[32];   // nodes that aren't parts (fingers, toes): their matrices ...
	s32 extrapart[32];  // ... and the part they hang off
	s32 numextra = 0;
	Mtxf identity;
	Mtxf *buffer;
	Mtxf *saved;
	s32 i;
	bool ok = true;

	for (i = 0; i < rd->numparts; i++) {
		mtxindex[i] = -1;
		parentpart[i] = -2;
	}

	for (i = 0; i < 20; i++) {
		hitpartpart[i] = -1;
	}

	// Map parts to matrices and parents, and hitparts to parts
	ragdollApplyRelations(model);
	node = def->rootnode;

	while (node) {
		u32 type = node->type & 0xff;

		if (type == MODELNODETYPE_CHRINFO) {
			mtxindex[0] = node->rodata->chrinfo.mtxindex;
			parentpart[0] = -1;
		} else if (type == MODELNODETYPE_POSITION || type == MODELNODETYPE_BBOX) {
			// The nearest ancestor that's a part of the skeleton (the
			// Skedar's tail tip, fingers and toes aren't)
			struct modelnode *anc = node->parent;
			s32 ancpart = -1;

			while (anc) {
				u32 anctype = anc->type & 0xff;

				if (anctype == MODELNODETYPE_CHRINFO) {
					ancpart = 0;
					break;
				}

				if (anctype == MODELNODETYPE_POSITION && rdPartOfNode(rd->skel, anc->rodata->position.part) > 0) {
					ancpart = rdPartOfNode(rd->skel, anc->rodata->position.part);
					break;
				}

				anc = anc->parent;
			}

			if (type == MODELNODETYPE_POSITION) {
				s32 part = rdPartOfNode(rd->skel, node->rodata->position.part);

				if (part > 0) {
					mtxindex[part] = node->rodata->position.mtxindex0;
					parentpart[part] = ancpart;
				} else if (ancpart > 0 && numextra < ARRAYCOUNT(extramtx)) {
					extramtx[numextra] = node->rodata->position.mtxindex0;
					extrapart[numextra] = ancpart;
					numextra++;
				}
			} else {
				struct modelrodata_bbox *bbox = &node->rodata->bbox;
				s32 hitpart = bbox->hitpart;

				if (hitpart >= 0 && hitpart < 20 && ancpart >= 0) {
					hitpartpart[hitpart] = ancpart;
				}

				// Its hit boxes (an empty one is a placeholder)
				if (ancpart >= 0 && bbox->xmax > bbox->xmin && bbox->ymax > bbox->ymin && bbox->zmax > bbox->zmin) {
					f32 *hb = rd->hitbox[ancpart];

					if (!rd->hashitbox[ancpart]) {
						hb[0] = bbox->xmin; hb[1] = bbox->ymin; hb[2] = bbox->zmin;
						hb[3] = bbox->xmax; hb[4] = bbox->ymax; hb[5] = bbox->zmax;
						rd->hashitbox[ancpart] = true;
					} else {
						hb[0] = fminf(hb[0], bbox->xmin); hb[1] = fminf(hb[1], bbox->ymin); hb[2] = fminf(hb[2], bbox->zmin);
						hb[3] = fmaxf(hb[3], bbox->xmax); hb[4] = fmaxf(hb[4], bbox->ymax); hb[5] = fmaxf(hb[5], bbox->zmax);
					}
				}
			}
		}

		node = ragdollWalkNext(model, node);
	}

	for (i = 0; i < rd->numparts; i++) {
		if (mtxindex[i] < 0 || mtxindex[i] >= def->nummatrices || parentpart[i] != rd->skel->parent[i]) {
			ok = false;
		}
	}

	if (!ok) {
		sysLogPrintf(LOG_WARNING, "RAGDOLL: chr %d body %d has an unexpected skeleton layout, using death animation", chr->chrnum, chr->bodynum);
		return false;
	}

	for (i = 0; i < rd->numparts; i++) {
		rd->mtxindex[i] = mtxindex[i];
	}

	// World-space matrices for the current pose
	buffer = malloc(sizeof(Mtxf) * def->nummatrices);

	if (!buffer) {
		return false;
	}

	mtx4LoadIdentity(&identity);
	renderdata.unk00 = &identity;
	renderdata.unk10 = buffer;

	saved = model->matrices;
	modelSetMatricesWithAnim(&renderdata, model);
	model->matrices = saved;

	{
		Mtxf *root = &buffer[mtxindex[0]];
		rd->scale = rdVecLen(root->m[0]);
	}

	if (rd->scale < 0.0001f) {
		free(buffer);
		return false;
	}

	for (i = 0; i < rd->numparts; i++) {
		Mtxf *src = &buffer[mtxindex[i]];
		s32 c, r;

		for (c = 0; c < 3; c++) {
			for (r = 0; r < 3; r++) {
				rd->world[i].m[c][r] = src->m[c][r] / rd->scale;
			}

			rd->world[i].m[c][3] = 0.0f;
		}

		rdVecCopy(src->m[3], rd->world[i].m[3]);
		rd->world[i].m[3][3] = 1.0f;
	}

	// The far end of what hangs off parts with nothing after them
	for (i = 0; i < numextra; i++) {
		s32 part = extrapart[i];

		if (extramtx[i] >= 0 && extramtx[i] < def->nummatrices && part > 0 && rd->skel->child[part] < 0) {
			f32 *at = buffer[extramtx[i]].m[3];
			f32 d[3];
			f32 cur[3];

			rdVecSub(at, rd->world[part].m[3], d);

			if (rd->hastip[part]) {
				rdVecSub(rd->tip[part], rd->world[part].m[3], cur);
			}

			if (!rd->hastip[part] || rdVecLen(d) > rdVecLen(cur)) {
				rdVecCopy(at, rd->tip[part]);
				rd->hastip[part] = true;
			}
		}
	}

	free(buffer);

	// Fixed local offsets (keeps the skeleton rigid exactly as modelled)
	for (i = 1; i < rd->numparts; i++) {
		s32 p = rd->skel->parent[i];
		f32 diff[3];
		f32 local[3];

		rdVecSub(rd->world[i].m[3], rd->world[p].m[3], diff);
		rdRotApplyT(&rd->world[p], diff, local);

		rd->localpos[i].x = local[0] / rd->scale;
		rd->localpos[i].y = local[1] / rd->scale;
		rd->localpos[i].z = local[2] / rd->scale;

		rdRotTransposeMul(&rd->world[p], &rd->world[i], &rd->localrot[i]);
	}

	// Hitpart -> physics part (parts without a body use their parent's)
	{
		s32 n = 0;

		for (i = 0; i < rd->numparts; i++) {
			rd->bodyindex[i] = rd->skel->hasbody[i] ? n++ : -1;
		}

		for (i = 0; i < 20; i++) {
			s32 part;

			rd->hitskel[i] = hitpartpart[i];
			part = rdHitpartPart(rd, i);
			rd->hitpartbody[i] = part >= 0 ? rd->bodyindex[part] : -1;
		}
	}

	return true;
}

#define RD_MASS_REFTORSO 41.0f // torso length (units) the human masses are for

static void ragdollSetCapsule(pdphysRagdollPart *pp, const f32 *a, const f32 *b, f32 radius)
{
	pp->shape = PDPHYS_SHAPE_CAPSULE;
	pp->a[0] = a[0]; pp->a[1] = a[1]; pp->a[2] = a[2];
	pp->b[0] = b[0]; pp->b[1] = b[1]; pp->b[2] = b[2];
	pp->radius = radius;
}

static void ragdollSetSwingTwist(pdphysRagdollPart *pp, const f32 *twist, const f32 *plane, f32 normalcone, f32 planecone, f32 twistmin, f32 twistmax)
{
	pp->joint = PDPHYS_JOINT_SWINGTWIST;
	memcpy(pp->axis, twist, sizeof(pp->axis));
	memcpy(pp->axis2, plane, sizeof(pp->axis2));
	rdVecSet(pp->limits, normalcone, planecone, twistmin);
	pp->limits[3] = twistmax;
}

// Torso length (units): sizes and masses scale with it
f32 rdTorsoLength(struct ragdoll *rd)
{
	f32 spine[3];
	f32 len;

	// Human: waist to neck. Skedar: pelvis to neck (their chest joint sits
	// at the pelvis).
	rdVecSub(rd->world[2].m[3], rd->world[rd->skel->human ? 1 : 0].m[3], spine);
	len = rdVecLen(spine);

	return len < 5.0f ? 40.0f : len;
}

// A point of part 'i' given in its joint frame (model units) to world
static void rdPartLocalToWorld(struct ragdoll *rd, s32 i, const f32 *local, f32 *out)
{
	s32 k;

	for (k = 0; k < 3; k++) {
		out[k] = rd->world[i].m[3][k] + (local[0] * rd->world[i].m[0][k] + local[1] * rd->world[i].m[1][k]
				+ local[2] * rd->world[i].m[2][k]) * rd->scale;
	}
}

// The thickness (radius, units) of part i's hit box across its longest side, or 0
static f32 rdHitboxRadius(struct ragdoll *rd, s32 i)
{
	const f32 *hb = rd->hitbox[i];
	f32 ext[3];
	f32 lo, hi;

	if (!rd->hashitbox[i]) {
		return 0.0f;
	}

	ext[0] = hb[3] - hb[0];
	ext[1] = hb[4] - hb[1];
	ext[2] = hb[5] - hb[2];

	// The two smaller sides
	if (ext[0] >= ext[1] && ext[0] >= ext[2]) {
		lo = ext[1]; hi = ext[2];
	} else if (ext[1] >= ext[2]) {
		lo = ext[0]; hi = ext[2];
	} else {
		lo = ext[0]; hi = ext[1];
	}

	return (lo + hi) * 0.25f * 0.85f * rd->scale;
}

/**
 * Non-human skeletons (the Skedar): the trunk parts are capsules fitted to
 * their hit boxes, the limbs and tail capsules along their bones as thick as
 * their hit boxes. Hinges bend the way they're bent at death. Masses from
 * the shapes' sizes.
 */
#define RD_DENSITY 1000.0f // kg/m^3 (bodies are about as dense as water)

static void ragdollShapeFromHitboxes(struct ragdoll *rd, s32 i, pdphysRagdollPart *pp, f32 (*p)[3], f32 (*bone)[3], f32 torso)
{
	const struct rdskel *skel = rd->skel;
	s32 child = skel->child[i];
	s32 parent = skel->parent[i];
	f32 radius = rdHitboxRadius(rd, i);
	f32 localy[3] = { rd->world[i].m[1][0], rd->world[i].m[1][1], rd->world[i].m[1][2] };
	f32 a[3], b[3];
	bool trunk = i <= 2; // pelvis, chest, head: the box's shape

	if (radius <= 0.0f) {
		// No hit box: as thick as the part before it, tapering (tail)
		s32 q = parent;

		while (q > 0 && rdHitboxRadius(rd, q) <= 0.0f) {
			q = skel->parent[q];
		}

		radius = q >= 0 && rdHitboxRadius(rd, q) > 0.0f ? rdHitboxRadius(rd, q) * 0.75f : torso * 0.1f;

		if (i >= 15) {
			s32 k;
			radius = rdHitboxRadius(rd, 15) > 0.0f ? rdHitboxRadius(rd, 15) : torso * 0.12f;

			for (k = 15; k < i; k++) {
				radius *= 0.75f;
			}
		}
	}

	if (trunk && rd->hashitbox[i]) {
		// Along the box's longest side
		const f32 *hb = rd->hitbox[i];
		f32 ext[3] = { hb[3] - hb[0], hb[4] - hb[1], hb[5] - hb[2] };
		f32 mid[3] = { (hb[0] + hb[3]) * 0.5f, (hb[1] + hb[4]) * 0.5f, (hb[2] + hb[5]) * 0.5f };
		s32 ax = ext[0] >= ext[1] && ext[0] >= ext[2] ? 0 : ext[1] >= ext[2] ? 1 : 2;
		f32 half = ext[ax] * 0.5f * rd->scale - radius;
		f32 la[3], lb[3];

		RD_CLAMPLO(half, 0.0f);

		rdVecCopy(mid, la);
		rdVecCopy(mid, lb);
		la[ax] -= half / rd->scale;
		lb[ax] += half / rd->scale;
		rdPartLocalToWorld(rd, i, la, a);
		rdPartLocalToWorld(rd, i, lb, b);
	} else {
		// Along the bone: to the next joint, or to the end of what hangs
		// off it (forearm + hand to the fingertips, tail end to the tip)
		s32 end = child;

		rdVecCopy(p[i], a);

		if (end >= 0 && !skel->hasbody[end] && rd->hastip[end] && (i == 5 || i == 6)) {
			rdVecCopy(rd->tip[end], b);
		} else if (end >= 0) {
			rdVecCopy(p[end], b);
		} else if (rd->hastip[i]) {
			rdVecCopy(rd->tip[i], b);
		} else {
			rdVecMad(p[i], bone[i], radius * 3.0f, b);
		}

		// Not into the joint before it
		rdVecMad(a, bone[i], radius * 0.5f, a);
	}

	ragdollSetCapsule(pp, a, b, radius);
	rd->partradius[i] = radius;

	// Shins: the foot (no body of its own) to the toes
	if ((i == 11 || i == 12) && child >= 0 && rd->hastip[child]) {
		s32 k;

		for (k = 0; k < 3; k++) {
			pp->foota[k] = p[child][k];
			pp->footb[k] = rd->tip[child][k];
		}

		pp->footradius = radius * 0.6f;
	}

	// Mass from the capsule's volume
	{
		f32 d[3];
		f32 len, r;

		rdVecSub(b, a, d);
		len = rdVecLen(d) / 100.0f;
		r = radius / 100.0f;
		pp->mass = RD_DENSITY * (M_PI * r * r * len + 4.0f / 3.0f * M_PI * r * r * r);

		RD_CLAMPLO(pp->mass, 1.0f);
	}

	// Joints
	if (i == 0) {
		pp->joint = PDPHYS_JOINT_NONE;
	} else if (skel->hingesign[i] != 0) {
		// Bends the way it's bent now, about the axis it's bent around
		f32 *n1 = bone[parent];
		f32 *n2 = bone[i];
		f32 cross[3];
		f32 sinang;
		f32 angle;

		rdVecCross(n1, n2, cross);
		sinang = rdVecLen(cross);

		pp->joint = PDPHYS_JOINT_HINGE;
		memcpy(pp->axis2, bone[parent], sizeof(pp->axis2));
		memcpy(pp->axis3, bone[i], sizeof(pp->axis3));

		if (sinang > 0.15f) {
			cross[0] /= sinang; cross[1] /= sinang; cross[2] /= sinang;
			memcpy(pp->axis, cross, sizeof(pp->axis));
			angle = atan2f(sinang, rdVecDot(n1, n2));
			pp->limits[0] = -0.1f;
			pp->limits[1] = angle + 0.05f > 2.4f ? angle + 0.05f : 2.4f;
		} else {
			// Nearly straight: either way
			memcpy(pp->axis, localy, sizeof(pp->axis));
			pp->limits[0] = -1.2f;
			pp->limits[1] = 1.2f;
		}
	} else {
		f32 cone1, cone2, twist;

		switch (i) {
		case 1: cone1 = 0.5f; cone2 = 0.4f; twist = 0.4f; break;        // chest
		case 2: cone1 = 0.6f; cone2 = 0.5f; twist = 0.5f; break;        // head and neck
		case 3: case 4: cone1 = 1.3f; cone2 = 1.1f; twist = 0.8f; break; // shoulders
		case 9: case 10: cone1 = 1.0f; cone2 = 0.6f; twist = 0.4f; break; // hips
		default: cone1 = 0.6f; cone2 = 0.6f; twist = 0.3f; break;       // tail
		}

		ragdollSetSwingTwist(pp, bone[i], localy, cone1, cone2, -twist, twist);
	}
}

/**
 * Builds the physics description of the ragdoll from the captured pose and
 * creates it. Shape sizes scale with the chr's torso length.
 */
s32 ragdollCreatePhysics(struct ragdoll *rd, const f32 *vel)
{
	rd->inwater = false; // a new physics body: the water is looked for again
	RD_CRASHCTX("createphysics", RD_RDCHRNUM(rd), -1);
	pdphysRagdollPart parts[RD_NUMPARTS];
	f32 p[RD_NUMPARTS][3];
	f32 bone[RD_NUMPARTS][3];
	f32 torso;
	s32 numparts = 0;
	s32 i;

	for (i = 0; i < rd->numparts; i++) {
		rdVecCopy(rd->world[i].m[3], p[i]);
	}

	for (i = 0; i < rd->numparts; i++) {
		if (rd->skel->child[i] >= 0) {
			rdVecSub(p[rd->skel->child[i]], p[i], bone[i]);
		} else if (i == 2) {
			rdVecSub(p[2], p[rd->skel->parent[2]], bone[i]); // head points the same way as the spine
		} else if (i == 0) {
			rdVecSub(p[2], p[0], bone[i]);
		} else {
			rdVecSub(p[i], p[rd->skel->parent[i]], bone[i]);
		}

		rdVecNorm(bone[i]);
	}

	torso = rdTorsoLength(rd);

	for (i = 0; i < rd->numparts; i++) {
		pdphysRagdollPart *pp;
		f32 a[3];
		f32 b[3];
		f32 localy[3];
		f32 localz[3];
		s32 c;

		if (rd->bodyindex[i] < 0) {
			continue;
		}

		pp = &parts[rd->bodyindex[i]];
		memset(pp, 0, sizeof(*pp));
		numparts++;

		pp->parent = i == 0 ? -1 : rd->bodyindex[rd->skel->parent[i]];

		for (c = 0; c < 4; c++) {
			pp->jointmtx[c * 4 + 0] = rd->world[i].m[c][0];
			pp->jointmtx[c * 4 + 1] = rd->world[i].m[c][1];
			pp->jointmtx[c * 4 + 2] = rd->world[i].m[c][2];
			pp->jointmtx[c * 4 + 3] = rd->world[i].m[c][3];
		}

		rdVecCopy(rd->world[i].m[1], localy);
		rdVecCopy(rd->world[i].m[2], localz);

		rdVecCopy(vel, pp->linvel);

		{
			f32 sc = torso / RD_MASS_REFTORSO;

			pp->mass = rd->skel->mass[i] * sc * sc * sc;
		}

		if (!rd->skel->human) {
			ragdollShapeFromHitboxes(rd, i, pp, p, bone, torso);
			continue;
		}

		switch (i) {
		case 0: // pelvis: across the hips
			ragdollSetCapsule(pp, p[9], p[10], torso * 0.25f);
			pp->joint = PDPHYS_JOINT_NONE;
			break;
		case 1: // chest
			rdVecMad(p[1], bone[1], torso * 0.30f, a);
			rdVecMad(p[2], bone[1], -torso * 0.15f, b);
			ragdollSetCapsule(pp, a, b, torso * 0.25f);
			// Lower back: bends ~35 degrees, sideways ~25, twists ~25
			ragdollSetSwingTwist(pp, bone[1], localz, 0.6f, 0.45f, -0.45f, 0.45f);
			break;
		case 2: // head
			pp->shape = PDPHYS_SHAPE_SPHERE;
			rdVecMad(p[2], bone[1], torso * 0.30f, pp->a);
			pp->radius = torso * 0.26f;
			// Lying on its side, the shoulder holds the chest up: the neck needs
			// ~45 degrees sideways for the head to reach the floor
			// Nodding ~50 degrees, twisting ~60
			ragdollSetSwingTwist(pp, bone[1], localz, 0.85f, 0.8f, -1.0f, 1.0f);
			break;
		case 3: // upper arms
		case 4:
			rdVecMad(p[i], bone[i], torso * 0.11f, a);
			ragdollSetCapsule(pp, a, p[rd->skel->child[i]], torso * 0.12f);
			// Shoulders: the most mobile joint (~85 / 75 degrees of swing
			// here, kept short of the full range so arms don't wrap round),
			// rotating ~50 degrees each way
			ragdollSetSwingTwist(pp, bone[i], localy, 1.5f, 1.3f, -0.9f, 0.9f);
			break;
		case 5: // forearms (+ hand)
		case 6:
			rdVecMad(p[rd->skel->child[i]], bone[i], torso * 0.18f, b);
			ragdollSetCapsule(pp, p[i], b, torso * 0.10f);
			break;
		case 9: // thighs
		case 10:
			ragdollSetCapsule(pp, p[i], p[rd->skel->child[i]], torso * 0.17f);
			// Normal cone = forward/back swing, plane cone = sideways (localy
			// is the knee's hinge axis, i.e. the hip's left-right axis)
			// Hips: ~70 degrees forward/back, ~30 sideways, ~30 rotation
			ragdollSetSwingTwist(pp, bone[i], localy, 1.2f, 0.5f, -0.5f, 0.5f);
			break;
		case 11: // shins
		case 12:
			ragdollSetCapsule(pp, p[i], p[rd->skel->child[i]], torso * 0.14f);

			// The foot (no body of its own): a capsule from the heel to the
			// toes, along the foot's forward (Z) axis and down to the sole.
			// The shin's capsule only reached the ankle.
			{
				s32 foot = rd->skel->child[i];
				const f32 *fwd = rd->world[foot].m[2];
				const f32 *up = rd->world[foot].m[1];
				s32 k;

				for (k = 0; k < 3; k++) {
					pp->foota[k] = p[foot][k] - up[k] * torso * 0.10f - fwd[k] * torso * 0.04f;
					pp->footb[k] = p[foot][k] - up[k] * torso * 0.12f + fwd[k] * torso * 0.36f;
				}

				pp->footradius = torso * 0.07f;
			}
			break;
		}

		// Elbows and knees are hinges around the joint's local Y axis.
		// Zero angle = straight limb; limits allow bending one way only.
		if (rd->skel->hingesign[i] != 0) {
			s32 parent = rd->skel->parent[i];
			f32 maxbend = (i == 5 || i == 6) ? 2.55f : 2.45f; // elbows ~145 degrees, knees ~140
			f32 n1[3];
			f32 n2[3];
			f32 cross[3];
			f32 angle;

			pp->joint = PDPHYS_JOINT_HINGE;
			memcpy(pp->axis, localy, sizeof(pp->axis));
			memcpy(pp->axis2, bone[parent], sizeof(pp->axis2));
			memcpy(pp->axis3, bone[i], sizeof(pp->axis3));

			if (rd->skel->hingesign[i] > 0) {
				pp->limits[0] = -0.05f;
				pp->limits[1] = maxbend;
			} else {
				pp->limits[0] = -maxbend;
				pp->limits[1] = 0.05f;
			}

			// Make sure the current pose is inside the limits
			rdVecCopy(bone[parent], n1);
			rdVecCopy(bone[i], n2);
			rdVecCross(n1, n2, cross);
			angle = atan2f(rdVecDot(cross, localy),
					rdVecDot(n1, n2));

			// Death pose slightly outside the limits: allow a little, but
			// never let a knee or elbow bend more than ~15 degrees backwards
			if (angle < pp->limits[0]) {
				pp->limits[0] = angle - 0.05f;

				if (rd->skel->hingesign[i] > 0 && pp->limits[0] < -0.25f) {
					pp->limits[0] = -0.25f; // backwards bend
				}
			}

			if (angle > pp->limits[1]) {
				pp->limits[1] = angle + 0.05f;

				if (rd->skel->hingesign[i] < 0 && pp->limits[1] > 0.25f) {
					pp->limits[1] = 0.25f; // backwards bend
				}
			}
		}
	}

	// Dismembered joints stay cut
	for (i = 1; i < rd->numparts; i++) {
		if ((rd->severed & (1u << i)) && rd->bodyindex[i] >= 0) {
			parts[rd->bodyindex[i]].joint = PDPHYS_JOINT_NONE;
		}
	}

	return pdphysRagdollCreate(parts, numparts);
}

#endif // PD_PHYSICS
