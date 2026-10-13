#ifndef _IN_AUDIO_H
#define _IN_AUDIO_H

#include <PR/ultratypes.h>

s32 audioInit(void);
s32 audioGetBytesBuffered(void);
s32 audioGetSamplesBuffered(void);
void audioSetNextBuffer(const s16 *buf, u32 len);
void audioEndFrame(void);

// MOD: surround 5.1
s32 audioGetSurround51(void);
void audioSetSurround51(s32 enabled);
s32 audioGetOutputChannels(void);
s32 audioGetDeviceChannels(void);
void audioStartSpeakerTest(void);
void audioStopSpeakerTest(void);
const char *audioGetSpeakerTestName(void);

// MOD: speaker volumes (speaker: 0 front left, 1 front right, 2 centre, 3 subwoofer, 4 rear left, 5 rear right)
s32 audioGetSpeakerVolume(s32 speaker);
void audioSetSpeakerVolume(s32 speaker, s32 percent);

#endif
