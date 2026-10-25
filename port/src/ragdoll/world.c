/**
 * MOD: ragdoll physics - bodies and the world: thuds, surfaces, friction,
 * water, ledges, and guards reacting to bodies. Part of the ragdoll module
 * (port/src/ragdoll.c).
 */

#include "rd_internal.h"
#include "lib/snd.h"
#include "game/file.h"
#ifdef PD_GRAVITY
#include "gravity.h" // MOD: gravity
#endif

#ifdef PD_PHYSICS

/**
 * Plays a thud when the pelvis or chest stops falling fast (hits the floor).
 */
#define RD_THUD_MINSPEED 90.0f   // PD units/s (~0.9 m/s) along the contact normal
#define RD_THUD_INTERVAL60 30    // at most one thud per body every 0.5 s
#define RD_THUD_FEETDELAY60 45   // shins/feet can't thud for the first 0.75 s
#define RD_KO_KILLSPEED 1100.0f // a knocked-out body hitting something this hard (~11 m/s, a fall of ~6 m) dies
#define RD_IMPACT_SPLATSPEED 650.0f // a landing this hard (~6.5 m/s, a fall of 2+ m) leaves blood
#define RD_IMPACT_MAXSPLATS 3

/**
 * The sound of a body hitting the floor: the game's body thuds, or for a
 * Skedar its own footsteps (heavier and meatier; the same choice as
 * footstepChooseSound: metal floors have their own, Mini Skedar step like
 * humans)
 */
static s32 ragdollThudSound(struct ragdoll *rd, s32 *thudindex)
{
	struct chrdata *chr = rd->chr;
	s32 sound;

	if (CHRRACE(chr) == RACE_SKEDAR && chr->bodynum != BODY_MINISKEDAR) {
		rd->thudsample ^= 1;

		if (chr->floortype == FLOORTYPE_METAL) {
			return rd->thudsample ? SFX_FOOTSTEP_8191 : SFX_FOOTSTEP_8192;
		}

		return rd->thudsample ? SFX_FOOTSTEP_818F : SFX_FOOTSTEP_8190;
	}

	sound = SFX_THUD_808D + *thudindex;
	*thudindex = (*thudindex + 1) % 11;

	return sound;
}

/**
 * What a body lands on: the floor's type under a point (the game's tiles, as
 * for footsteps), or -1 if there's no floor just under it.
 */
static s32 rdFloorTypeAt(struct coord *pos, RoomNum *rooms, f32 maxbelow)
{
	struct coord p = *pos;
	u8 floortype = 0;
	f32 ground;

	p.y += 10.0f;
	ground = cdFindGroundAtCyl(&p, 5.0f, rooms, NULL, &floortype);

	if (ground < -100000.0f || pos->y - ground > maxbelow) {
		return -1;
	}

	return floortype <= FLOORTYPE_SNOW ? floortype : FLOORTYPE_DEFAULT;
}

#define RD_SURFACE_DEBRISSPEED 300.0f // a landing this hard (~3 m/s) in shallow water splashes

/**
 * The rooms a sound at pos should be placed in: the corpse's, or a cut
 * piece's when the piece is nearer (a piece carried far from the body is in
 * other rooms; given the body's, its sounds were heard from the body's
 * rooms, which is often not at all).
 */
static RoomNum *rdRoomsNear(struct ragdoll *rd, const f32 *pos)
{
	RoomNum *rooms = rd->prop->rooms;
	f32 best = rdDistSqCoord(pos, &rd->prop->pos);
	s32 k;

	for (k = 1; k < rd->numparts; k++) {
		if ((rd->severed & (1u << k)) && rd->piecerooms[k][0] > 0) {
			f32 d[3];
			f32 dist;

			rdVecSub(pos, rd->world[k].m[3], d);
			dist = rdVecLenSq(d);

			if (dist < best) {
				best = dist;
				rooms = rd->piecerooms[k];
			}
		}
	}

	return rooms;
}

/**
 * The surface a body hits: as well as the thud, the sound of the surface
 * (the game's running footsteps on it: snow crunches, metal clanks, water
 * splashes) and, landing hard in water, a splash.
 */
static void ragdollSurfaceImpact(struct ragdoll *rd, struct coord *pos, f32 impact)
{
	extern s32 g_FootstepSounds[];
	f32 posv[3] = { pos->x, pos->y, pos->z };
	RoomNum *rooms = rdRoomsNear(rd, posv); // (a cut piece's, if it's the nearer)
	s32 floortype = rdFloorTypeAt(pos, rooms, 40.0f);

	if (floortype < 0) {
		return;
	}

	if (floortype != FLOORTYPE_DEFAULT) {
		static const u8 running[] = {2, 3, 6, 7}; // the running steps (footstepChooseSound)
		s32 sound = g_FootstepSounds[floortype * 8 + running[rngRandom() % 4]];

		if (sound > 0) {
			psCreate(NULL, NULL, sound, -1,
					-1, 0, 0, PSTYPE_NONE, pos, -1, rooms, -1, -1, -1, -1);
		}
	}

	if (impact >= RD_SURFACE_DEBRISSPEED && rooms[0] >= 0) {
		struct coord up = {0.0f, 1.0f, 0.0f};
		struct coord at = *pos;

		// Only water splashes (dust on other floors was too much)
		if (floortype == FLOORTYPE_WATER) {
			sparksCreate(rooms[0], NULL, &at, &up, &up, SPARKTYPE_SHALLOWWATER);
		}
	}
}

/**
 * Living guards and bodies (Ragdoll.RagdollBodyAlerts):
 * - a body (or a cut-off piece) thrown into a guard staggers him like a
 *   weak hit, and he knows someone's there
 * - a corpse that's moved (dragged, thrown, knocked about) is checked again
 *   by guards that might see it: one that can (the witness field of view
 *   rule) has seen a death, as if he saw it happen
 */
#define RD_BODYHIT_MINSPEED  220.0f // the body must be moving this fast itself (~2.2 m/s)
#define RD_BODYHIT_INTERVAL60 60    // a guard is staggered by bodies at most once a second
#define RD_ACTORTHUD_INTERVAL60 20  // a body thuds against characters at most every 1/3 s
#define RD_SEEN_MOVE         60.0f  // a corpse moving this far is checked again
#define RD_SEEN_SETTLEMOVE   8.0f   // ... and once more where it comes to rest, if it moved this far
#define RD_SEEN_STILL60      15     // ... (still for this long)

void ragdollBodyHitsActor(struct ragdollprop *rp, pdphysPropImpact *pi)
{
	static s32 thudindex = 0;
	struct chrdata *chr = rp->prop->chr;
	struct ragdoll *rd = ragdollFindByHandle(pi->ragdoll);
	struct coord from;
	f32 rel;
	s32 hitpart;
	f32 shield;
	s32 race;

	if (rd == NULL || rd->chr == chr) {
		return;
	}

	// The thud of it hitting them (its own thuds are limited to one every
	// 0.5 s and usually just played for the floor)
	if (pi->speed >= RD_THUD_MINSPEED
			&& (rd->actorthud60 == 0 || g_Vars.lvframe60 - rd->actorthud60 > RD_ACTORTHUD_INTERVAL60 || g_Vars.lvframe60 < rd->actorthud60)) {
		struct coord pos;

		rdVecToCoord(pi->pos, &pos);
		psCreate(NULL, NULL, ragdollThudSound(rd, &thudindex), -1,
				-1, 0, 0, PSTYPE_NONE, &pos, -1, rdRoomsNear(rd, pi->pos), -1, -1, -1, -1);
		rd->actorthud60 = RD_FRAMESTAMP();
	}

	if (!g_RagdollBodyAlerts || rp->prop->type != PROPTYPE_CHR || chr == NULL || chr->prop != rp->prop
			|| pi->ownspeed < RD_BODYHIT_MINSPEED) {
		return;
	}

	race = CHRRACE(chr);

	if (chr->aibot || race == RACE_ROBOT || race == RACE_DRCAROLL || race == RACE_EYESPY
			|| (chr->chrflags & CHRCFLAG_HIDDEN)
			|| chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD || chr->actiontype == ACT_ARGH
			|| chr->actiontype == ACT_DRUGGEDDROP || chr->actiontype == ACT_DRUGGEDKO
			|| chr->actiontype == ACT_DRUGGEDCOMINGUP) {
		return;
	}

	if (rp->lastimpact60 != 0 && g_Vars.lvframe60 - rp->lastimpact60 < RD_BODYHIT_INTERVAL60
			&& g_Vars.lvframe60 >= rp->lastimpact60) {
		return;
	}

	rp->lastimpact60 = RD_FRAMESTAMP();

	// Where it hit him decides the flinch; it came from where the body was
	rel = (pi->pos[1] - chr->manground) / (chr->height > 30.0f ? chr->height : 160.0f);
	hitpart = rel > 0.85f ? HITPART_HEAD : rel > 0.45f ? HITPART_TORSO : rel > 0.3f ? HITPART_PELVIS
		: (rngRandom() & 1) ? HITPART_LTHIGH : HITPART_RTHIGH;

	from.x = chr->prop->pos.x - pi->dir[0] * 100.0f;
	from.y = chr->prop->pos.y;
	from.z = chr->prop->pos.z - pi->dir[2] * 100.0f;

	shield = chrGetShield(chr);

	if (shield > 0) {
		// A shield takes it, as it does shots, and the chr isn't staggered
		// (a scripted walk like Cassandra's in Extraction would stop for good)
		shieldhitCreate(chr->prop, shield, NULL, NULL, NULL, 0, 0);
	} else {
		chrBeginArgh(chr, chrGetAngleToPos(chr, &from), hitpart);
	}

	// Someone threw it: an enemy of the player's is on alert (as if he
	// heard them)
	if (g_Vars.currentplayer && g_Vars.currentplayer->prop && g_Vars.currentplayer->prop->chr
			&& chrCompareTeams(chr, g_Vars.currentplayer->prop->chr, COMPARE_ENEMIES)) {
		chrRecordLastHearTargetTime(chr);
	}

	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d's body hit chr %d at %.0f u/s: %s (hit part %d)",
			RD_RDCHRNUM(rd), chr->chrnum, pi->ownspeed, shield > 0 ? "shield took it" : "staggered", hitpart);
}

/**
 * Who can see the body. The game looks at the corpse's position: a standing
 * chr's (about waist high, where it stays for an animated death), but a
 * ragdoll's is its pelvis, a hand's width off the floor, which the smallest
 * step, crate or railing hides. The check looks a little above the body
 * instead.
 */
#define RD_WITNESS_RAISE 50.0f

static void rdAlertWitnesses(struct ragdoll *rd)
{
	struct chrdata *chr = rd->chr;
	struct coord saved = chr->prop->pos;
	f32 top = rd->world[0].m[3][1];
	s32 part;

	for (part = 1; part <= 2 && part < rd->numparts; part++) {
		if (rd->bodyindex[part] >= 0 && rd->world[part].m[3][1] > top) {
			top = rd->world[part].m[3][1];
		}
	}

	chr->prop->pos.y = top + RD_WITNESS_RAISE;
	chrAlertOthersOfInjury(chr, true);
	chr->prop->pos = saved;
}

/**
 * chrTickDie(), for a ragdoll that's still dying: who sees it
 */
void ragdollAlertOthersOfDeath(struct chrdata *chr)
{
	struct ragdoll *rd = ragdollFindByChr(chr);

	if (rd && rd->state == RDSTATE_SIMULATING && !rd->isplayer && rd->chr == chr && chr->prop == rd->prop) {
		rdAlertWitnesses(rd);
	} else {
		chrAlertOthersOfInjury(chr, true);
	}
}

void ragdollCheckSeen(struct ragdoll *rd)
{
	RD_CRASHCTX("checkseen", RD_RDCHRNUM(rd), -1);
	struct chrdata *chr = rd->chr;
	f32 *pelvis = rd->world[0].m[3];
	f32 d[3];
	bool restart = false;

	if (!g_RagdollBodyAlerts || rd->isplayer || chr == NULL || chr->prop != rd->prop
			|| (!RD_ISDYING(rd) && !RD_ISLYING(rd))) {
		rd->seenset = false;
		rd->rescanning = false;
		return;
	}

	if (!rd->seenset) {
		memcpy(rd->seenpos, pelvis, sizeof(rd->seenpos));
		memcpy(rd->seenprev, pelvis, sizeof(rd->seenprev));
		rd->seenstill60 = 0;
		rd->seenset = true;
		return;
	}

	// Still or moving
	rdVecSub(pelvis, rd->seenprev, d);

	if (rd->sleeping || rdVecLen(d) < 0.5f) {
		rd->seenstill60 += g_Vars.lvupdate60;
	} else {
		rd->seenstill60 = 0;
	}

	memcpy(rd->seenprev, pelvis, sizeof(rd->seenprev));

	// Moved: the guards check again, every so often while it's moving (it
	// may pass through their view) and once more where it comes to rest
	rdVecSub(pelvis, rd->seenpos, d);

	if (rdVecLen(d) > RD_SEEN_MOVE) {
		restart = true;
	} else if (rdVecLen(d) > RD_SEEN_SETTLEMOVE && rd->seenstill60 >= RD_SEEN_STILL60) {
		restart = true;
	}

	if (restart) {
		memcpy(rd->seenpos, pelvis, sizeof(rd->seenpos));

		// Still dying (or going down, knocked out): chrTickDie /
		// chrTickDruggedDrop check every tick, from where the check has got
		// to (ragdollAlertOthersOfDeath). Dead or out cold: checked here.
		if (RD_ISDYING(rd)) {
			chr->act_die.notifychrindex = 0;
		} else {
			chr->act_dead.notifychrindex = 0;
			rd->rescanning = true;
		}
	}

	if (rd->rescanning && RD_ISLYING(rd)) {
		rdAlertWitnesses(rd);

		if (chr->act_dead.notifychrindex < 0 || chr->act_dead.notifychrindex >= chrsGetNumSlots()) {
			rd->rescanning = false;
		}
	}
}

/**
 * Ledge catch (Ragdoll.RagdollLedgeCatch): a body going over a ledge with an
 * arm or leg still on top holds there for a moment (a pull on that part that
 * carries the body's weight) before it drops.
 */
#define RD_CATCH_PARTS      { 5, 6, 7, 8, 11, 12, 13, 14 } // forearms, hands, shins, feet (those with a physics part)
#define RD_CATCH_REACH      22.0f  // the part is on a floor this close below it
#define RD_CATCH_HANG       -5.0f  // the pelvis is down at that floor or lower (going over the edge)
#define RD_CATCH_DROP       45.0f  // with nothing under the pelvis this far down
#define RD_CATCH_FALLSPEED  0.5f   // units per tick the pelvis must be falling at
#define RD_CATCH_FORCE      1400.0f // N (a body weighs about 700; it swings down from the hold)
#define RD_CATCH_FREQ       5.0f
#define RD_CATCH_SLIP       35.0f  // let go early if the part is dragged this far anyway
#define RD_CATCH_MINTIME60  15     // holds for 0.25 to 0.65 s
#define RD_CATCH_RNDTIME60  25
#define RD_CATCH_MAX        2      // per body
#define RD_CATCH_INTERVAL60 90

void ragdollLedgeCatch(struct ragdoll *rd)
{
	RD_CRASHCTX("ledgecatch", RD_RDCHRNUM(rd), -1);
	static const s32 parts[] = RD_CATCH_PARTS;
	f32 *pelvis = rd->world[0].m[3];
	f32 fall = rd->catchprevy - pelvis[1];
	s32 i;

	rd->catchprevy = pelvis[1];

	if (!g_RagdollLedgeCatch || rd->grabbed || rd->riding || rd->inwater || rd->severed || !rd->skel->human) {
		rd->catchuntil60 = 0;
		return;
	}

	// Holding on
	if (rd->catchuntil60 > 0) {
		f32 pos[3];
		f32 d[3];

		pdphysRagdollGetPartPos(rd->handle, rd->catchpart, pos);
		rdVecSub(pos, rd->catchpos, d);

		if (rd->age60 >= rd->catchuntil60 || rdVecLen(d) > RD_CATCH_SLIP) {
			rd->catchuntil60 = 0;
		} else {
			pdphysRagdollPull(rd->handle, rd->catchpart, rd->catchpos, RD_CATCH_FORCE, RD_CATCH_FREQ);
		}

		return;
	}

	if (rd->numcatches >= RD_CATCH_MAX || (rd->numcatches > 0 && rd->age60 - rd->lastcatch60 < RD_CATCH_INTERVAL60)
			|| g_Vars.lvupdate60 <= 0 || fall < RD_CATCH_FALLSPEED * g_Vars.lvupdate60) {
		return;
	}

	// Nothing under the pelvis: it's going over an edge
	{
		f32 to[3] = { pelvis[0], pelvis[1] - RD_CATCH_DROP, pelvis[2] };
		f32 hit[3];
		f32 normal[3];
		s32 room;

		if (pdphysRagdollRaycast(rd->handle, pelvis, to, hit, normal, &room)) {
			return;
		}
	}

	// A limb still on top of the ledge, above the pelvis
	for (i = 0; i < ARRAYCOUNT(parts); i++) {
		s32 b = parts[i] < rd->numparts ? rd->bodyindex[parts[i]] : -1;
		f32 pos[3];
		f32 to[3];
		f32 hit[3];
		f32 normal[3];
		s32 room;

		if (b < 0) {
			continue;
		}

		pdphysRagdollGetPartPos(rd->handle, b, pos);
		to[0] = pos[0];
		to[1] = pos[1] - RD_CATCH_REACH;
		to[2] = pos[2];

		if (pdphysRagdollRaycast(rd->handle, pos, to, hit, normal, &room) && normal[1] >= 0.7f
				&& pelvis[1] < hit[1] - RD_CATCH_HANG) {
			rd->catchpart = b;
			rdVecCopy(pos, rd->catchpos);
			rd->catchuntil60 = rd->age60 + RD_CATCH_MINTIME60 + (s32) (rngRandom() % RD_CATCH_RNDTIME60);
			rd->numcatches++;
			rd->lastcatch60 = rd->age60;
			sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d caught on a ledge by part %d for %d ticks",
					RD_RDCHRNUM(rd), parts[i], rd->catchuntil60 - rd->age60);
			pdphysRagdollPull(rd->handle, b, rd->catchpos, RD_CATCH_FORCE, RD_CATCH_FREQ);
			break;
		}
	}
}

/**
 * A body in the air screams, as guards falling over a railing do: a fresh
 * death (one that hasn't come to rest yet, or did less than 2 s ago) or a
 * knocked-out guard that has been off the ground for Ragdoll.RagdollScreamAirTime
 * seconds (flung by a blast, a throw or a punch, or falling), counted from
 * when it's free of its death animation. The cry comes from the body and
 * follows it. Off the ground: nothing under its lowest part within
 * RD_SCREAM_GROUNDGAP and no impact; time only counts while it's moving
 * (a body hanging still over an edge isn't flying).
 */
#define RD_SCREAM_GROUNDGAP 40.0f    // ground within this far below its lowest part: not in the air
#define RD_SCREAM_MOVING 50.0f       // air time counts while the pelvis moves at least this fast (units/s)
#define RD_SCREAM_KOAGAIN60 240      // a knocked-out guard can scream again after 4 s
#define RD_SCREAM_TELEPORT 4000.0f   // faster than this it was moved, not flying
#define RD_SCREAM_RESTGRACE60 120    // still screams if sent flying within 2 s of first coming to rest (the flight can last longer)
#define RD_SCREAM_WAVCHANCE 100      // men: 1 scream in this many is data/sounds/scream.wav (wavsound.c)

/**
 * Cassandra's scream is an MP3, and the game plays one MP3 at a time: a
 * mission voice line starting during it takes over the stream. From then on
 * the scream's channel would set that line's volume and pan from the body,
 * so it is told to stop adjusting (PSFLAG2_0010, as dialogue) and frees
 * itself when the line ends.
 */
extern struct mp3vars g_Mp3Vars;

static s32 g_RdScreamMp3Channel = -1;
static u32 g_RdScreamMp3Uuid;
static uintptr_t g_RdScreamMp3Rom;

void ragdollScreamTick(void)
{
	struct pschannel *channel;

	if (g_RdScreamMp3Channel < 0) {
		return;
	}

	channel = &g_PsChannels[g_RdScreamMp3Channel];

	if ((channel->flags & PSFLAG_FREE) || channel->uuid != g_RdScreamMp3Uuid) {
		g_RdScreamMp3Channel = -1; // finished
	} else if (!sndIsPlayingMp3() || g_Mp3Vars.romaddr != g_RdScreamMp3Rom) {
		channel->flags2 |= PSFLAG2_0010;
		g_RdScreamMp3Channel = -1;
	}
}

static void ragdollScream(struct ragdoll *rd)
{
	// The game's own cries for a fall over a railing (chrTickDie's specialdie
	// sounds: sounds 0x34c, 0x313 and 0x411); now and then (1 in
	// RD_SCREAM_WAVCHANCE) data/sounds/scream.wav instead, when it's there
	static const u16 male[] = { SFX_M1_SCREAM, SFX_M1_SCREAM, SFX_M1_NOOO, SFX_M2_NOOO };
	struct chrdata *chr = rd->chr;
	struct prop *prop = rd->prop;
	s32 sound = -1;

	rd->scream60 = rd->age60 > 0 ? rd->age60 : 1;

	// Women: Cassandra's screams as she dies in Attack Ship, an MP3. The
	// game plays one MP3 at a time, so not over a voice line (no scream)
	if (CHRRACE(chr) == RACE_HUMAN && !g_HeadsAndBodies[chr->headnum].ismale && sndIsPlayingMp3()) {
		sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d would scream (%.1f s in the air) but a voice line is playing", RD_RDCHRNUM(rd), rd->screamair);
		return;
	}

	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d screams (%.1f s in the air)", RD_RDCHRNUM(rd), rd->screamair);

	// Over whatever cry it was making
	psStopSound(prop, PSTYPE_CHRTALK, 0);
	psStopSound(prop, PSTYPE_CHRCHOKE, 0);

	if (CHRRACE(chr) == RACE_HUMAN) {
		if (g_HeadsAndBodies[chr->headnum].ismale) {
			if (rngRandom() % RD_SCREAM_WAVCHANCE == 0 && rdWavAvailable(RDWAV_SCREAM)) {
				// At the body and following it, as loud as the game's scream there
				rdWavPlay(RDWAV_SCREAM, prop, SFX_M1_SCREAM);
				return;
			}

			sound = male[rngRandom() % ARRAYCOUNT(male)];
		} else {
			// Its 0x2000 | 0x4000 bits make the game play it as dialogue, at
			// one volume wherever it is (psCreate: PSFLAG2_0010); without
			// them it's placed at the body like any other sound
			sound = MP3_0486 & ~(0x2000 | 0x4000);
		}
	}

	if (sound >= 0) {
		// On the body's prop: it moves with the body, as the death cries do
		s32 channel = psCreate(NULL, prop, sound, -1, -1, 0, 0, PSTYPE_CHRCHOKE, NULL, -1, NULL, -1, -1, -1, -1);

		if (channel >= 0 && sndIsMp3(sound)) {
			union soundnumhack ref;

			ref.packed = sound;
			g_RdScreamMp3Channel = channel;
			g_RdScreamMp3Uuid = g_PsChannels[channel].uuid;
			g_RdScreamMp3Rom = fileGetRomAddress(ref.id);
			ragdollScreamTick(); // refused at the start (another MP3 took the stream)
		}
	} else {
		// Maians, Skedar: their own death cry
		chrChoke(chr, CHOKETYPE_NONE);
	}
}

void ragdollCheckScream(struct ragdoll *rd, f32 dt)
{
	f32 *pelvis = rd->world[0].m[3];
	f32 d[3], speed, from[3], to[3], hit[3], normal[3];
	s32 i, room, lowest = 0;
	bool grounded;

	if (dt <= 0.0f) {
		return;
	}

	for (i = 0; i < 3; i++) {
		d[i] = rd->screamprevset ? pelvis[i] - rd->screamprev[i] : 0.0f;
		rd->screamprev[i] = pelvis[i];
	}

	rd->screamprevset = true;
	speed = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / dt;

#ifdef PD_GRAVITY
	// MOD: gravity - below the original gravity bodies float slowly through
	// the air: no screaming
	if (gravityScale() < 1.0f) {
		rd->screamair = 0.0f;
		return;
	}

#endif
	// Who screams: not the player, nor a body that came to rest more than
	// 2 s before it was sent flying (unless knocked out), nor one without a
	// head; not while it's following its death animation (the air time only
	// starts once it's free of it), carried, on a lift or holding on to a
	// ledge
	if (g_RagdollScreamAirTime <= 0.0f || rd->isplayer || rd->chr == NULL || rd->prop == NULL
			|| rd->blending
			|| !(rd->knockedout || rd->firstsleep60 == 0 || rd->screamair > 0.0f
				|| rd->age60 - rd->firstsleep60 < RD_SCREAM_RESTGRACE60)
			|| (rd->severed & ((1u << 1) | (1u << 2)))
			|| (rd->scream60 != 0 && !(rd->knockedout && rd->age60 - rd->scream60 >= RD_SCREAM_KOAGAIN60))
			|| rd->grabbed || speed > RD_SCREAM_TELEPORT
			|| (rd->liftnear60 > 0 && g_Vars.lvframe60 - rd->liftnear60 < 30)
			|| rd->catchuntil60 > rd->age60) {
		rd->screamair = 0.0f;
		return;
	}

	// Ground under its lowest part, or a hit this tick (ragdollCheckThud)
	grounded = rd->thudpart >= 0 && rd->contacts[rd->thudpart] >= RD_THUD_MINSPEED;

	if (!grounded) {
		for (i = 1; i < rd->numparts; i++) {
			if (rd->bodyindex[i] >= 0 && (rd->severed & (1u << i)) == 0
					&& rd->world[i].m[3][1] < rd->world[lowest].m[3][1]) {
				lowest = i;
			}
		}

		rdVecCopy(rd->world[lowest].m[3], from);
		rdVecCopy(from, to);
		to[1] -= RD_SCREAM_GROUNDGAP;
		grounded = pdphysRagdollRaycast(rd->handle, from, to, hit, normal, &room) != 0;
	}

	if (grounded) {
		rd->screamair = 0.0f;
		return;
	}

	if (speed >= RD_SCREAM_MOVING) {
		rd->screamair += dt;
	}

	if (rd->screamair >= g_RagdollScreamAirTime) {
		ragdollScream(rd);
		rd->screamair = 0.0f;
	}
}

void ragdollCheckThud(struct ragdoll *rd)
{
	RD_CRASHCTX("checkthud", RD_RDCHRNUM(rd), -1);
	static s32 thudindex = 0;
	f32 speeds[PDPHYS_MAX_RAGDOLL_PARTS];
	f32 normalys[PDPHYS_MAX_RAGDOLL_PARTS];
	f32 impact = 0.0f;
	s32 i;

	pdphysRagdollTakeNewContactsEx(rd->handle, speeds, normalys); // always read (clears them)
	memcpy(rd->contacts, speeds, sizeof(rd->contacts));
	rd->thudpart = -1;

	// Riding a moving lift: the floor keeps knocking into the body, but
	// that's not a fall. Nor is being dragged around by the player.
	if ((rd->liftnear60 > 0 && g_Vars.lvframe60 - rd->liftnear60 < 30) && !rd->grabbed) {
		for (i = 0; i < 2; i++) {
			rd->prevvy[i] = 0.0f;
		}

		// A body caught between a moving lift and the level (hanging over
		// its edge, or against a character on it) is squeezed out at
		// absurd speeds: nothing on a lift moves that fast relative to it
		if (g_Vars.lvframe60 - rd->liftnear60 < 6
				&& (rd->lastexplosion60 < 0 || g_Vars.lvframe60 - rd->lastexplosion60 > 60)) {
			pdphysRagdollClampSpeed(rd->handle, rd->liftvel, RD_LIFT_MAXRELSPEED);
		}

		return;
	}

	// Held by the player: dragging it over the floor isn't a fall, but
	// swinging it into a wall, a door or another body is a hit like any other
	if (rd->grabbed) {
		for (i = 0; i < 2; i++) {
			rd->prevvy[i] = 0.0f;
		}
	}

	for (i = 0; i < PDPHYS_MAX_RAGDOLL_PARTS; i++) {
		// The shins/feet are usually on the floor already when the body
		// starts to fall: they don't count at first
		if (rd->age60 < RD_THUD_FEETDELAY60 && (i == rd->bodyindex[11] || i == rd->bodyindex[12])) {
			continue;
		}

		if (rd->grabbed && normalys[i] >= 0.7f) {
			continue;
		}

		if (speeds[i] > impact) {
			impact = speeds[i];
			rd->thudpart = i;
		}
	}

	// A knocked-out guard hitting something very hard (a long fall, thrown
	// into a wall) dies
	if (impact >= RD_KO_KILLSPEED && rd->knockedout && rd->age60 > 2 && !rd->grabbed) {
		ragdollKillKnockedOut(rd, impact);
	}

	// A thud when a body part starts touching a surface (or another body)
	// hard enough; at most one every 0.5 s per body
	if (impact >= RD_THUD_MINSPEED && rd->age60 > 2 && g_Vars.lvframe60 - rd->lastthud60 > RD_THUD_INTERVAL60) {
		struct coord pos;
		s32 part;

		// From the part that hit (a cut piece can be far from the body)
		pos = rd->prop->pos;

		for (part = 0; part < rd->numparts; part++) {
			if (rd->bodyindex[part] == rd->thudpart && rd->thudpart >= 0) {
				rdVecToCoord(rd->world[part].m[3], &pos);
				break;
			}
		}

		{
			f32 at[3];

			rdCoordToVec(&pos, at);
			psCreate(NULL, NULL, ragdollThudSound(rd, &thudindex), -1,
					-1, 0, 0, PSTYPE_NONE, &pos, -1, rdRoomsNear(rd, at), -1, -1, -1, -1);
		}

		// Landing on a floor: what it's made of
		if (rd->thudpart >= 0 && normalys[rd->thudpart] >= 0.7f) {
			ragdollSurfaceImpact(rd, &pos, impact);
		}

		rd->lastthud60 = g_Vars.lvframe60;
		rd->numthuds++;

		// A hard landing (a long fall, a body thrown into a wall) leaves a
		// blood mark where it hit, using the game's own splats
		if (impact >= RD_IMPACT_SPLATSPEED && rd->impactsplats < RD_IMPACT_MAXSPLATS && !rd->chr->noblood
				&& CHRRACE(rd->chr) != RACE_ROBOT && CHRRACE(rd->chr) != RACE_DRCAROLL && rd->prop->rooms[0] >= 0) {
			struct shotdata sd;

			memset(&sd, 0, sizeof(sd));
			sd.gunpos3d = pos;
			sd.gunpos2d = pos;
			sd.gundir3d.y = sd.gundir2d.y = -1.0f;
			pos.y += 10.0f;
			sd.gunpos3d.y = sd.gunpos2d.y = pos.y;

			splatsCreate(1, 1.0f + impact / 1500.0f, rd->prop, &sd, &pos, &pos,
					CHRRACE(rd->chr) == RACE_SKEDAR, 0, TICKS(50), rd->chr, 0);
			rd->impactsplats++;
			sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d hit something at %.0f u/s: blood splat", RD_RDCHRNUM(rd), impact);
		}
	}
}

/**
 * Per tick: step the simulation and pull the new poses back into the game.
 */
// Far from the camera: some per-tick work can be done less often
/**
 * Bodies float in water (MOD: ragdoll physics). The water surface comes from
 * the drawn water the collision leaves out (clipcheck.c).
 */
/**
 * How much a body grips the floor it's on: snow, carpet and mud hold it,
 * metal and polished floors let it slide further.
 */
void ragdollUpdateFriction(struct ragdoll *rd)
{
	static const f32 frictions[] = {
		0.8f,  // FLOORTYPE_DEFAULT (as before)
		0.6f,  // FLOORTYPE_WOOD
		0.65f, // FLOORTYPE_STONE
		1.1f,  // FLOORTYPE_CARPET
		0.4f,  // FLOORTYPE_METAL
		1.1f,  // FLOORTYPE_MUD
		0.5f,  // FLOORTYPE_WATER
		0.95f, // FLOORTYPE_DIRT
		1.0f,  // FLOORTYPE_SNOW
	};
	struct coord pelvis;
	s32 floortype;

	rdVecToCoord(rd->world[0].m[3], &pelvis);
	floortype = rdFloorTypeAt(&pelvis, rd->prop->rooms, 60.0f);

	if (floortype >= 0 && frictions[floortype] != rd->friction) {
		rd->friction = frictions[floortype];
		pdphysRagdollSetFriction(rd->handle, rd->friction);
	}
}

void ragdollCheckWater(struct ragdoll *rd)
{
#ifdef PD_CLIPCHECK
	f32 *pelvis = rd->world[0].m[3];
	f32 y;
	bool in = rd->prop != NULL && clipcheckWaterLevel(rd->prop->rooms, pelvis[0], pelvis[2], pelvis[1] - 30.0f, pelvis[1] + 600.0f, &y);

	if (in != rd->inwater || (in && fabsf(y - rd->watery) > 1.0f)) {
		if (in && !rd->inwater) {
			sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d is in water (surface %.0f, pelvis %.0f)", RD_RDCHRNUM(rd), y, pelvis[1]);
		}

		rd->inwater = in;
		rd->watery = in ? y : 0.0f;
		pdphysRagdollSetWater(rd->handle, in, rd->watery);
	}
#endif
}

#endif // PD_PHYSICS
