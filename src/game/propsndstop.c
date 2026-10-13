#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "lib/snd.h"
#include "lib/lib_317f0.h"
#include "data.h"
#include "types.h"

#ifndef PLATFORM_N64
#define PSCHANNELCOUNT() 160 // MOD: sound capacity (4 x 40)
#else
#define PSCHANNELCOUNT() (IS4MB() ? 30 : 40)
#endif

void psStop(void)
{
	if (g_SndDisabled) return;
	s32 i;

	for (i = 0; i < PSCHANNELCOUNT(); i++) {
		if (g_PsChannels[i].flags & PSFLAG_ISMP3) {
			sndStopMp3(g_PsChannels[i].soundnum26);
			g_PsChannels[i].flags &= ~PSFLAG_ISMP3;
			g_PsChannels[i].flags &= ~PSFLAG_REPEATING;
		} else if (g_PsChannels[i].audiohandle && sndGetState(g_PsChannels[i].audiohandle) != AL_STOPPED) {
			audioStop(g_PsChannels[i].audiohandle);
			g_PsChannels[i].flags &= ~PSFLAG_REPEATING;
		}
	}
}
