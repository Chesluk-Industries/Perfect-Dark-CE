#include <ultra64.h>
#include "constants.h"
#include "../lib/naudio/n_sndp.h"
#include "game/casing.h"
#include "bss.h"
#include "lib/snd.h"
#include "lib/rng.h"
#include "lib/mtx.h"
#include "lib/lib_317f0.h"
#include "data.h"
#include "types.h"
#ifdef PD_GRAVITY
#include "gravity.h" // MOD: gravity
#endif

#ifdef PD_GRAVITY
// MOD: gravity - how long each casing has been in the air (240ths), by slot.
// A slot not ticked last frame holds a new casing.
static s32 g_CasingAge240[ARRAYCOUNT(g_Casings)];
static s32 g_CasingLastFrame[ARRAYCOUNT(g_Casings)];
#endif

void casingTick(struct casing *casing)
{
	f32 tmp;
	f32 tmp2;
	f32 sp8c[3][3];
	f32 sp68[3][3];
	s32 i;
	s32 j;
	f32 lvupdate = g_Vars.lvupdate60freal;
	f32 sp58;

	tmp2 = lvupdate * (1.0f / 3.6f);
#ifdef PD_GRAVITY
	if (casing >= g_Casings && casing < g_Casings + ARRAYCOUNT(g_Casings)) {
		s32 index = casing - g_Casings;

		if (g_CasingLastFrame[index] != g_Vars.lvframenum - 1 && g_CasingLastFrame[index] != g_Vars.lvframenum) {
			g_CasingAge240[index] = 0;
		}

		g_CasingLastFrame[index] = g_Vars.lvframenum;
		g_CasingAge240[index] += g_Vars.lvupdate240;
	}

	tmp2 *= gravityScale(); // MOD: gravity
#endif
	tmp = casing->speed.y - tmp2;
	casing->pos.y += lvupdate * 0.5f * (casing->speed.y + tmp);

#ifdef PD_GRAVITY
	// MOD: gravity - with no gravity or inverted, a casing rises for good: it
	// vanishes once well above the floor it would have landed on (it doesn't
	// collide with ceilings)
	if (gravityScale() <= 0.0f && casing->pos.y > casing->ground + 400.0f) {
		casing->modeldef = NULL;
		return;
	}
#endif

	if (casing->pos.y < casing->ground) {
		i = -1;

#ifdef PD_GRAVITY
		// MOD: gravity - in low gravity casings fall for a long time and the
		// landing sound (not positional: it plays as if at your feet) kept
		// going long after the shooting stopped, wherever you went. One that
		// was in the air longer than they ever are in the original lands
		// quietly.
		if (gravityScale() < 1.0f && casing >= g_Casings && casing < g_Casings + ARRAYCOUNT(g_Casings)
				&& g_CasingAge240[casing - g_Casings] > TICKS(360)) {
			casing->modeldef = NULL;
			return;
		}
#endif

		if (var8009d0d8 == 0) {
			if (g_CasingAudioHandles[0] == NULL) {
				i = 0;
			}

			if (g_CasingAudioHandles[1] == NULL) {
				i = 1;
			}
		}

		if (g_Vars.lvupdate240 > 0
				&& i >= 0
				&& g_Vars.currentplayer->hands[0].mode != HANDMODE_2
				&& g_Vars.currentplayer->hands[1].mode != HANDMODE_2) {
			sp58 = RANDOMFRAC() * 0.25f + 0.98f;

			var8009d0d8 = TICKS(20);
			sndStart(var80095200, SFX_8051, &g_CasingAudioHandles[i], -1, -1, -1.0f, -1, -1);

			if (g_CasingAudioHandles[i]) {
				audioPostEvent(g_CasingAudioHandles[i], AL_SNDP_PITCH_EVT, *(s32 *)&sp58);
			}
		}

		casing->modeldef = NULL;
		return;
	}

	casing->speed.y = tmp;
	casing->pos.x += lvupdate * casing->speed.x;
	casing->pos.z += lvupdate * casing->speed.z;

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			sp8c[i][j] = casing->rotspeed[i][j] * (1.0f / 4096.0f);
			sp68[i][j] = casing->rot[i][j] * (1.0f / 4096.0f);
		}
	}

	for (i = 0; i < g_Vars.lvupdate240; i++) {
		mtx00016110(sp8c, sp68);
	}

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			casing->rot[i][j] = sp68[i][j] * 4096.0f;
		}
	}
}

void casingsTick(void)
{
	s32 i;
	struct casing *end;
	struct casing *casing;

	if (var8009d0d8 > 0) {
		var8009d0d8 -= g_Vars.lvupdate240;
	}

	if (var8009d0d8 < 0) {
		var8009d0d8 = 0;
	}

	if (g_CasingsActive) {
		g_CasingsActive = false;

		end = g_Casings + ARRAYCOUNT(g_Casings);
		casing = g_Casings;

		while (casing < end) {
			if (casing->modeldef) {
				casingTick(casing);

				if (!g_CasingsActive) {
					g_CasingsActive = true;
				}
			}

			casing++;
		}
	}
}
