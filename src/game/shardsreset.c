#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "lib/memp.h"
#include "data.h"
#include "types.h"

void shardsReset(void)
{
	s32 i;

#ifndef PLATFORM_N64
	// MOD: co-op effects - as many glass shards in co-op/counter-op as solo
	g_MaxShards = g_Vars.normmplayerisrunning ? 200 / PLAYERCOUNT() : 200;
#else
	g_MaxShards = 200 / PLAYERCOUNT();
#endif

	if (IS4MB()) {
		g_MaxShards = g_MaxShards / 2;
	}

	if (g_Vars.stagenum >= STAGE_TITLE) {
		g_MaxShards = 0;
	}

	if (g_MaxShards == 0) {
		g_Shards = NULL;
	} else {
		g_Shards = mempAlloc((g_MaxShards * sizeof(struct shard) + 0xf) & ~0xf, MEMPOOL_STAGE);

		for (i = 0; i < g_MaxShards; i++) {
			g_Shards[i].age60 = 0;
		}
	}

	g_NextShardNum = 0;
}
