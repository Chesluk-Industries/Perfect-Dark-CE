// MOD: ragdoll physics - sounds from WAV files (data/sounds/), played at a
// body and following it as the game's own sounds do. The game can only play
// sounds from the ROM, so these are mixed into the port's output
// (audio.c: audioSetNextBuffer -> rdWavMix) with the volume and pan the game
// works out for one of its sounds at the same place (psGetTheoreticalVolPan).
#include <math.h>
#include <string.h>
#include <SDL.h>
#include "rd_internal.h"
#include "lib/snd.h"
#include "game/lv.h"
#include "fs.h"

#ifdef PD_PHYSICS

#define RD_WAV_RATE 22020          // the game's output rate (audio.c)
#define RD_WAV_MAXVOICES 4
#define RD_WAV_RAMP (1.0f / 441.0f) // gain changes spread over ~20 ms, so moving doesn't click

// Loudness, measured against the game's scream (SFX_M1_SCREAM) played the same
// way from the same place (the port's output dumped): the game's sounds come
// out at about twice vol x pan x sample, and that scream's loudest 0.1 s is
// at an RMS of about 0.36 of full scale. Each WAV is brought to that level
// when it's loaded (peaks above RD_WAV_KNEE eased under full scale), so it's
// as loud as the game's cries whatever level it was saved at.
#define RD_WAV_OUTGAIN 2.0f
#define RD_WAV_LOUDNESS 0.36f
#define RD_WAV_KNEE 0.8f

struct rdwav {
	const char *file;
	f32 *samples;                   // mono, RD_WAV_RATE
	u32 numsamples;
	s32 loaded;                     // 0 = not yet, 1 = ok, -1 = failed
};

struct rdwavvoice {
	struct rdwav *wav;
	u32 pos;                        // next sample
	struct prop *prop;
	struct chrdata *chr;
	struct coord at;                // where it is (the body, or where it was when the body went)
	RoomNum rooms[8];
	s16 likesound;                  // the game sound whose distances and volume it uses
	f32 gain[4], target[4];         // front left, front right, rear left, rear right
};

static struct rdwav g_RdWavs[] = {
	{ "sounds/scream.wav" },        // RDWAV_SCREAM
};

static struct rdwavvoice g_RdWavVoices[RD_WAV_MAXVOICES];

static void rdWavLoad(struct rdwav *wav)
{
	u32 size = 0;
	void *data;
	SDL_RWops *rw;
	SDL_AudioSpec spec;
	Uint8 *pcm = NULL;
	Uint32 pcmlen = 0;
	SDL_AudioStream *stream;
	s32 got;

	wav->loaded = -1;
	data = fsFileSize(wav->file) > 0 ? fsFileLoad(wav->file, &size) : NULL;

	if (data == NULL) {
		sysLogPrintf(LOG_WARNING, "RAGDOLL: %s not found in the data folder, that sound is left out", wav->file);
		return;
	}

	rw = SDL_RWFromConstMem(data, size);

	if (rw == NULL || SDL_LoadWAV_RW(rw, 1, &spec, &pcm, &pcmlen) == NULL) {
		sysLogPrintf(LOG_WARNING, "RAGDOLL: can't read %s: %s", wav->file, SDL_GetError());
		sysMemFree(data);
		return;
	}

	sysMemFree(data);

	// Down to mono at the game's rate
	stream = SDL_NewAudioStream(spec.format, spec.channels, spec.freq, AUDIO_F32SYS, 1, RD_WAV_RATE);

	if (stream && SDL_AudioStreamPut(stream, pcm, pcmlen) == 0 && SDL_AudioStreamFlush(stream) == 0) {
		got = SDL_AudioStreamAvailable(stream);
		wav->samples = malloc(got > 0 ? got : 4);

		if (wav->samples) {
			got = SDL_AudioStreamGet(stream, wav->samples, got);

			if (got > 0) {
				wav->numsamples = got / sizeof(f32);
				wav->loaded = 1;
			}
		}
	}

	if (stream) {
		SDL_FreeAudioStream(stream);
	}

	SDL_FreeWAV(pcm);

	if (wav->loaded == 1) {
		// To the game's loudness (see the top): its loudest 0.1 s
		u32 win = RD_WAV_RATE / 10, i;
		f64 sum = 0.0, loudest = 0.0;
		f32 gain;

		for (i = 0; i < wav->numsamples; i++) {
			sum += wav->samples[i] * wav->samples[i];

			if (i >= win) {
				sum -= wav->samples[i - win] * wav->samples[i - win];
			}

			if (sum > loudest) {
				loudest = sum;
			}
		}

		loudest = sqrt(loudest / MIN(win, wav->numsamples));
		gain = loudest > 0.0001 ? RD_WAV_LOUDNESS / loudest : 1.0f;

		for (i = 0; i < wav->numsamples; i++) {
			f32 v = wav->samples[i] * gain;
			f32 a = fabsf(v);

			if (a > RD_WAV_KNEE) {
				a = RD_WAV_KNEE + (1.0f - RD_WAV_KNEE) * tanhf((a - RD_WAV_KNEE) / (1.0f - RD_WAV_KNEE));
				v = v < 0.0f ? -a : a;
			}

			wav->samples[i] = v;
		}

		sysLogPrintf(LOG_NOTE, "RAGDOLL: loaded %s (%.2f s, level x%.2f)", wav->file, wav->numsamples / (f32) RD_WAV_RATE, gain);
	} else {
		sysLogPrintf(LOG_WARNING, "RAGDOLL: can't convert %s: %s", wav->file, SDL_GetError());
		free(wav->samples);
		wav->samples = NULL;
	}
}

bool rdWavAvailable(s32 which)
{
	struct rdwav *wav = &g_RdWavs[which];

	if (wav->loaded == 0) {
		rdWavLoad(wav);
	}

	return wav->loaded == 1;
}

/**
 * The gains for where the voice is: the game's volume and pan for likesound
 * there (distance, rooms and portals), its volume setting for that sound,
 * the Sound Effects volume, and the pan and front/rear as naudio applies them.
 */
static void rdWavVoiceAim(struct rdwavvoice *v)
{
	s32 vol = 0, pan = AL_PAN_CENTER;
	union soundnumhack ref;
	f32 amp, p, l, r;
	bool rear;
	s32 i;

	if (v->prop && v->prop->type == PROPTYPE_CHR && v->prop->chr == v->chr) {
		v->at = v->prop->pos;
		memcpy(v->rooms, v->prop->rooms, sizeof(v->rooms));
	} else {
		v->prop = NULL; // the body is gone: it carries on where it was
	}

	psGetTheoreticalVolPan(&v->at, v->rooms, v->likesound, &vol, &pan);

	ref.packed = v->likesound;

	if (ref.hasconfig) {
		struct audioconfig *config = &g_AudioConfigs[g_AudioRussMappings[ref.confignum].audioconfig_index];

		vol = vol * config->volpercentage / 100;

		if (config->pan != -1) {
			pan = config->pan;
		}
	}

	amp = RD_WAV_OUTGAIN * vol / AL_VOL_FULL * snd0000e9dc() / AL_VOL_FULL;
	rear = g_SoundMode == SOUNDMODE_SURROUND && (pan & 0x80);
	p = (pan & 0x7f) / 127.0f;
	l = amp * cosf(p * (f32) (M_PI / 2));
	r = amp * sinf(p * (f32) (M_PI / 2));

	for (i = 0; i < 4; i++) {
		v->target[i] = 0.0f;
	}

	v->target[rear ? 2 : 0] = l;
	v->target[rear ? 3 : 1] = r;
}

void rdWavPlay(s32 which, struct prop *prop, s16 likesound)
{
	struct rdwavvoice *v = NULL;
	s32 i;

	if (!rdWavAvailable(which)) {
		return;
	}

	// A free voice, or the one furthest through its sound
	for (i = 0; i < RD_WAV_MAXVOICES; i++) {
		struct rdwavvoice *c = &g_RdWavVoices[i];

		if (c->wav == NULL) {
			v = c;
			break;
		}

		if (v == NULL || c->pos > v->pos) {
			v = c;
		}
	}

	memset(v, 0, sizeof(*v));
	v->wav = &g_RdWavs[which];
	v->prop = prop;
	v->chr = prop->chr;
	v->likesound = likesound;
	v->at = prop->pos;
	memcpy(v->rooms, prop->rooms, sizeof(v->rooms));
	rdWavVoiceAim(v);

	for (i = 0; i < 4; i++) {
		v->gain[i] = v->target[i];
	}
}

/**
 * Each tick: follow the bodies
 */
void rdWavTick(void)
{
	s32 i;

	for (i = 0; i < RD_WAV_MAXVOICES; i++) {
		if (g_RdWavVoices[i].wav) {
			rdWavVoiceAim(&g_RdWavVoices[i]);
		}
	}
}

void rdWavStopAll(void)
{
	memset(g_RdWavVoices, 0, sizeof(g_RdWavVoices));
}

s32 rdWavActive(void)
{
	s32 i;

	for (i = 0; i < RD_WAV_MAXVOICES; i++) {
		if (g_RdWavVoices[i].wav) {
			return true;
		}
	}

	return false;
}

static s16 rdWavClamp(f32 v)
{
	return v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (s16) v;
}

/**
 * Adds the voices to the output: front (stereo) and, with 5.1, rear
 * (stereo). With stereo output the rear part goes in the front, out of phase
 * on the right for Dolby Surround as the mixer does; with plain stereo there
 * is no rear part.
 */
void rdWavMix(s16 *front, s16 *rear, u32 frames, s32 dolby)
{
	s32 i, k;
	u32 f;

	if (lvIsPaused()) {
		return; // as the game's sounds, held while paused
	}

	for (i = 0; i < RD_WAV_MAXVOICES; i++) {
		struct rdwavvoice *v = &g_RdWavVoices[i];

		if (v->wav == NULL) {
			continue;
		}

		for (f = 0; f < frames && v->pos < v->wav->numsamples; f++, v->pos++) {
			f32 s = v->wav->samples[v->pos] * 32767.0f;
			f32 rl, rr;

			for (k = 0; k < 4; k++) {
				if (v->gain[k] < v->target[k]) {
					v->gain[k] = MIN(v->gain[k] + RD_WAV_RAMP, v->target[k]);
				} else if (v->gain[k] > v->target[k]) {
					v->gain[k] = MAX(v->gain[k] - RD_WAV_RAMP, v->target[k]);
				}
			}

			rl = s * v->gain[2];
			rr = s * v->gain[3];

			front[f * 2 + 0] = rdWavClamp(front[f * 2 + 0] + s * v->gain[0]);
			front[f * 2 + 1] = rdWavClamp(front[f * 2 + 1] + s * v->gain[1]);

			if (rear) {
				rear[f * 2 + 0] = rdWavClamp(rear[f * 2 + 0] + rl);
				rear[f * 2 + 1] = rdWavClamp(rear[f * 2 + 1] + rr);
			} else if (dolby) {
				front[f * 2 + 0] = rdWavClamp(front[f * 2 + 0] + rl);
				front[f * 2 + 1] = rdWavClamp(front[f * 2 + 1] - rr);
			} else {
				front[f * 2 + 0] = rdWavClamp(front[f * 2 + 0] + rl);
				front[f * 2 + 1] = rdWavClamp(front[f * 2 + 1] + rr);
			}
		}

		if (v->pos >= v->wav->numsamples) {
			v->wav = NULL; // finished
		}
	}
}

#endif // PD_PHYSICS
