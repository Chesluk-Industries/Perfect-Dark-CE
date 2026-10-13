#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "lib/memp.h"
#include "data.h"
#include "types.h"
#ifdef PD_PHYSICS
#include "ragdoll.h" // MOD: ragdoll physics
#endif

void vtxstoreReset(void)
{
	s32 i;
	s32 j;
	s32 num;
	s32 val;

	for (i = 0; i < ARRAYCOUNT(g_VtxstoreTypes); i++) {
#ifndef PLATFORM_N64
		// MOD: co-op effects - co-op/counter-op get the solo budgets: corpses
		// don't vanish early
		if (PLAYERCOUNT() >= 2 && g_Vars.normmplayerisrunning) {
#else
		if (PLAYERCOUNT() >= 2) {
#endif
			val = g_VtxstoreTypes[i].valifmp;
			num = g_VtxstoreTypes[i].numifmp;
		} else if (g_Vars.stagenum >= STAGE_TITLE) {
			val = g_VtxstoreTypes[i].valifspecial;
			num = g_VtxstoreTypes[i].numifspecial;
		} else {
			val = g_VtxstoreTypes[i].valifsp;
			num = g_VtxstoreTypes[i].numifsp;
#ifdef PD_PHYSICS

			// MOD: ragdoll physics - many more corpses are kept, and each shot
			// one holds a copy of its vertices (bullet damage). The original
			// budget ran out after a handful of bodies and then faded corpses
			// as soon as the player looked away. See RD_MAINMEMA_EXTRA.
			if (g_RagdollsEnabled && (i == VTXSTORETYPE_CHRVTX || i == VTXSTORETYPE_CHRCOL)) {
				val *= 12;
				num *= 8;
			}
#endif
		}

		if (num > 0) {
			g_VtxstoreTypes[i].unk24 = mempAlloc(num * sizeof(struct var8007e3d0_data), MEMPOOL_STAGE);
		}

		for (j = 0; j < num; j++) {
			g_VtxstoreTypes[i].unk24[j].unk0e = 0;
		}

		g_VtxstoreTypes[i].numallocated = num;
		g_VtxstoreTypes[i].val1 = val;
		g_VtxstoreTypes[i].val2 = val;
	}
}
