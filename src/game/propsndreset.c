#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "lib/memp.h"
#include "data.h"
#include "types.h"

#ifndef PLATFORM_N64
#define PSCHANNELCOUNT() 160 // MOD: sound capacity (4 x 40)
#else
#define PSCHANNELCOUNT() (IS4MB() ? 30 : 40)
#endif

void psReset(void)
{
	s32 i;

	g_PsChannels = mempAlloc(ALIGN16(PSCHANNELCOUNT() * sizeof(struct pschannel)), MEMPOOL_STAGE);

	for (i = 0; i < PSCHANNELCOUNT(); i++) {
		g_PsChannels[i].flags = PSFLAG_FREE;
		g_PsChannels[i].audiohandle = NULL;
		g_PsChannels[i].distance = -1;
	}
}
