/**
 * MOD: blood wrap - blood splats on the level carry on round corners.
 *
 * A splat is one square decal on the surface that was hit. With the decal
 * edge clamp (Video.DecalClamp) the part hanging past the surface's edge is
 * cut off, which looks unnatural at a corner. Here the surface around the
 * splat is probed with the physics raycast (the level's collision and
 * drawn surfaces): where the splat runs over an edge (outer corner) or into
 * a wall (inner corner), a second splat goes on the other surface, sized to
 * what hangs over. The clamp then trims each to its own surface.
 *
 * Called from splat.c after a splat on the level (not on objects).
 */
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "game/wallhit.h"
#include "lib/rng.h"

#ifdef PD_PHYSICS
#include "pdphysics.h"

#define BW_MINOVER 2.0f // smaller overhangs aren't worth a splat

static void bwNorm(f32 *v)
{
	f32 len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (len > 0.0001f) {
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
}

static f32 bwDot(const f32 *a, const f32 *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void bwSplat(const f32 *pos, const f32 *normal, f32 size, s16 texnum, s32 room,
		struct prop *chrprop, struct chrdata *chr, u8 minalpha, u8 maxalpha,
		u32 timermax, u32 timerspeed, bool xlu)
{
	struct coord p;
	struct coord n;

	p.x = pos[0] + normal[0] * 0.5f;
	p.y = pos[1] + normal[1] * 0.5f;
	p.z = pos[2] + normal[2] * 0.5f;
	n.x = normal[0];
	n.y = normal[1];
	n.z = normal[2];

	wallhitCreateWith20Args(&p, &n, chr ? &chr->prop->pos : NULL, NULL,
			NULL, texnum, room, NULL,
			chrprop, -1, 0, chr,
			size, size, minalpha, maxalpha,
			rngRandom() % 360, timermax, timerspeed, xlu);
}

void bloodWrapSplat(struct coord *pos, struct coord *normal, f32 width, f32 height, s16 texnum, RoomNum room,
		struct prop *chrprop, struct chrdata *chr, u8 minalpha, u8 maxalpha, u32 timermax, u32 timerspeed, bool xlu)
{
	f32 n[3] = { normal->x, normal->y, normal->z };
	f32 c[3] = { pos->x, pos->y, pos->z };
	f32 t1[3], t2[3], dirs[4][3];
	f32 r = 0.5f * (width > height ? width : height);
	s32 i;

	if (!pdphysIsReady() || r < 4.0f) {
		return;
	}

	bwNorm(n);

	// The hit surface's normal can face either way: towards the body that
	// bled (or the surface's front, failing that)
	if (chr && chr->prop) {
		f32 to[3] = { chr->prop->pos.x - c[0], chr->prop->pos.y - c[1], chr->prop->pos.z - c[2] };

		if (bwDot(to, n) < 0.0f) {
			n[0] = -n[0];
			n[1] = -n[1];
			n[2] = -n[2];
		}
	}

	// Two directions along the surface
	if (fabsf(n[1]) < 0.9f) {
		t1[0] = n[2]; t1[1] = 0.0f; t1[2] = -n[0];
	} else {
		t1[0] = 1.0f; t1[1] = 0.0f; t1[2] = 0.0f;
		t1[0] -= n[0] * bwDot(t1, n);
		t1[1] -= n[1] * bwDot(t1, n);
		t1[2] -= n[2] * bwDot(t1, n);
	}

	bwNorm(t1);
	t2[0] = n[1] * t1[2] - n[2] * t1[1];
	t2[1] = n[2] * t1[0] - n[0] * t1[2];
	t2[2] = n[0] * t1[1] - n[1] * t1[0];

	for (i = 0; i < 3; i++) {
		dirs[0][i] = t1[i];
		dirs[1][i] = -t1[i];
		dirs[2][i] = t2[i];
		dirs[3][i] = -t2[i];
	}

	for (i = 0; i < 4; i++) {
		const f32 *d = dirs[i];
		f32 from[3], to[3], hit[3], hitn[3];
		s32 hitroom = -1;
		s32 k;

		// Inner corner: a wall rising from the surface within the splat
		for (k = 0; k < 3; k++) {
			from[k] = c[k] + n[k] * 2.0f;
			to[k] = from[k] + d[k] * r;
		}

		if (pdphysRaycast(from, to, hit, hitn, &hitroom)) {
			f32 dist = (hit[0] - from[0]) * d[0] + (hit[1] - from[1]) * d[1] + (hit[2] - from[2]) * d[2];
			f32 over = r - dist;

			if (bwDot(hitn, d) < -0.5f && over >= BW_MINOVER) {
				bwSplat(hit, hitn, over * 2.0f, texnum, hitroom > 0 ? hitroom : room,
						chrprop, chr, minalpha, maxalpha, timermax, timerspeed, xlu);
			}

			continue;
		}

		// Does the surface carry on to the splat's edge?
		for (k = 0; k < 3; k++) {
			from[k] = c[k] + d[k] * r * 0.9f + n[k] * 3.0f;
			to[k] = c[k] + d[k] * r * 0.9f - n[k] * 3.0f;
		}

		if (pdphysRaycast(from, to, hit, hitn, &hitroom) && bwDot(hitn, n) > 0.9f) {
			continue;
		}

		// Outer corner: the face round the edge, found from beyond it and
		// below the surface, looking back towards the splat
		for (k = 0; k < 3; k++) {
			from[k] = c[k] + d[k] * r - n[k] * (r * 0.5f);
			to[k] = c[k] - n[k] * (r * 0.5f);
		}

		if (pdphysRaycast(from, to, hit, hitn, &hitroom) && bwDot(hitn, d) > 0.5f) {
			f32 edge = (hit[0] - c[0]) * d[0] + (hit[1] - c[1]) * d[1] + (hit[2] - c[2]) * d[2];
			f32 over = r - edge;
			f32 at[3];

			if (over >= BW_MINOVER) {
				// Up the face to the edge: half the splat hangs down the face
				for (k = 0; k < 3; k++) {
					at[k] = hit[k] + n[k] * (r * 0.5f);
				}

				bwSplat(at, hitn, over * 2.0f, texnum, hitroom > 0 ? hitroom : room,
						chrprop, chr, minalpha, maxalpha, timermax, timerspeed, xlu);
			}
		}
	}
}
#else
void bloodWrapSplat(struct coord *pos, struct coord *normal, f32 width, f32 height, s16 texnum, RoomNum room,
		struct prop *chrprop, struct chrdata *chr, u8 minalpha, u8 maxalpha, u32 timermax, u32 timerspeed, bool xlu)
{
}
#endif
