#include <PR/ultratypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "system.h"
#include "mixer.h" // MOD: surround 5.1
#include "constants.h" // MOD: surround 5.1 - SOUNDMODE_SURROUND
#ifdef PD_PHYSICS
#include <string.h>
#include "ragdoll.h" // MOD: ragdoll physics - the ragdolls' WAV sounds
#endif

extern s32 g_SoundMode; // MOD: surround 5.1 - the game's Sound option

static SDL_AudioDeviceID dev = 0;
static const s16 *nextBuf;
static u32 nextSize = 0;

static s32 bufferSize = 512;
static s32 queueLimit = 8192;

/**
 * MOD: surround 5.1 - with Audio.Surround51 on, the output device is opened
 * with 6 channels (front left/right, centre, LFE, rear left/right). The game's
 * stereo mix goes to the front; in the game's Surround sound mode, sounds
 * behind the player go to the rear channels (see mixer.c). Centre and LFE are
 * left silent. If the device has fewer speakers, SDL downmixes.
 */
static s32 surround51 = 1; // (used only when the device has 6 or more channels)
static s32 outChannels = 2;
static s16 *outBuf51 = NULL;
static u32 outBuf51Frames = 0;
static s16 *rearBuf = NULL;

static s32 deviceChannels = 2;
static s32 deviceProbed = 0;
static s32 openSoundMode = -1; // the game's Sound option when the device was opened

/**
 * MOD: speaker volumes - a volume per speaker (percent, 0-150). In 5.1 the
 * centre and subwoofer are upmixed from the game's mix: the centre gets what
 * front left and right share (their average, at -3 dB), the subwoofer the
 * bass (below ~120 Hz) of all four channels. In stereo only the front left
 * and right volumes apply. Applied last, so the speaker test follows them.
 */
enum { SPK_FL, SPK_FR, SPK_C, SPK_LFE, SPK_RL, SPK_RR, SPK_COUNT };
static s32 speakerVolume[SPK_COUNT] = { 100, 100, 100, 100, 100, 100 };
static f32 lfeState[2] = { 0, 0 }; // two one-pole low-pass stages
#define LFE_ALPHA 0.0337f          // 1 - exp(-2 pi 120 / 22020)
static s16 *volBuf = NULL;
static u32 volBufFrames = 0;

// MOD: speaker test - noise bursts on each speaker in turn
static s32 speakerTestStep = -1;   // index into the order, -1 when off
static s32 speakerTestFrames = 0;  // frames played of the current step
static u32 speakerTestSeed = 12345;
static f32 speakerTestFilter = 0;
static s16 *testBuf = NULL;
#ifdef PD_PHYSICS
static s16 *wavBuf = NULL; // MOD: ragdoll physics - the game's output with the ragdolls' WAV sounds
static u32 wavBufFrames = 0;
#endif
static u32 testBufFrames = 0;

#define SPEAKERTEST_ON_FRAMES  (22020 * 3 / 2) // 1.5 seconds of noise
#define SPEAKERTEST_OFF_FRAMES (22020 / 3)     // then a third of a second of silence

// The order receivers use: front left, centre, front right, rear right, rear left, subwoofer
static const s32 speakerTestOrder51[] = { 0, 2, 1, 5, 4, 3 };
static const char *speakerTestNames51[] = { "Front Left", "Centre", "Front Right", "Rear Right", "Rear Left", "Subwoofer" };
static const s32 speakerTestOrder20[] = { 0, 1 };
static const char *speakerTestNames20[] = { "Left", "Right" };

// MOD: surround 5.1 - 5.1 output only with the game's Sound option on
// Surround, the 5.1 setting on and a device with 6+ speakers. Otherwise
// stereo: with Surround, Dolby Surround encoded (as on the N64); with Stereo,
// Headphone or Mono, the game's plain mix (no levels, no upmix).
static s32 audioWantChannels(void)
{
	return (g_SoundMode == SOUNDMODE_SURROUND && surround51 && deviceChannels >= 6) ? 6 : 2;
}

/**
 * MOD: Dolby Surround - stereo output in the Surround sound mode: sounds
 * behind the player go out of phase between left and right (as on the N64),
 * so a Pro Logic decoder sends them to the rear speakers.
 */
static s32 audioDolby(void)
{
	return outChannels == 2 && g_SoundMode == SOUNDMODE_SURROUND;
}

static void audioApplyRouting(void)
{
	mixerSetRear51(outChannels == 6);
	mixerSetDolby(audioDolby());
}

static s32 audioOpenDevice(void)
{
	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 22020; // TODO: this might cause trouble for some platforms
	want.format = AUDIO_S16SYS;
	outChannels = 2;

	if (!deviceProbed) {
		// Find how many speakers the device has
		SDL_AudioSpec probe;
		SDL_AudioDeviceID probedev;

		deviceChannels = 2;
		want.channels = 6;
		probedev = SDL_OpenAudioDevice(NULL, 0, &want, &probe, SDL_AUDIO_ALLOW_CHANNELS_CHANGE);

		if (probedev != 0) {
			deviceChannels = probe.channels;
			SDL_CloseAudioDevice(probedev);
		}

		deviceProbed = 1;
	}

	// Only use 6 channels if the device has at least that many speakers
	// (otherwise SDL's downmix would make the stereo mix quieter)
	outChannels = audioWantChannels();
	openSoundMode = g_SoundMode;

	want.channels = outChannels;
	want.samples = bufferSize;
	want.callback = NULL;

	nextBuf = NULL;

	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (dev == 0) {
		sysLogPrintf(LOG_ERROR, "SDL_OpenAudio error: %s", SDL_GetError());
		return -1;
	}

	audioApplyRouting();

	sysLogPrintf(LOG_NOTE, "AUDIO: %d output channels (device has %d, sound mode %d%s)", outChannels, deviceChannels,
			g_SoundMode, audioDolby() ? ", Dolby Surround" : "");

	SDL_PauseAudioDevice(dev, 0);

	return 0;
}

s32 audioInit(void)
{
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		sysLogPrintf(LOG_ERROR, "SDL audio init error: %s", SDL_GetError());
		return -1;
	}

	return audioOpenDevice();
}

s32 audioGetSurround51(void)
{
	return surround51;
}

// Turn 5.1 output on or off, reopening the device
void audioSetSurround51(s32 enabled)
{
	if (surround51 == enabled) {
		return;
	}

	surround51 = enabled;

	if (dev != 0) {
		SDL_CloseAudioDevice(dev);
		dev = 0;
	}

	speakerTestStep = -1;
	audioOpenDevice();
}

s32 audioGetOutputChannels(void)
{
	return outChannels;
}

s32 audioGetDeviceChannels(void)
{
	return deviceChannels;
}

// MOD: speaker volumes (speaker: 0 front left, 1 front right, 2 centre, 3 subwoofer, 4 rear left, 5 rear right)
s32 audioGetSpeakerVolume(s32 speaker)
{
	return speaker >= 0 && speaker < SPK_COUNT ? speakerVolume[speaker] : 100;
}

void audioSetSpeakerVolume(s32 speaker, s32 percent)
{
	if (speaker >= 0 && speaker < SPK_COUNT) {
		speakerVolume[speaker] = percent < 0 ? 0 : percent > 150 ? 150 : percent;
	}
}

void audioStartSpeakerTest(void)
{
	speakerTestStep = 0;
	speakerTestFrames = 0;
}

void audioStopSpeakerTest(void)
{
	speakerTestStep = -1;
}

// The speaker being tested, or NULL if the test isn't running
const char *audioGetSpeakerTestName(void)
{
	if (speakerTestStep < 0) {
		return NULL;
	}

	return outChannels == 6 ? speakerTestNames51[speakerTestStep] : speakerTestNames20[speakerTestStep];
}

// Replace the output with the speaker test
static void audioApplySpeakerTest(s16 *out, u32 frames)
{
	s32 numsteps = outChannels == 6 ? 6 : 2;
	u32 i;

	for (i = 0; i < frames; i++) {
		s32 c;

		for (c = 0; c < outChannels; c++) {
			out[i * outChannels + c] = 0;
		}

		if (speakerTestStep < 0) {
			continue;
		}

		if (speakerTestFrames < SPEAKERTEST_ON_FRAMES) {
			s32 channel = outChannels == 6 ? speakerTestOrder51[speakerTestStep] : speakerTestOrder20[speakerTestStep];
			f32 white;
			f32 sample;

			speakerTestSeed = speakerTestSeed * 1664525 + 1013904223;
			white = (s32)(speakerTestSeed >> 16) / 32768.0f - 1.0f;

			if (outChannels == 6 && channel == 3) {
				// subwoofer: low rumble
				speakerTestFilter += (white - speakerTestFilter) * 0.02f;
				sample = speakerTestFilter * 4.0f;
			} else {
				// softened noise
				speakerTestFilter += (white - speakerTestFilter) * 0.5f;
				sample = speakerTestFilter;
			}

			if (sample > 1.0f) {
				sample = 1.0f;
			} else if (sample < -1.0f) {
				sample = -1.0f;
			}

			out[i * outChannels + channel] = (s16)(sample * 9000.0f);
		}

		speakerTestFrames++;

		if (speakerTestFrames >= SPEAKERTEST_ON_FRAMES + SPEAKERTEST_OFF_FRAMES) {
			speakerTestFrames = 0;
			speakerTestStep++;

			if (speakerTestStep >= numsteps) {
				speakerTestStep = -1;
			}
		}
	}
}

s32 audioGetBytesBuffered(void)
{
	// (in stereo terms, as the game expects)
	return SDL_GetQueuedAudioSize(dev) * 2 / outChannels;
}

s32 audioGetSamplesBuffered(void)
{
	return audioGetBytesBuffered() / 4;
}

// MOD: surround 5.1 - the game's Sound option changed: switch output to match
static void audioFollowSoundMode(void)
{
	if (dev == 0 || g_SoundMode == openSoundMode) {
		return;
	}

	openSoundMode = g_SoundMode;

	if (audioWantChannels() != outChannels) {
		SDL_CloseAudioDevice(dev);
		dev = 0;
		speakerTestStep = -1;
		audioOpenDevice();
	} else {
		audioApplyRouting();
		sysLogPrintf(LOG_NOTE, "AUDIO: sound mode %d%s", g_SoundMode, audioDolby() ? ", Dolby Surround" : "");
	}
}

void audioSetNextBuffer(const s16 *buf, u32 len)
{
#ifdef PD_PHYSICS
	s32 wav = buf && len && rdWavActive();

#endif
	audioFollowSoundMode();

#ifdef PD_PHYSICS
	if (wav) {
		// MOD: ragdoll physics - the ragdolls' WAV sounds added to the game's
		// (with 5.1, below: their rear part goes to the rear)
		u32 frames = len / 4;

		if (frames > wavBufFrames) {
			wavBuf = realloc(wavBuf, frames * 2 * sizeof(s16));
			wavBufFrames = frames;
		}

		memcpy(wavBuf, buf, frames * 2 * sizeof(s16));
		buf = wavBuf;

		if (outChannels != 6) {
			rdWavMix(wavBuf, NULL, frames, audioDolby());
		}
	}

#endif
	if (outChannels == 6 && buf && len) {
		// MOD: surround 5.1 - widen to 6 channels, with the rear from the mixer
		u32 frames = len / 4;
		u32 i;

		if (frames > outBuf51Frames) {
			outBuf51 = realloc(outBuf51, frames * 6 * sizeof(s16));
			rearBuf = realloc(rearBuf, frames * 2 * sizeof(s16));
			outBuf51Frames = frames;
		}

		mixerRearPop(rearBuf, frames);

#ifdef PD_PHYSICS
		if (wav) {
			rdWavMix(wavBuf, rearBuf, frames, 0);
		}

#endif
		for (i = 0; i < frames; i++) {
			s32 l = buf[i * 2 + 0];
			s32 r = buf[i * 2 + 1];
			s32 rl = rearBuf[i * 2 + 0];
			s32 rr = rearBuf[i * 2 + 1];
			f32 lfe;

			// MOD: speaker volumes - centre and subwoofer upmixed (see the top)
			lfeState[0] += ((l + r + rl + rr) * 0.25f - lfeState[0]) * LFE_ALPHA;
			lfeState[1] += (lfeState[0] - lfeState[1]) * LFE_ALPHA;
			lfe = lfeState[1] * 2.0f;

			outBuf51[i * 6 + 0] = l;
			outBuf51[i * 6 + 1] = r;
			outBuf51[i * 6 + 2] = (s16)((l + r) * 0.3536f); // (l + r) / 2 at -3 dB
			outBuf51[i * 6 + 3] = (s16)(lfe > 32767.0f ? 32767.0f : lfe < -32768.0f ? -32768.0f : lfe);
			outBuf51[i * 6 + 4] = rl;
			outBuf51[i * 6 + 5] = rr;
		}

		buf = outBuf51;
		len = frames * 6 * sizeof(s16);
	}

	if (speakerTestStep >= 0 && buf && len) {
		// MOD: speaker test - replaces the game's sound while it runs
		u32 frames = len / (outChannels * sizeof(s16));

		if (frames > testBufFrames) {
			testBuf = realloc(testBuf, frames * 6 * sizeof(s16));
			testBufFrames = frames;
		}

		audioApplySpeakerTest(testBuf, frames);
		buf = testBuf;
	}

	if (buf && len && g_SoundMode == SOUNDMODE_SURROUND && deviceChannels >= 6) {
		// MOD: speaker volumes - each channel at its speaker's volume (Surround,
		// on a device with 5.1 speakers: the levels can't be set on a stereo one)
		static const s32 chan20[] = { SPK_FL, SPK_FR };
		static const s32 chan51[] = { SPK_FL, SPK_FR, SPK_C, SPK_LFE, SPK_RL, SPK_RR };
		const s32 *chan = outChannels == 6 ? chan51 : chan20;
		u32 frames = len / (outChannels * sizeof(s16));
		s32 allfull = 1;
		s32 c;

		for (c = 0; c < outChannels; c++) {
			if (speakerVolume[chan[c]] != 100) {
				allfull = 0;
			}
		}

		if (!allfull) {
			u32 i;

			if (frames > volBufFrames) {
				volBuf = realloc(volBuf, frames * 6 * sizeof(s16));
				volBufFrames = frames;
			}

			for (i = 0; i < frames; i++) {
				for (c = 0; c < outChannels; c++) {
					s32 v = buf[i * outChannels + c] * speakerVolume[chan[c]] / 100;
					volBuf[i * outChannels + c] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
				}
			}

			buf = volBuf;
		}
	}

	nextBuf = buf;
	nextSize = len;
}

void audioEndFrame(void)
{
	if (nextBuf && nextSize) {
		if (audioGetSamplesBuffered() < queueLimit) {
			SDL_QueueAudio(dev, nextBuf, nextSize);
		}
		nextBuf = NULL;
		nextSize = 0;
	}
}

PD_CONSTRUCTOR static void audioConfigInit(void)
{
	configRegisterInt("Audio.BufferSize", &bufferSize, 0, 1 * 1024 * 1024);
	configRegisterInt("Audio.QueueLimit", &queueLimit, 0, 1 * 1024 * 1024);
	configRegisterInt("Audio.Surround51", &surround51, 0, 1); // MOD: surround 5.1
	// MOD: speaker volumes
	configRegisterInt("Audio.VolumeFrontLeft", &speakerVolume[SPK_FL], 0, 150);
	configRegisterInt("Audio.VolumeFrontRight", &speakerVolume[SPK_FR], 0, 150);
	configRegisterInt("Audio.VolumeCentre", &speakerVolume[SPK_C], 0, 150);
	configRegisterInt("Audio.VolumeSubwoofer", &speakerVolume[SPK_LFE], 0, 150);
	configRegisterInt("Audio.VolumeRearLeft", &speakerVolume[SPK_RL], 0, 150);
	configRegisterInt("Audio.VolumeRearRight", &speakerVolume[SPK_RR], 0, 150);
}
