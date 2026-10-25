/**
 * MOD: ragdoll physics - game-side glue. See port/include/ragdoll.h.
 *
 * This file: settings, the skeletons, making a ragdoll at death (pose,
 * shapes, physics body), the per-tick update and the game-facing calls.
 * The rest is in port/src/ragdoll/ (rd_internal.h has what they share).
 */

#include "ragdoll/rd_internal.h"
#include "game/body.h"     // MOD: combat simulator ragdolls - the fresh model at a respawn
#include "game/modelmgr.h"
#ifdef PD_GRAVITY
#include "gravity.h" // MOD: gravity
#endif

s32 g_RagdollsEnabled = 1;
f32 g_RagdollPushScale = 0.8f;
// MOD: ragdoll ranges - each body rolls its own value between ...Min and the
// setting itself (the max) when it dies; equal = a fixed setting
f32 g_RagdollMuscleStrengthMin = 1.0f;
f32 g_RagdollMuscleTimeMin = 1.0f;
f32 g_RagdollReflexStrengthMin = 1.2f;
s32 g_RagdollTestSpawnKey = 0;
s32 g_RagdollMaxBodies = 0;    // corpses kept as ragdolls; oldest fades out. 0 = unlimited, -1 = vanilla (they fade off-screen as in the original)
s32 g_RagdollMaxPhysical = 0;  // bodies kept in the physics world; older ones freeze in place. 0 = unlimited (at least RD_MINACTIVE)
// MOD: combat simulator ragdolls - its own limits (-1 = vanilla: the body goes with its owner at respawn)
s32 g_RagdollMpMaxBodies = 32;
s32 g_RagdollMpMaxPhysical = 32;
s32 g_RagdollChrPush = 0;       // living chrs and players push bodies around
s32 g_RdPropMeshLogged = 0;
s32 g_RagdollGrab = 1;          // hold Use (E / B) on a body to drag the part you're aiming at
f32 g_RagdollGrabStrength = 1.2f; // how much the player can lift (1 = about a body's weight)
s32 g_RagdollMuscles = 1;       // powered ragdoll, joints follow the death animation at first
f32 g_RagdollMuscleStrength = 3.0f; // motor torque multiplier (0-3)
f32 g_RagdollMuscleTime = 3.8f;      // seconds for the muscles to go fully limp (0.1-10)
s32 g_RagdollAnimBlend = 80;          // % of deaths that play the vanilla death animation first, physics taking over (0 off)
s32 g_RagdollDeathCam = 1;            // the dead player's view rides in their ragdoll's head
s32 g_RagdollDeathCamHold = 0;        // solo: the death camera stays until Start or Fire is pressed
s32 g_RagdollDeathCamControl = 1;     // the dead player can nudge their body (move) and turn its head (look)
f32 g_RagdollDeathCamBodyView = 0.8f; // body awareness: how strongly the death camera keeps the body in view (0 = off, 1 = centred)
f32 g_RagdollDeathCamSmooth = 0.5f;   // camera smoothing (0 = raw head motion, 0.9 = very smooth)
s32 g_RagdollReflexBraceChance = 53;  // % of deaths where the arms reach out to break the fall
s32 g_RagdollReflexHead = 1;          // neck keeps the head from slamming/whipping
s32 g_RagdollReflexStumble = 1;       // shot while running, the legs take a step or two
s32 g_RagdollReflexClutchChance = 53; // % of deaths where a hand goes to the wound
s32 g_RagdollHeadshotLimpChance = 40; // % of headshot deaths with no reaction at all (instantly limp)
s32 g_RagdollHeadshotLifelessChance = 40; // % of those that also take no push from the shot: they just drop
s32 g_RagdollDismember = 1;            // strong weapons/explosions can take limbs off
f32 g_RagdollDismemberDamage = 2.0f;   // weapon damage needed (Falcon 2 = 1, DY357 = 2, FarSight = 100)
s32 g_RagdollDismemberChance = 40;     // % chance when strong enough
s32 g_RagdollDismemberExplosions = 1;
s32 g_RagdollDismemberTorso = 1;       // the heaviest hits/blasts can split the torso from the hips
s32 g_RagdollLedgeCatch = 1;           // an arm or leg left on a ledge holds the body for a moment as it goes over
s32 g_RagdollBodyAlerts = 1;           // a body thrown into a guard staggers him; a body moved into view is a seen death
f32 g_RagdollDeadlyPunchPushScale = 3.0f; // Deadly Punch punches push bodies this much harder
f32 g_RagdollSlopeGrip = 0.70f;        // body parts' friction on slopes (0 off)
f32 g_RagdollScreamAirTime = 1.5f;     // seconds in the air before a body screams (0 never; world.c)
f32 g_RagdollGlassBreakSpeed = 420.0f; // a body part hitting breakable glass this fast (units/s) breaks it
s32 g_RagdollBodyCover = 1;            // a body the player holds in front of him stops shots at him
f32 g_RagdollReflexStrength = 2.0f;   // reflex torque multiplier (0-3)
s32 g_RagdollMpSolid = RD_SOLID_COMBATSIM; // MOD: combat simulator ragdolls - resting bodies turn solid (RD_SOLID_*)

#ifdef PD_PHYSICS

bool g_RagdollLevelLoaded = false;

/**
 * A value between lo and hi, both included, for one body's ranged setting.
 * Uses its own random numbers so the game's (AI, weapons) aren't disturbed.
 */
f32 rdRollRange(f32 lo, f32 hi)
{
	static u32 seed = 0x9e3779b9u;
	f32 t;

	if (lo > hi) {
		f32 tmp = lo;
		lo = hi;
		hi = tmp;
	}

	if (hi - lo < 0.0001f) {
		return hi;
	}

	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	t = (f32)(seed & 0xffffff) / (f32)0xffffff;

	return lo + (hi - lo) * t;
}
struct chrdata *g_RagdollCreatingPlayer = NULL; // ragdollOnChrDeath() is making the player's body
static bool g_RagdollSpecialDieEnd = false; // ragdollOnSpecialDieEnd(): a scripted death fall has just ended

static void ragdollReleaseAll(void);
static void ragdollTickAll(void);
static void ragdollEnforceBodyLimit(void);
static void ragdollEnforcePhysicalLimit(s32 extra);

static void ragdollLog(int level, const char *msg)
{
	sysLogPrintf(level, "%s", msg);
}

void ragdollStageLoad(void)
{
	struct meshbuilder mb;
	struct meshbuilder mbwall;
	struct geostats stats;
	pdphysLevelStats physstats;
	u64 starttime;
	s32 roomnum;
	s32 emptyrooms = 0;

	g_RagdollLevelLoaded = false;
	rdObjGrabReset();
	memset(g_RdPlayerCam, 0, sizeof(g_RdPlayerCam));
	memset(g_RdPlayerGuns, 0, sizeof(g_RdPlayerGuns));
	memset(&g_RdVolley, 0, sizeof(g_RdVolley));
	rdFreeRoomArrays();
	rdFreeRoomTris();

	free(g_RdPropSnaps);
	g_RdPropSnaps = NULL;

	if (!pdphysIsReady()) {
		return;
	}

	ragdollReleaseAll();
	pdphysLevelClear();

	if (g_TileFileData.u8 == NULL || g_TileRooms == NULL || g_TileNumRooms <= 0) {
		sysLogPrintf(LOG_WARNING, "PHYSICS: stage 0x%02x has no collision tiles", g_Vars.stagenum);
		return;
	}

	starttime = sysGetMicroseconds();
	memset(&mb, 0, sizeof(mb));
	memset(&mbwall, 0, sizeof(mbwall));
	memset(&stats, 0, sizeof(stats));

	for (roomnum = 0; roomnum < g_TileNumRooms; roomnum++) {
		mbReset(&mb);
		mbReset(&mbwall);
		ragdollBuildRoom(&mb, &mbwall, roomnum, &stats);

		if (mb.numtris > 0) {
			pdphysLevelAddRoomGroup(roomnum, RD_GROUP_FLOORS, mb.verts, mb.numverts, mb.indices, mb.numtris);
		}

		if (mbwall.numtris > 0) {
			pdphysLevelAddRoomGroup(roomnum, RD_GROUP_WALLS, mbwall.verts, mbwall.numverts, mbwall.indices, mbwall.numtris);
		}

		if (mb.numtris == 0 && mbwall.numtris == 0) {
			emptyrooms++;
		}
	}

	mbFree(&mb);
	mbFree(&mbwall);
	pdphysLevelFinish(&physstats);

	// Sized later (rdEnsureRoomArrays): this runs before bgReset, while
	// g_Vars.roomcount is still the previous stage's
	rdFreeRoomArrays();
	rdEnsureRoomArrays();
	rdFreeRoomTris();
	g_RdCeilingRooms = 0;
	g_RdCeilingTris = 0;
	g_RdInvisibleRooms = 0;
	g_RdInvisibleTiles = 0;
	g_RdInvisLowered = 0;
	g_RdPropMeshLogged = 0;
	free(g_RdPropSnaps);
	g_RdPropSnaps = NULL;

	g_RagdollLevelLoaded = physstats.bodies > 0;

	sysLogPrintf(LOG_NOTE, "PHYSICS: stage 0x%02x collision built in %.1f ms: %d rooms (%d empty, %d failed), %d triangles, %d bodies",
			g_Vars.stagenum, (sysGetMicroseconds() - starttime) / 1000.0f,
			g_TileNumRooms, emptyrooms, physstats.failedrooms, physstats.triangles, physstats.bodies);
	sysLogPrintf(LOG_NOTE, "PHYSICS: geo: %d int tiles (%d wall, %d floor, %d other), %d float tiles, %d blocks, %d cylinders, %d unknown",
			stats.tilei, stats.tileswall, stats.tilesfloor, stats.tilesother, stats.tilef, stats.blocks, stats.cyls, stats.unknown);
}

void ragdollStageUnload(void)
{
	rdWavStopAll();
	ragdollReleaseAll();
	rdObjGrabReset();

	if (g_RagdollLevelLoaded) {
		pdphysLevelClear();
		g_RagdollLevelLoaded = false;
	}

	rdFreeRoomArrays();
	rdFreeRoomTris();
	free(g_RdPropSnaps);
	g_RdPropSnaps = NULL;
}
#define RD_SETTLETIME60 (60 * 6) // treat as settled (ACT_DEAD) after 6 s even if still moving
#define RD_MPBOTDEAD60  (60 * 2) // MOD: combat simulator ragdolls - a simulant's respawn countdown starts 2 s after death at the latest, like the death animations
// Extra chr/model/prop slots per level (about 1.3 KB each): guards spawned
// during a mission, test dummies and the Combat Simulator's bodies (each
// respawn leaves one as a new chr). As many as Max Bodies allows.
#define RD_EXTRACHRSLOTS RD_MAXBODIES_LIMIT

/*
 * Human. Masses for an average adult (about 80 kg; segment shares after
 * Winter / de Leva), merged: chest = upper + middle trunk, forearm + hand,
 * shin + foot. Elbows and knees on the same side bend opposite ways (elbow
 * forward, knee backward).
 */
static const struct rdskel g_RdSkelHuman = {
	"human", 15, true,
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 },
	{ -1, 0, 1, 1, 1, 3, 4, 5, 6, 0, 0, 9, 10, 11, 12 },
	{ 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0 },
	{ -1, 2, -1, 5, 6, 7, 8, -1, -1, 11, 12, 13, 14, -1, -1 },
	{ 0, 0, 0, 0, 0, -1, 1, 0, 0, 0, 0, 1, -1, 0, 0 },
	{ 11.0f, 26.0f, 5.5f, 2.2f, 2.2f, 1.8f, 1.8f, 0, 0, 11.4f, 11.4f, 4.5f, 4.5f, 0, 0 },
	{ 0, 150.0f, 25.0f, 45.0f, 45.0f, 25.0f, 25.0f, 0, 0, 120.0f, 120.0f, 70.0f, 70.0f, 0, 0 },
	{ 0, 0.30f, 0.17f, 0.15f, 0.15f, 0.12f, 0.12f, 0, 0, 0.19f, 0.19f, 0.16f, 0.16f, 0, 0 },
};

/*
 * Skedar (g_SkelSkedar), from a runtime dump of Skedar Ruins' Skedar: the
 * root is the pelvis, node 7 the chest the arms hang from, node 1 the head
 * and neck, with the tail (nodes 2-6) hanging from it. Arms 29-30-31 and
 * 22-23-24 (fingers after), legs 15-16-17 and 8-9-10 (toes after). The tail
 * tip, fingers and toes move with their parts. Shapes come from the hit
 * boxes. The head (with the tail: it hangs off it), limbs and tail can be
 * cut off; not the chest.
 */
static const struct rdskel g_RdSkelSkedar = {
	"skedar", 19, false,
	{ 0, 7, 1, 29, 22, 30, 23, 31, 24, 15, 8, 16, 9, 17, 10, 2, 3, 4, 5 },
	{ -1, 0, 0, 1, 1, 3, 4, 5, 6, 0, 0, 9, 10, 11, 12, 2, 15, 16, 17 },
	{ 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1 },
	{ -1, -1, -1, 5, 6, 7, 8, -1, -1, 11, 12, 13, 14, -1, -1, 16, 17, 18, -1 },
	{ 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0 },
	{ 0 },
	{ 0, 200.0f, 60.0f, 60.0f, 60.0f, 35.0f, 35.0f, 0, 0, 160.0f, 160.0f, 100.0f, 100.0f, 0, 0, 50.0f, 35.0f, 20.0f, 12.0f },
	{ 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1 },
};

// The part of skeleton 'skel' a model POSITION node part is, or -1
s32 rdPartOfNode(const struct rdskel *skel, s32 nodepart)
{
	s32 i;

	for (i = 1; i < skel->numparts; i++) {
		if (skel->nodepart[i] == nodepart) {
			return i;
		}
	}

	return -1;
}

// The ragdoll skeleton for a model's skeleton, or NULL
static const struct rdskel *ragdollSkelFor(struct model *model)
{
	if (model == NULL || model->definition == NULL) {
		return NULL;
	}

	if (model->definition->skel == &g_SkelChr) {
		return &g_RdSkelHuman;
	}

	if (model->definition->skel == &g_SkelSkedar) {
		return &g_RdSkelSkedar;
	}

	return NULL;
}

#define RD_COLROOM_MOVE     12.0f   // collision rooms are redone at once after the pelvis moves this far
#define RD_KILLFLOOR_Y      -32000.0f // bodies falling below this are stopped (and NPCs removed)
#define RD_PROPWAKE_HOLD60  90        // a woken body isn't frozen by the physical limit for this long

struct ragdoll g_Ragdolls[RD_MAXRAGDOLLS];

/*
 * Crash logs: what the ragdoll code is doing right now. RD_CRASHCTX at the
 * top of a function pushes a step (with the chr/part it's working on) and
 * pops it automatically when the function returns (gcc cleanup attribute).
 * The crash log prints this stack and a summary of the cut bodies.
 */
#define RD_CTXDEPTH 12

struct rdcrashstep {
	const char *what;
	s32 chrnum;
	s32 part;
	s32 frame;
};

static struct rdcrashstep g_RdCtx[RD_CTXDEPTH];
static s32 g_RdCtxDepth = 0;
static struct rdcrashstep g_RdCtxLast; // the last top-level step that finished
static s32 g_RdCtxCalls = 0;

s32 rdCtxEnter(const char *what, s32 chrnum, s32 part)
{
	s32 d = g_RdCtxDepth++;

	if (d >= 0 && d < RD_CTXDEPTH) {
		g_RdCtx[d].what = what;
		g_RdCtx[d].chrnum = chrnum;
		g_RdCtx[d].part = part;
		g_RdCtx[d].frame = g_Vars.lvframe60;
	}

	g_RdCtxCalls++;

	return d;
}

void rdCtxLeave(s32 *token)
{
	g_RdCtxDepth = *token;

	if (g_RdCtxDepth == 0) {
		g_RdCtxLast = g_RdCtx[0];
	}
}

static void ragdollCrashInfo(char *buf, s32 len)
{
	s32 n = 0;
	s32 i;
	s32 numused = 0;
	s32 numcut = 0;

#define RD_INFO(...) if (n < len) n += snprintf(buf + n, len - n, __VA_ARGS__)

	RD_INFO("RAGDOLLS: stage 0x%x, frame %d, %d ragdoll calls so far\n", g_Vars.stagenum, g_Vars.lvframe60, g_RdCtxCalls);

	if (g_RdCtxDepth > 0) {
		RD_INFO("Crashed INSIDE the ragdoll code:");

		for (i = 0; i < g_RdCtxDepth && i < RD_CTXDEPTH; i++) {
			RD_INFO("%s %s (chr %d, part %d)", i ? " >" : "", g_RdCtx[i].what, g_RdCtx[i].chrnum, g_RdCtx[i].part);
		}

		RD_INFO("\n");
	} else {
		RD_INFO("Not inside the ragdoll code. Last ragdoll step: %s (chr %d, part %d) at frame %d\n",
				g_RdCtxLast.what ? g_RdCtxLast.what : "none", g_RdCtxLast.chrnum, g_RdCtxLast.part, g_RdCtxLast.frame);
	}

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];

		if (rd->state == RDSTATE_FREE) {
			continue;
		}

		numused++;

		if (rd->severed || rd->isplayer) {
			numcut++;

			if (numcut <= 24) {
				RD_INFO("  ragdoll %d: chr %d%s, state %d, age %d, cut parts 0x%04x, %d display lists patched, %d caps (%d discs)\n",
						i, RD_RDCHRNUM(rd), rd->isplayer ? " (player)" : "", rd->state, rd->age60, rd->severed,
						rd->numpatches, rd->numcaps, rd->numdisccaps);
			}
		}
	}

	RD_INFO("%d ragdolls in use, %d cut or player bodies\n", numused, numcut);
	RD_INFO("Settings: Dismember %d, DismemberDamage %.2f, Chance %d, Explosions %d, Torso %d, AnimBlend %d, DeathCam %d\n",
			g_RagdollDismember, g_RagdollDismemberDamage, g_RagdollDismemberChance, g_RagdollDismemberExplosions,
			g_RagdollDismemberTorso, g_RagdollAnimBlend, g_RagdollDeathCam);
#undef RD_INFO
}
s32 g_RagdollNumUsed = 0;
static u32 g_RagdollSerial = 0;

/* ---- small matrix helpers (Mtxf is column-major: m[col][row]) ---- */

void rdVecSub(const f32 *a, const f32 *b, f32 *out)
{
	out[0] = a[0] - b[0];
	out[1] = a[1] - b[1];
	out[2] = a[2] - b[2];
}

f32 rdVecLen(const f32 *v)
{
	return sqrtf(rdVecLenSq(v));
}

void rdVecNorm(f32 *v)
{
	f32 len = rdVecLen(v);

	if (len > 0.0001f) {
		rdVecDiv(v, len);
	} else {
		rdVecSet(v, 0.0f, 1.0f, 0.0f);
	}
}

void rdVecMad(const f32 *a, const f32 *dir, f32 scale, f32 *out)
{
	out[0] = a[0] + dir[0] * scale;
	out[1] = a[1] + dir[1] * scale;
	out[2] = a[2] + dir[2] * scale;
}

// out = R(a)^T * R(b), rotation only
void rdRotTransposeMul(Mtxf *a, Mtxf *b, Mtxf *out)
{
	s32 c, r;

	for (c = 0; c < 3; c++) {
		for (r = 0; r < 3; r++) {
			// (A^T B)[r][c] = sum_k A[k][r] * B[k][c] = dot(colA_r, colB_c)
			out->m[c][r] = rdVecDot(a->m[r], b->m[c]);
		}

		out->m[c][3] = 0.0f;
	}

	out->m[3][0] = out->m[3][1] = out->m[3][2] = 0.0f;
	out->m[3][3] = 1.0f;
}

// out = R(a) * R(b), rotation only
void rdRotMul(Mtxf *a, Mtxf *b, Mtxf *out)
{
	s32 c, r;

	for (c = 0; c < 3; c++) {
		for (r = 0; r < 3; r++) {
			out->m[c][r] = a->m[0][r] * b->m[c][0] + a->m[1][r] * b->m[c][1] + a->m[2][r] * b->m[c][2];
		}

		out->m[c][3] = 0.0f;
	}
}

// out = R(a) * v
void rdRotApply(Mtxf *a, const f32 *v, f32 *out)
{
	s32 r;

	for (r = 0; r < 3; r++) {
		out[r] = a->m[0][r] * v[0] + a->m[1][r] * v[1] + a->m[2][r] * v[2];
	}
}

// out = R(a)^T * v
void rdRotApplyT(Mtxf *a, const f32 *v, f32 *out)
{
	s32 c;

	for (c = 0; c < 3; c++) {
		out[c] = rdVecDot(a->m[c], v);
	}
}

/* ---- bookkeeping ---- */

u32 g_RagdollChangeSerial = 1; // bumped whenever a ragdoll is added or removed
bool g_RagdollSamplingAnim = false; // render hooks stand aside while the death animation is sampled

static struct ragdoll *ragdollFindByModel(struct model *model)
{
	// The render hooks ask once per node, so remember the last answer
	static struct model *lastmodel = NULL;
	static struct ragdoll *lastrd = NULL;
	static u32 lastserial = 0;
	s32 i;

	if (g_RagdollNumUsed == 0) {
		return NULL;
	}

	if (model == lastmodel && lastserial == g_RagdollChangeSerial) {
		return lastrd;
	}

	lastmodel = model;
	lastserial = g_RagdollChangeSerial;
	lastrd = NULL;

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		if (g_Ragdolls[i].state != RDSTATE_FREE && g_Ragdolls[i].model == model) {
			lastrd = &g_Ragdolls[i];
			break;
		}
	}

	return lastrd;
}

struct ragdoll *ragdollFindByChr(struct chrdata *chr)
{
	s32 i;

	if (g_RagdollNumUsed == 0 || chr == NULL) {
		return NULL;
	}

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		if (g_Ragdolls[i].state != RDSTATE_FREE && g_Ragdolls[i].chr == chr) {
			return &g_Ragdolls[i];
		}
	}

	return NULL;
}

// The simulating ragdoll with this physics handle, or NULL
struct ragdoll *ragdollFindByHandle(s32 handle)
{
	s32 i;

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		if (g_Ragdolls[i].state == RDSTATE_SIMULATING && g_Ragdolls[i].handle == handle) {
			return &g_Ragdolls[i];
		}
	}

	return NULL;
}

static void ragdollFreeze(struct ragdoll *rd)
{
	RD_CRASHCTX("freeze", RD_RDCHRNUM(rd), -1);
	rd->powered = false;
	rd->blending = false;
	rd->reflexes = false;
	rd->headguard = false; // woken later (shot, lift, grab): the head isn't held where it was at death
	rd->driven = false;

	if (rd->state == RDSTATE_SIMULATING) {
		pdphysRagdollDestroy(rd->handle);
		rd->handle = -1;
		rd->state = RDSTATE_FROZEN;
	}
}

static void ragdollRelease(struct ragdoll *rd)
{
	RD_CRASHCTX("release", RD_RDCHRNUM(rd), -1);
	if (rd->state == RDSTATE_FREE) {
		return;
	}

	ragdollFreeze(rd);
	ragdollRestoreSeams(rd);
	ragdollFreeCaps(rd);
	rd->severed = 0;

	if (rd->prop && (rd->prop->type == PROPTYPE_CHR || rd->prop->type == PROPTYPE_PLAYER)) {
		rd->prop->flags &= ~PROPFLAG_RENDERPOSTBG;
	}

	rd->state = RDSTATE_FREE;
	rd->chr = NULL;
	rd->model = NULL;
	rd->prop = NULL;
	g_RagdollNumUsed--;
	g_RagdollChangeSerial++;
}

static void ragdollReleaseAll(void)
{
	s32 i;

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		ragdollRelease(&g_Ragdolls[i]);
	}

	g_RagdollNumUsed = 0;
	ragdollPropsReleaseAll();
}

/* ---- pose ---- */

// Recomputes the render data (root matrix + local rotations) from rd->world
static void ragdollUpdateRenderPose(struct ragdoll *rd)
{
	s32 i;
	s32 c, r;

	// Root: world frame with the model scale applied
	for (c = 0; c < 3; c++) {
		for (r = 0; r < 3; r++) {
			rd->rootrender.m[c][r] = rd->world[0].m[c][r] * rd->scale;
		}

		rd->rootrender.m[c][3] = 0.0f;
	}

	rdVecCopy(rd->world[0].m[3], rd->rootrender.m[3]);
	rd->rootrender.m[3][3] = 1.0f;

	for (i = 1; i < rd->numparts; i++) {
		Mtxf local;
		rdRotTransposeMul(&rd->world[rd->skel->parent[i]], &rd->world[i], &local);
		quaternion0f097044(&local, rd->localquat[i]);

		if (rd->severed & (1u << i)) {
			// Cut off: wherever the physics put it, relative to the old parent
			f32 diff[3];
			f32 off[3];

			rdVecSub(rd->world[i].m[3], rd->world[rd->skel->parent[i]].m[3], diff);
			rdRotApplyT(&rd->world[rd->skel->parent[i]], diff, off);
			rd->renderpos[i].x = off[0] / rd->scale;
			rd->renderpos[i].y = off[1] / rd->scale;
			rd->renderpos[i].z = off[2] / rd->scale;
		} else {
			rd->renderpos[i] = rd->localpos[i];
		}
	}
}

// Fills in world frames of parts without a physics body from their parent
static void ragdollPropagateRigidParts(struct ragdoll *rd)
{
	s32 i;

	for (i = 1; i < rd->numparts; i++) {
		if (rd->bodyindex[i] < 0) {
			s32 p = rd->skel->parent[i];
			f32 local[3];
			f32 offset[3];

			rdRotMul(&rd->world[p], &rd->localrot[i], &rd->world[i]);

			local[0] = rd->localpos[i].x * rd->scale;
			local[1] = rd->localpos[i].y * rd->scale;
			local[2] = rd->localpos[i].z * rd->scale;

			// DK Mode: a hand is drawn out at the arm's scale
			if (rdDkArmPart(rd, i)) {
				rdVecScale(local, RD_DK_ARMSCALE, local);
			}
			rdRotApply(&rd->world[p], local, offset);

			rdVecAdd(rd->world[p].m[3], offset, rd->world[i].m[3]);
			rd->world[i].m[3][3] = 1.0f;
		}
	}
}


/**
 * How hard a shot pushes the body, in PD units per second.
 *
 * Weapon stats vary wildly (damage is ~0.6-2.4 for normal guns but 100 for
 * the FarSight/crossbow and 200 for the golden Magnum; impact force is 0-10,
 * with the MagSec 4 at 10), so both are squashed into a narrow range and the
 * result is capped. Falcon 2 = 1.0x, the strongest guns end up at ~1.4x.
 * Pd.ini Ragdoll.RagdollPushScale scales everything.
 */
#define RD_PUSH_HIT   525.0f // push on the part that was hit (Falcon 2)
#define RD_PUSH_ALL   180.0f // push on the whole body
#define RD_PUSH_LIFT  60.0f  // upward push on the whole body
#define RD_PUSH_MAXMULT 1.45f

static void ragdollGetPushSpeeds(struct ragdoll *rd, struct gset *gset, f32 *hitspeed, f32 *allspeed, f32 *liftspeed, f32 *outdamage, f32 *outforce)
{
	f32 damage = gset ? gsetGetDamage(gset) : 0.0f;
	f32 force = gset ? gsetGetImpactForce(gset) : 0.0f;
	f32 weight;
	f32 mult;

	RD_CLAMPLO(damage, 0.0f);

	RD_CLAMPLO(force, 0.0f);

	// Damage 1 -> 1.0, 2 and above -> 2.0 (so one-shot-kill weapons count as 2)
	weight = damage;

	RD_CLAMPTO(weight, 0.5f, 2.0f);

	mult = 0.75f + 0.25f * weight + 0.025f * RD_MIN(force, 10.0f);

	RD_CLAMPHI(mult, RD_PUSH_MAXMULT);

	// Deadly Punch punches hit much harder than a normal punch
	if (cheatIsDeadlyPunch(gset)) {
		mult *= g_RagdollDeadlyPunchPushScale;
	}

	mult *= rd->pushscale;

	*hitspeed = RD_PUSH_HIT * mult;
	*allspeed = RD_PUSH_ALL * mult;
	*liftspeed = RD_PUSH_LIFT * mult;

	if (outdamage) {
		*outdamage = damage;
	}

	if (outforce) {
		*outforce = force;
	}
}

/**
 * MOD: combat simulator ragdolls - solo/co-op and the Combat Simulator have
 * their own limits. Max Active Bodies: the most simulated at once.
 */
s32 ragdollMaxActive(void)
{
	s32 setting = g_Vars.normmplayerisrunning ? g_RagdollMpMaxPhysical : g_RagdollMaxPhysical;

	if (setting <= 0 || setting > RD_MAXRAGDOLLS) {
		return RD_MAXRAGDOLLS;
	}

	return setting < RD_MINACTIVE ? RD_MINACTIVE : setting;
}

// Max Bodies on Vanilla: bodies go as in the original
bool ragdollBodiesVanilla(void)
{
	return (g_Vars.normmplayerisrunning ? g_RagdollMpMaxBodies : g_RagdollMaxBodies) < 0;
}

// Resting Bodies Turn Solid applies to the current game
bool ragdollSolidEnabled(void)
{
	return g_RagdollMpSolid == RD_SOLID_BOTH || (g_RagdollMpSolid == RD_SOLID_COMBATSIM && g_Vars.normmplayerisrunning);
}

/**
 * Keeps at most Ragdoll.RagdollMaxBodies ragdoll corpses: the oldest settled
 * corpse is told to fade out (the game then removes it, freeing the slot).
 */
static void ragdollEnforceBodyLimit(void)
{
	RD_CRASHCTX("enforcebodylimit", -1, -1);
	s32 setting = g_Vars.normmplayerisrunning ? g_RagdollMpMaxBodies : g_RagdollMaxBodies;
	s32 limit = setting > 0 ? RD_MAX(setting, RD_MINBODIES) : RD_MAXRAGDOLLS; // (Vanilla: the game's own fading)
	s32 count = 0;
	s32 fading = 0;
	struct ragdoll *oldest = NULL;
	s32 i;

	RD_CLAMPHI(limit, RD_MAXRAGDOLLS);

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];

		if (rd->state == RDSTATE_FREE) {
			continue;
		}

		count++;

		if (rd->chr->actiontype == ACT_DEAD && (rd->chr->act_dead.fadenow || rd->chr->act_dead.fadetimer60 >= 0)) {
			fading++;
		} else if (rd->chr->actiontype == ACT_DEAD && !rd->isplayer
				&& rd->chr->aibot == NULL // (a simulant's own body: fading it would respawn it early)
				&& (oldest == NULL || rd->serial < oldest->serial)) {
			oldest = rd;
		}
	}

	// +1 for the body about to be created
	if (count - fading + 1 > limit && oldest) {
		oldest->chr->act_dead.fadenow = true;
		sysLogPrintf(LOG_NOTE, "RAGDOLL: %d bodies (max %d%s): the oldest, chr %d, fades out",
				count - fading, limit, g_Vars.normmplayerisrunning ? ", combat simulator" : "", RD_RDCHRNUM(oldest));
	}
}

/**
 * Keeps at most Ragdoll.RagdollMaxPhysical bodies in the physics world. The
 * oldest settled ones are frozen in place first (they keep their pose and
 * can be woken by a shot).
 */
static void ragdollEnforcePhysicalLimit(s32 extra)
{
	RD_CRASHCTX("enforcephysicallimit", -1, -1);
	s32 limit = ragdollMaxActive();
	s32 count = 0;
	s32 i;

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		if (g_Ragdolls[i].state == RDSTATE_SIMULATING && !g_Ragdolls[i].solid) { // (solid ones cost next to nothing)
			count++;
		}
	}

	while (count + extra > limit) {
		struct ragdoll *oldest = NULL;

		for (i = 0; i < RD_MAXRAGDOLLS; i++) {
			struct ragdoll *rd = &g_Ragdolls[i];

			if (rd->state != RDSTATE_SIMULATING || rd->isplayer || rd->solid) {
				continue;
			}

			// Not a body the player is holding, or that a lift or a pushed object has just woken up
			if (rd->grabbed || (rd->propwake60 > 0 && g_Vars.lvframe60 - rd->propwake60 < RD_PROPWAKE_HOLD60)) {
				continue;
			}

			// Nor one whose muscles still hold something up (frozen like that,
			// the head would stay off the floor)
			if (rd->driven || rd->headguard) {
				continue;
			}

			// Prefer settled bodies, then the oldest
			if (oldest == NULL
					|| (rd->settled && !oldest->settled)
					|| (rd->settled == oldest->settled && rd->serial < oldest->serial)) {
				oldest = rd;
			}
		}

		if (oldest == NULL) {
			break;
		}

		ragdollFreeze(oldest);
		count--;
	}
}

#define RD_AIRBORNE 15.0f // units: feet this far above the floor at death = in mid-air (no animation blend)
#define RD_SOLID_MAXSPEED 40.0f // units/s: any part faster is still falling, collapsing or settling
#define RD_SOLID_STILL60  18     // every part that slow for 0.3 s: lying where it is

#define RD_REACTING(rd) ((rd)->blending || (rd)->powered || (rd)->reflexes || (rd)->headguard || (rd)->driven)

// Lying where it is: no death reactions left and no part moving (every part
// counts: a body buckling at the knees drops its pelvis slowly at first)
static bool rdMpAtRest(struct ragdoll *rd)
{
	return !RD_REACTING(rd) && rd->restset && rd->still60 >= RD_SOLID_STILL60;
}

static void rdSetSolid(struct ragdoll *rd, const char *why)
{
	pdphysRagdollSetSolid(rd->handle, 1);
	rd->solid = true;
	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d %s: solid now", RD_RDCHRNUM(rd), why);
}

/**
 * MOD: combat simulator ragdolls - Resting Bodies Turn Solid: a body turns
 * static in the physics (still collides, never moves) as soon as it's done
 * reacting and lies still, whatever it lies on (a pile of bodies never lets
 * it sleep). Only a moving lift or breaking glass makes it move again.
 */
static void rdMpSolidCheck(struct ragdoll *rd)
{
	f32 dt = g_Vars.lvupdate60freal / 60.0f;
	bool moving = false;

	if (!ragdollSolidEnabled()) {
		return;
	}

	// The fastest part's speed since last tick (the player's own body too:
	// at the respawn it can turn solid at once)
	if (dt > 0.0f) {
		s32 part;

		if (rd->restset) {
			f32 fastest = 0.0f;

			for (part = 0; part < rd->numparts; part++) {
				f32 d[3];
				f32 len;

				rdVecSub(rd->world[part].m[3], rd->restpos[part], d);
				len = rdVecLen(d);

				if (len > fastest) {
					fastest = len;
				}
			}

			rd->restspeed = fastest / dt;
			moving = rd->restspeed > RD_SOLID_MAXSPEED;
		}

		for (part = 0; part < rd->numparts; part++) {
			rdVecCopy(rd->world[part].m[3], rd->restpos[part]);
		}

		rd->restset = true;

		// Asleep: its reactions are over (they only end while it's awake). The
		// dead player's body keeps its death camera control.
		if (!rd->isplayer && rd->handle >= 0 && pdphysRagdollIsAsleep(rd->handle)) {
			rd->blending = false;
			rd->powered = false;
			rd->reflexes = false;
			rd->headguard = false;
			rd->driven = false;
		}

		if (moving || RD_REACTING(rd)) {
			rd->still60 = 0;
		} else {
			rd->still60 += g_Vars.lvupdate60;
		}
	}

	if (!rd->isplayer && !rd->grabbed && !rd->riding && !rd->fellout && rd->handle >= 0
			&& !moving && rdMpAtRest(rd)) {
		rdSetSolid(rd, "done reacting and lying still");
	}
}

void ragdollUnsolid(struct ragdoll *rd, const char *why)
{
	if (rd->solid) {
		pdphysRagdollSetSolid(rd->handle, 0);
		rd->solid = false;
		sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d moves again (no longer solid: %s)", RD_RDCHRNUM(rd), why);
	}
}

#ifdef PD_GRAVITY
#define RD_FLYOUT_MARGIN 1000.0f // inverted gravity: this far above the highest room, a body has left the level

/**
 * MOD: gravity - the physics world follows Game.Gravity. When it changes,
 * the bodies lying still (asleep, or solid in the Combat Simulator) wake up
 * and follow it: flipped, they fall up to the ceiling. Frozen bodies (past
 * Max Active Bodies) stay as they are.
 */
static void ragdollFollowGravity(void)
{
	static f32 lastscale = 1.0f;
	f32 scale = gravityScale();
	s32 i;

	pdphysSetGravityScale(scale);

	if (scale == lastscale) {
		return;
	}

	lastscale = scale;

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];

		if (rd->state == RDSTATE_SIMULATING && rd->handle >= 0 && !rd->fellout) {
			ragdollUnsolid(rd, "the gravity changed");
			pdphysRagdollKeepAwake(rd->handle, 0.5f);
		}
	}
}

// The top of the level's highest room (worked out once per level)
static f32 ragdollLevelTopY(void)
{
	static void *rooms = NULL;
	static s32 numrooms = -1;
	static s32 stagenum = -1;
	static f32 top = 0.0f;
	s32 i;

	if (rooms != (void *) g_Rooms || numrooms != g_Vars.roomcount || stagenum != g_Vars.stagenum) {
		rooms = (void *) g_Rooms;
		numrooms = g_Vars.roomcount;
		stagenum = g_Vars.stagenum;
		top = -RD_FLYOUT_MARGIN;

		for (i = 1; i < numrooms; i++) {
			if (g_Rooms[i].bbmax[1] > top) {
				top = g_Rooms[i].bbmax[1];
			}
		}
	}

	return top;
}
#endif

// A frozen body gets its physics back (a shot, a blast, a grab). False if it couldn't.
static bool ragdollUnfreeze(struct ragdoll *rd)
{
	f32 zero[3] = {0, 0, 0};

	if (rd->state != RDSTATE_FROZEN) {
		return true;
	}

	ragdollEnforcePhysicalLimit(1);
	rd->handle = ragdollCreatePhysics(rd, zero);

	if (rd->handle < 0) {
		return false;
	}

	rd->state = RDSTATE_SIMULATING;
	rd->age60 = 0;

	return true;
}

#define RD_BLEND_TWITCHPUSH 30.0f  // the shot nudges the animation's path (units/s; replaced, not added)

/**
 * Called from chrHit() when a shot hits a dead or dying chr. Pushes the
 * ragdoll, waking it up first if it was frozen.
 */
void ragdollOnChrShot(struct chrdata *chr, s32 hitpart, struct coord *dir, struct gset *gset)
{
	RD_CRASHCTX("onchrshot", RD_CHRNUM(chr), hitpart);
	struct ragdoll *rd = ragdollFindByChr(chr);
	f32 hitspeed, allspeed, liftspeed;
	f32 push[3];
	f32 all[3];
	s32 body;

	if (rd == NULL || dir == NULL || !pdphysIsReady()) {
		return;
	}

	ragdollTryDismemberShot(rd, hitpart, dir, gset);

	// Shot again while still animating: a headshot hands over to the physics
	// (not a Skedar's: it shrugs it off); anywhere else the hit part jolts
	// with the shot (the animation lets go of it for a moment) and the rest
	// carries on
	if (rd->blending && rd->blendbreak60 < 0) {
		if (hitpart == HITPART_HEAD && CHRRACE(rd->chr) != RACE_SKEDAR) {
			rd->blendbreak60 = rd->age60;
		} else {
			// The hit part (and what hangs off it) is knocked along the shot
			// and springs back; nothing is added to the body's speed, so
			// repeated shots don't build up
			s32 sp = (hitpart >= 0 && hitpart < 20) ? rdHitpartPart(rd, hitpart) : 1;

			if (sp == 0) {
				sp = 1; // the hips: the upper body jolts
			}

			rd->twitchpart = sp;
			rd->twitchuntil60 = rd->age60 + RD_BLEND_TWITCH60;
			rdVecSet(rd->twitchdir, dir->x, dir->y * 0.5f, dir->z);
			rdVecNorm(rd->twitchdir);
			rd->offvel[0] = dir->x * RD_BLEND_TWITCHPUSH;
			rd->offvel[2] = dir->z * RD_BLEND_TWITCHPUSH;
			return;
		}
	}

	// Automatic weapons: at most one push every 4 frames per body
	if (rd->lastpush60 != 0 && g_Vars.lvframe60 - rd->lastpush60 < 4) {
		return;
	}

	if (!ragdollUnfreeze(rd)) {
		return;
	}

	rd->lastpush60 = RD_FRAMESTAMP();

	ragdollGetPushSpeeds(rd, gset, &hitspeed, &allspeed, &liftspeed, NULL, NULL);

	body = (hitpart >= 0 && hitpart < 20) ? rd->hitpartbody[hitpart] : -1;

	rdVecSet(push, dir->x * hitspeed * 0.45f, dir->y * hitspeed * 0.45f, dir->z * hitspeed * 0.45f);
	rdVecSet(all, dir->x * allspeed * 0.15f, dir->y * allspeed * 0.15f, dir->z * allspeed * 0.15f);

	// The whole-body push only moves the piece that was hit (cut pieces
	// are separate)
	if (rd->severed && body >= 0) {
		s32 hitskel = -1;
		s32 part;

		for (part = 0; part < rd->numparts; part++) {
			if (rd->bodyindex[part] == body) {
				hitskel = part;
				break;
			}
		}

		for (part = 0; part < rd->numparts && hitskel >= 0; part++) {
			if (rd->bodyindex[part] >= 0 && ragdollPieceOf(rd, part) == ragdollPieceOf(rd, hitskel)) {
				pdphysRagdollAddVelocity(rd->handle, rd->bodyindex[part], all);
			}
		}
	} else {
		pdphysRagdollAddVelocity(rd->handle, -1, all);
	}

	if (body >= 0) {
		pdphysRagdollAddVelocity(rd->handle, body, push);
	}
}

/**
 * Called at the end of chrBeginDeath(). Turns a dying human into a ragdoll
 * if possible; otherwise the normal death animation plays.
 */
void ragdollOnChrDeath(struct chrdata *chr, struct coord *dir, s32 hitpart, struct gset *gset, bool knockout)
{
	RD_CRASHCTX("onchrdeath", RD_CHRNUM(chr), hitpart);
	struct ragdoll *rd = NULL;
	struct prop *prop;
	f32 vel[3] = {0, 0, 0};
	f32 force, damage, hitspeed, allspeed, liftspeed;
	f32 hspeed;
	bool limp;
	bool lifeless = false; // a limp headshot that also takes no push: it just drops
	s32 i;

	if ((rd = ragdollFindByChr(chr)) != NULL) {
		// A ragdolled knockout killed (shot, punched, a hard landing): the
		// body carries on as it is, now dying
		if (rd->knockedout && chr->actiontype == ACT_DIE) {
			chr->act_die.timeextra = 0;
			sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d was knocked out, now dead", chr->chrnum);
		}

		return; // (a ragdoll shot again: ragdollOnChrShot)
	}

	if (!g_RagdollsEnabled || !g_RagdollLevelLoaded || !pdphysIsReady() || chr == NULL) {
		return;
	}

	prop = chr->prop;

	// Only real deaths of non-player humans and Skedar (or the dead player's
	// own body, for the ragdoll death camera). MOD: combat simulator ragdolls -
	// the Combat Simulator's simulants too (solo AI buddies stay as they were)
	if (knockout
			|| (chr->actiontype != ACT_DIE && chr != g_RagdollCreatingPlayer
				&& !(g_RdCreatingKnockout && chr->actiontype == ACT_DRUGGEDDROP))
			|| prop == NULL
			|| (prop->type != PROPTYPE_CHR && !(chr == g_RagdollCreatingPlayer && prop->type == PROPTYPE_PLAYER))
			|| chr->model == NULL
			|| chr->model->anim == NULL
			|| ragdollSkelFor(chr->model) == NULL // (humans and Skedar)
			|| (chr->aibot != NULL && !g_Vars.normmplayerisrunning)
			|| g_Vars.tickmode == TICKMODE_CUTSCENE
			|| (chr->specialdie != SPECIALDIE_NONE && !g_RagdollSpecialDieEnd)) {
		return;
	}

	// Too many bodies: fade out the oldest corpse (it frees its slot when gone)
	ragdollEnforceBodyLimit();

	// Find a free slot
	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		if (g_Ragdolls[i].state == RDSTATE_FREE) {
			rd = &g_Ragdolls[i];
			break;
		}
	}

	if (rd == NULL) {
		sysLogPrintf(LOG_NOTE, "RAGDOLL: all %d ragdoll slots in use, using death animation", RD_MAXRAGDOLLS);
		return;
	}

	memset(rd, 0, sizeof(*rd));
	rd->skel = ragdollSkelFor(chr->model);
	rd->numparts = rd->skel->numparts;

	if (!ragdollCapturePose(rd, chr)) {
		return;
	}

	// Keep the number of bodies in the physics world bounded
	ragdollEnforcePhysicalLimit(1);

	// Carry over the chr's movement from the last animation tick
	if (g_Vars.lvupdate60freal > 0.0f && chr != g_RagdollCreatingPlayer) {
		struct coord rootpos;
		f32 speed;

		modelGetRootPosition(chr->model, &rootpos);
		rdVecSet(vel, (rootpos.x - chr->prevpos.x) * 60.0f / g_Vars.lvupdate60freal, 0.0f, (rootpos.z - chr->prevpos.z) * 60.0f / g_Vars.lvupdate60freal);
		speed = rdVecLen(vel);

		if (speed > 600.0f) {
			rdVecScale(vel, 600.0f / speed, vel);
		}
	}

	rd->handle = ragdollCreatePhysics(rd, vel);

	if (rd->handle < 0) {
		sysLogPrintf(LOG_WARNING, "RAGDOLL: chr %d physics creation failed, using death animation", chr->chrnum);
		return;
	}

	// (rd was zeroed above: only what isn't 0/false is set here)
	rd->state = RDSTATE_SIMULATING;
	rd->chr = chr;
	rd->model = chr->model;
	rd->prop = prop;
	rd->serial = ++g_RagdollSerial;
	rd->lastexplosion60 = -1;
	rd->blendbreak60 = -1;
	rd->twitchpart = -1;
	rd->legcut60 = -1;
	rd->clutcharm = -1;
	rd->clutchpart = -1;
	rd->pushscale = g_RagdollPushScale;
	rd->musclestrength = rdRollRange(g_RagdollMuscleStrengthMin, g_RagdollMuscleStrength);
	rd->muscletime = rdRollRange(g_RagdollMuscleTimeMin, g_RagdollMuscleTime);
	rd->reflexstrength = rdRollRange(g_RagdollReflexStrengthMin, g_RagdollReflexStrength);
	// No blend for a scripted death (its animation has played out), a knockout or a Deadly Punch kill
	rd->blending = g_RagdollAnimBlend > 0 && (s32)(rngRandom() % 100) < g_RagdollAnimBlend && !g_RagdollSpecialDieEnd
		&& !g_RdCreatingKnockout && !cheatIsDeadlyPunch(gset);

	// Killed in mid-air (feet at manground, the floor at ground): no blend,
	// or the death animation holds the body up where it was hit
	if (rd->blending && chr->manground - chr->ground > RD_AIRBORNE) {
		rd->blending = false;
		sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d killed in mid-air (%.0f above the floor): no animation blend, it falls",
				chr->chrnum, chr->manground - chr->ground);
	}
	rd->powered = !rd->blending && g_RagdollMuscles && rd->musclestrength > 0.0f && !g_RagdollSpecialDieEnd;
	rd->reflexes = rd->skel->human; // (bracing, clutching: human poses)
	rd->headguard = rd->skel->human;
	rdVecCopy(rd->world[1].m[3], rd->prevchest);
	rdRotTransposeMul(&rd->world[1], &rd->world[2], &rd->headrel);
	rd->torso = rdTorsoLength(rd);
	rd->deathground = rd->blendfloor = rd->blendfloor0 = chr->ground;
	rd->prevy[0] = rd->world[0].m[3][1];
	rd->prevy[1] = rd->world[1].m[3][1];

	// Which reactions this death gets (the rolls in this order: the random
	// sequence is part of the regression suite's determinism). A headshot:
	// lights out, no muscle tone and no reflexes (not a Skedar: its death
	// animation plays on).
	limp = hitpart == HITPART_HEAD && CHRRACE(chr) != RACE_SKEDAR && (s32)(rngRandom() % 100) < g_RagdollHeadshotLimpChance;
	hspeed = rdVecLen(vel);
	rd->canbrace = (s32)(rngRandom() % 100) < g_RagdollReflexBraceChance;
	rd->movedir[0] = hspeed > 1.0f ? vel[0] / hspeed : 0.0f;
	rd->movedir[1] = hspeed > 1.0f ? vel[2] / hspeed : 0.0f;
	rd->stumble = g_RagdollReflexStumble && hspeed > 150.0f;
	rd->stumblesteps = 1 + rngRandom() % 3;
	rd->stumbleleg = rngRandom() % 2;

	if (hitpart >= 0 && hitpart < 20 && hitpart != HITPART_HEAD
			&& (s32)(rngRandom() % 100) < g_RagdollReflexClutchChance) {
		s32 sp = rd->hitskel[hitpart];

		// Torso: either hand. Upper arm/thigh: the other side's hand.
		if (sp == 0 || sp == 1) {
			rd->clutchpart = sp;
			rd->clutcharm = 3 + rngRandom() % 2;
		} else if (sp == 3 || sp == 4 || sp == 9 || sp == 10) {
			rd->clutchpart = sp;
			rd->clutcharm = (sp == 3 || sp == 9) ? 4 : 3;
		}
	}

	if (limp) {
		lifeless = (s32)(rngRandom() % 100) < g_RagdollHeadshotLifelessChance;
		rd->powered = false;
		rd->blending = false;
		rd->reflexes = false;
		rd->stumble = false;
		rd->clutcharm = -1;
	}

	g_RagdollChangeSerial++;
	g_RagdollNumUsed++;

	// Shot impulse: strong push on the part that was hit, a smaller push
	// plus a little lift on the whole body so it gets knocked back
	ragdollGetPushSpeeds(rd, gset, &hitspeed, &allspeed, &liftspeed, &damage, &force);

	if (lifeless) {
		hitspeed = allspeed = liftspeed = 0.0f;
		sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d headshot: drops lifeless", chr->chrnum);
	}

	if (dir && !lifeless) {
		f32 push[3];
		f32 all[3];
		s32 body = (hitpart >= 0 && hitpart < 20) ? rd->hitpartbody[hitpart] : -1;

		rdVecSet(push, dir->x * hitspeed, dir->y * hitspeed, dir->z * hitspeed);

		rdVecSet(all, dir->x * allspeed, dir->y * allspeed + liftspeed, dir->z * allspeed);

		pdphysRagdollAddVelocity(rd->handle, -1, all);

		if (body >= 0) {
			pdphysRagdollAddVelocity(rd->handle, body, push);
		}

		// With the animation blend the animation leads at first: the shot
		// knocks its path back instead (decays), so the impact still shows
		rdVecSet(rd->offvel, dir->x * (allspeed + hitspeed * 0.25f), 0.0f, dir->z * (allspeed + hitspeed * 0.25f));
	}

	// Strong enough weapons take the hit limb or head off
	g_RdDismemberAtDeath = true;
	ragdollTryDismemberShot(rd, hitpart, dir, gset);
	g_RdDismemberAtDeath = false;

	// The animation no longer drives this chr
	chr->act_die.timeextra = 0;

	ragdollUpdateRenderPose(rd);

	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d (body %d) ragdolled: weapon %d, hitpart %d, damage %.2f, impact %.1f, push %.0f/%.0f, dir (%.2f %.2f %.2f), torso %.1f, scale %.4f, slot %d",
			chr->chrnum, chr->bodynum, gset ? gset->weaponnum : -1, hitpart, damage, force, hitspeed, allspeed, dir ? dir->x : 0.0f, dir ? dir->y : 0.0f, dir ? dir->z : 0.0f,
			rdVecLen((f32[3]){ rd->world[2].m[3][0] - rd->world[1].m[3][0], rd->world[2].m[3][1] - rd->world[1].m[3][1], rd->world[2].m[3][2] - rd->world[1].m[3][2] }),
			rd->scale, (s32)(rd - g_Ragdolls));

	if (g_RagdollMuscleStrengthMin < g_RagdollMuscleStrength
			|| g_RagdollMuscleTimeMin < g_RagdollMuscleTime || g_RagdollReflexStrengthMin < g_RagdollReflexStrength) {
		sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d rolled stiffness %.2fx for %.2f s, reflexes %.2fx",
				chr->chrnum, rd->musclestrength, rd->muscletime, rd->reflexstrength);
	}
}

/**
 * Knockouts: chrDamage reports the blow (or dose) that knocks a guard out,
 * and the next tick chrTickDruggedComingUp asks ragdollOnKnockOutFall to make
 * him a ragdoll instead of playing the fall. He stays knocked out, not dead:
 * ACT_DRUGGEDDROP until the body comes to rest, then ACT_DRUGGEDKO.
 */
#define RD_KO_PENDING 4
#define RD_KO_MAXAGE60 30

static struct rdpendingko {
	struct chrdata *chr;
	struct coord dir;
	s32 hitpart;
	struct gset gset;
	s32 frame60;
} g_RdPendingKo[RD_KO_PENDING];

bool g_RdCreatingKnockout = false;
bool g_RdDismemberAtDeath = false; // ragdollTryDismemberShot is for the killing hit, not a hit on a body

void ragdollOnChrKnockOut(struct chrdata *chr, struct coord *dir, s32 hitpart, struct gset *gset)
{
	struct rdpendingko *pk;
	s32 i;
	s32 slot = 0;

	if (!g_RagdollsEnabled || chr == NULL) {
		return;
	}

	// Only blows push the body (a punch, a pistol whip); drugs just drop it
	if (gset == NULL || !gsetHasFunctionFlags(gset, FUNCFLAG_BLUNTIMPACT)) {
		dir = NULL;
	}

	for (i = 0; i < RD_KO_PENDING; i++) {
		if (g_RdPendingKo[i].chr == chr || g_RdPendingKo[i].chr == NULL) {
			slot = i;
			break;
		}

		if (g_RdPendingKo[i].frame60 < g_RdPendingKo[slot].frame60) {
			slot = i;
		}
	}

	pk = &g_RdPendingKo[slot];
	pk->chr = chr;
	pk->hitpart = hitpart;
	pk->frame60 = RD_FRAMESTAMP();

	if (gset) {
		pk->gset = *gset;
	} else {
		memset(&pk->gset, 0, sizeof(pk->gset));
		pk->gset.weaponnum = WEAPON_NONE;
	}

	pk->dir.x = pk->dir.y = pk->dir.z = 0.0f;

	if (dir) {
		f32 len = sqrtf(dir->x * dir->x + dir->y * dir->y + dir->z * dir->z);

		pk->dir = *dir;

		if (len > 0.001f) {
			pk->dir.x /= len;
			pk->dir.y /= len;
			pk->dir.z /= len;
		}
	}
}

s32 ragdollOnKnockOutFall(struct chrdata *chr)
{
	struct ragdoll *rd;
	struct coord *dir = NULL;
	struct gset *gset = NULL;
	s32 hitpart = -1;
	s32 now = RD_FRAMESTAMP();
	s32 i;

	if (!g_RagdollsEnabled || chr == NULL || chr->actiontype != ACT_DRUGGEDDROP) {
		return false;
	}

	// The blow that did it, if chrDamage reported one just now
	for (i = 0; i < RD_KO_PENDING; i++) {
		struct rdpendingko *pk = &g_RdPendingKo[i];

		if (pk->chr == chr) {
			pk->chr = NULL;

			if (now >= pk->frame60 && now - pk->frame60 <= RD_KO_MAXAGE60) {
				if (pk->dir.x * pk->dir.x + pk->dir.y * pk->dir.y + pk->dir.z * pk->dir.z > 0.25f) {
					dir = &pk->dir;
				}

				gset = &pk->gset;
				hitpart = pk->hitpart;
			}

			break;
		}
	}

	g_RdCreatingKnockout = true;
	ragdollOnChrDeath(chr, dir, hitpart, gset, false);
	g_RdCreatingKnockout = false;

	rd = ragdollFindByChr(chr);

	if (rd == NULL) {
		sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d knocked out: no ragdoll (seated, cutscene, ...), falls with the animation", chr->chrnum);
		return false;
	}

	// Out cold: pure physics from the first frame, no muscle tone, no
	// clutching a wound
	rd->knockedout = true;
	rd->blending = false;
	rd->powered = false;
	rd->clutcharm = -1;
	rd->clutchpart = -1;

	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d knocked out (weapon %d%s): ragdolled, alive",
			chr->chrnum, gset ? gset->weaponnum : -1, dir ? ", pushed" : "");

	return true;
}

/**
 * world.c: a knocked-out body hit something hard enough to leave a blood
 * splat (RD_IMPACT_SPLATSPEED): that kills him.
 */
void ragdollKillKnockedOut(struct ragdoll *rd, f32 impact)
{
	struct chrdata *chr = rd->chr;
	struct gset gset;
	struct coord dir = {0, -1, 0};

	if (!rd->knockedout || chr == NULL || rd->isplayer
			|| (chr->actiontype != ACT_DRUGGEDDROP && chr->actiontype != ACT_DRUGGEDKO)) {
		return;
	}

	memset(&gset, 0, sizeof(gset));
	gset.weaponnum = WEAPON_NONE;

	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d (knocked out) hit something at %.0f u/s: killed", chr->chrnum, impact);

	chr->damage = chr->maxdamage;
	chrBeginDeath(chr, &dir, 0, HITPART_GENERAL, &gset, false, -1);
}

/**
 * prop.c handInflictMeleeDamage, before the punch's damage: Deadly Punch
 * punches knock bodies about (corpses and knocked-out guards), like shots.
 */
void ragdollOnChrPunched(struct chrdata *chr, s32 hitpart, struct coord *dir, struct gset *gset)
{
	if (g_RagdollsEnabled && cheatIsDeadlyPunch(gset)) {
		ragdollOnChrShot(chr, hitpart, dir, gset); // (nothing unless chr is a ragdoll)
	}
}

/**
 * The body has moved: find the floor under it the way the game does for
 * living chrs, so the shadow and the lighting follow the body.
 */
static void ragdollUpdateFloor(struct ragdoll *rd)
{
	struct chrdata *chr = rd->chr;
	u16 floorcol = chr->floorcol;
	u8 floortype = chr->floortype;
	u16 floorflags = 0;
	RoomNum floorroom = chr->floorroom;
	s32 inlift = 0;
	struct prop *lift = NULL;
	f32 ground;

	ground = cdFindGroundInfoAtCyl(&rd->prop->pos, 20.0f, rd->prop->rooms, &floorcol, &floortype, &floorflags, &floorroom, &inlift, &lift);

	if (ground > -100000.0f && ground <= rd->prop->pos.y + 10.0f) {
		chr->ground = ground;
		chr->floorcol = floorcol;
		chr->floortype = floortype;
		chr->floorroom = floorroom;
	}

	propCalculateShadeColour(rd->prop, chr->nextcol, chr->floorcol);
}

/**
 * Called from explosionInflictDamage() for every chr the blast reaches, on
 * the explosion's first frame (after the damage, so chrs it kills are
 * already ragdolls). Throws the body away from the blast centre.
 */
void ragdollOnExplosion(struct chrdata *chr, struct coord *centre, f32 radius, f32 damage)
{
	RD_CRASHCTX("onexplosion", RD_CHRNUM(chr), -1);
	struct ragdoll *rd = ragdollFindByChr(chr);
	f32 strength;
	s32 part;

	if (rd == NULL || radius <= 0.0f || !pdphysIsReady()) {
		return;
	}

	// The blast pushes it, so the fallback push from chrYeetFromPos isn't needed
	rd->pendingyeet = false;
	rd->blending = false; // blasts are pure physics
	rd->lastexplosion60 = g_Vars.lvframe60;

	if (!ragdollUnfreeze(rd)) {
		return;
	}

	ragdollTryDismemberBlast(rd, centre, radius);

	// Grenade/rocket damage is around 1; clamp so big blasts don't launch bodies into orbit
	strength = damage;

	RD_CLAMPTO(strength, 0.5f, 2.0f);

	strength *= rd->pushscale;

	for (part = 0; part < rd->numparts; part++) {
		s32 body = rd->bodyindex[part];
		f32 dir[3];
		f32 dist;
		f32 falloff;
		f32 speed;
		f32 vel[3];

		if (body < 0) {
			continue;
		}

		dir[0] = rd->world[part].m[3][0] - centre->x;
		dir[1] = rd->world[part].m[3][1] - centre->y;
		dir[2] = rd->world[part].m[3][2] - centre->z;
		dist = rdVecLen(dir);

		if (dist < 1.0f) {
			rdVecSet(dir, 0.0f, 1.0f, 0.0f);
			dist = 1.0f;
		} else {
			rdVecDiv(dir, dist);
		}

		falloff = 1.0f - dist / (radius * 1.5f);

		RD_CLAMPLO(falloff, 0.15f);

		speed = 700.0f * strength * falloff;

		RD_CLAMPHI(speed, 1100.0f);

		// Blasts throw bodies up as well as away
		vel[0] = dir[0] * speed;
		vel[1] = dir[1] * speed * 0.5f + speed * 0.45f;
		vel[2] = dir[2] * speed;

		pdphysRagdollAddVelocity(rd->handle, body, vel);
	}

	rd->numthuds = 0;
}

/**
 * chrTickDie(): a scripted death animation (Villa's rooftop snipers falling
 * back or rolling off, Chicago's fall over the railing) has played to its
 * end. Instead of freezing in its last frame the body becomes a ragdoll
 * there, with no push. Returns true if it did. Guards slumped in their
 * chairs keep the game's animation.
 */
s32 ragdollOnSpecialDieEnd(struct chrdata *chr)
{
	RD_CRASHCTX("onspecialdieend", RD_CHRNUM(chr), -1);

	s32 animnum;

	if (chr == NULL || chr->prop == NULL || chr->model == NULL || ragdollFindByChr(chr)) {
		return false;
	}

	// Told by the animation, not chr->specialdie: the level's script can
	// clear that while the chr is still falling (Chicago's stairs guard has
	// it cleared once he's away from his pad, i.e. over the railing)
	animnum = modelGetAnimNum(chr->model);

	if (animnum != ANIM_SPECIALDIE_FALLBACK && animnum != ANIM_SPECIALDIE_ROLL1
			&& animnum != ANIM_SPECIALDIE_ROLL2 && animnum != ANIM_SPECIALDIE_ROLL3
			&& animnum != ANIM_SPECIALDIE_OVERRAILING) {
		return false; // (not the sitting deaths: bodies wedged in the chair got launched out of it)
	}

	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d's scripted death (anim %d) ended at %.0f %.0f %.0f",
			chr->chrnum, animnum, chr->prop->pos.x, chr->prop->pos.y, chr->prop->pos.z);

	// Only where it landed on a floor: an animation that ends over nothing
	// (or below a floor) keeps the game's last frame instead of falling away
	{
		f32 from[3] = { chr->prop->pos.x, chr->prop->pos.y + 100.0f, chr->prop->pos.z };
		f32 to[3] = { chr->prop->pos.x, chr->prop->pos.y - 1500.0f, chr->prop->pos.z };
		f32 hit[3], normal[3];
		s32 hitroom;

		if (!pdphysRaycastAnyRoom(from, to, hit, normal, &hitroom) || normal[1] < 0.6f || hit[1] > chr->prop->pos.y + 40.0f) {
			sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d's scripted death ended with no floor under it: keeps its last frame", chr->chrnum);
			return false;
		}
	}

	g_RagdollSpecialDieEnd = true;
	ragdollOnChrDeath(chr, NULL, HITPART_GENERAL, NULL, false);
	g_RagdollSpecialDieEnd = false;

	if (ragdollFindByChr(chr) == NULL) {
		sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d couldn't become a ragdoll (action %d, ragdolls %d, level %d): keeps its last frame",
				chr->chrnum, chr->actiontype, g_RagdollsEnabled, g_RagdollLevelLoaded);
		return false;
	}

	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d's scripted death: now a ragdoll", chr->chrnum);

	return true;
}

/**
 * chrYeetFromPos(): an explosion killed this chr. The game would play a
 * scripted "thrown by the blast" animation; humans become ragdolls instead
 * and the blast itself (ragdollOnExplosion) throws them. If the blast doesn't
 * push the body within a frame (e.g. it died on a later frame of the
 * explosion), a fallback push from the explosion's position is applied.
 */
void ragdollOnChrYeet(struct chrdata *chr, struct coord *exppos)
{
	RD_CRASHCTX("onchryeet", RD_CHRNUM(chr), -1);
	struct ragdoll *rd;

	ragdollOnChrDeath(chr, NULL, HITPART_GENERAL, NULL, false);

	rd = ragdollFindByChr(chr);

	if (rd && rd->state == RDSTATE_SIMULATING && rd->age60 == 0 && exppos) {
		rd->pendingyeet = true;
		rd->yeetpos = *exppos;
	}
}

/**
 * MOD: ragdoll physics - hoverbike runs over. bbikeCalculateNewPositionWithPush()
 * ran into a chr. With a real run-up (recentdist: how far the bike actually
 * moved in the last half second; its speed alone can build up while it's
 * held against someone) and heading at the chr, the chr is killed and
 * knocked down ahead of the bike as a ragdoll. A friend of the player, or a
 * slow bump, is left to the game (it blocks the bike as before). Returns
 * true if the chr was run over (the bike may carry on).
 */
// Half a second near top speed (about 18.5 per 60th: 555 units)
#define RD_BIKE_RUNOVER_DIST 450.0f
// And heading at the chr now (per 60th)
#define RD_BIKE_RUNOVER_CLOSING 10.0f

s32 ragdollOnBikeHitChr(struct chrdata *chr, struct prop *bike, f32 speedx, f32 speedz, f32 recentdist)
{
	RD_CRASHCTX("onbikehitchr", RD_CHRNUM(chr), -1);
	struct prop *playerprop = g_Vars.currentplayer ? g_Vars.currentplayer->prop : NULL;
	struct ragdoll *rd;
	struct coord dir;
	f32 dx, dz, dist, closing, speed, damage;
	f32 vel[3];

	if (!g_RagdollsEnabled || !g_RagdollLevelLoaded || !pdphysIsReady()
			|| chr == NULL || chr->prop == NULL || bike == NULL || playerprop == NULL) {
		return false;
	}

	if (chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD || ragdollFindByChr(chr) != NULL) {
		return false;
	}

	if (playerprop->chr && chrCompareTeams(playerprop->chr, chr, COMPARE_FRIENDS)) {
		return false;
	}

	dx = chr->prop->pos.x - bike->pos.x;
	dz = chr->prop->pos.z - bike->pos.z;
	dist = sqrtf(dx * dx + dz * dz);

	if (dist < 1.0f) {
		dist = 1.0f;
	}

	closing = (speedx * dx + speedz * dz) / dist;
	speed = sqrtf(speedx * speedx + speedz * speedz);

	if (recentdist < RD_BIKE_RUNOVER_DIST || closing < RD_BIKE_RUNOVER_CLOSING || speed < 0.01f) {
		return false;
	}

	dir.x = speedx / speed;
	dir.y = 0.0f;
	dir.z = speedz / speed;

	// Enough to kill through any shield or armour
	damage = chr->maxdamage - chr->damage + chr->cshield + 1.0f;

	if (damage < 1.0f) {
		damage = 1.0f;
	}

	chrDamageByMisc(chr, damage, &dir, NULL, playerprop);

	if (chr->actiontype != ACT_DIE && chr->actiontype != ACT_DEAD) {
		return false; // (invincible)
	}

	rd = ragdollFindByChr(chr);

	if (rd && rd->state == RDSTATE_SIMULATING && ragdollUnfreeze(rd)) {
		// Half the bike's speed: knocked down a few steps ahead, not sent
		// flying
		f32 hspeed = speed * 60.0f * 0.5f;

		RD_CLAMPHI(hspeed, 600.0f);

		// Thrown ahead of the bike and up off its feet, all at once (no
		// death animation blend: it's knocked flat)
		rd->blending = false;
		rd->stumble = false;
		rdVecSet(vel, dir.x * hspeed, 100.0f + hspeed * 0.2f, dir.z * hspeed);
		pdphysRagdollAddVelocity(rd->handle, -1, vel);
	}

	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d run over by the hoverbike at %.1f (speed %.1f, moved %.0f in the last half second)%s",
			chr->chrnum, closing, speed, recentdist, rd ? "" : ", no ragdoll");

	return true;
}

s32 ragdollWakeFrozen(struct ragdoll *rd)
{
	RD_CRASHCTX("wakefrozen", RD_RDCHRNUM(rd), -1);
	return !rd->fellout && ragdollUnfreeze(rd) && rd->state == RDSTATE_SIMULATING;
}

static bool ragdollIsFar(struct ragdoll *rd)
{
	return g_Vars.currentplayer && rdDistSqCoord(rd->world[0].m[3], &g_Vars.currentplayer->cam_pos) > RD_SQ(2000.0f);
}

/**
 * Where a body came to rest, for the log: sunk into the floor (a floor-like
 * surface just above the pelvis), fallen off a ledge, or settled normally.
 */
static void ragdollLogSettled(struct ragdoll *rd)
{
	f32 from[3];
	f32 to[3];
	f32 hit[3];
	f32 normal[3];
	s32 hitroom = -1;
	const char *timeout = rd->age60 >= RD_SETTLETIME60 ? " (timeout)" : "";
	bool found = false;
	s32 tries;

	rdCoordToVec(&rd->prop->pos, from);

#ifdef PD_GRAVITY
	// MOD: gravity - inverted, it rests against a ceiling: no floor checks
	if (gravityScale() < 0.0f) {
		sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d settled after %.1f s at (%.0f, %.0f, %.0f) with inverted gravity%s",
				RD_RDCHRNUM(rd), rd->age60 / 60.0f, from[0], from[1], from[2], timeout);
		return;
	}
#endif

	rdVecSet(to, from[0], from[1] + 25.0f, from[2]);

	if (pdphysRaycast(from, to, hit, normal, &hitroom) && fabsf(normal[1]) > 0.7f) {
		sysLogPrintf(LOG_WARNING, "RAGDOLL: chr %d SANK: floor %.1f (room %d) is %.1f above the pelvis at (%.0f, %.0f, %.0f), death ground %.1f, settled after %.1f s%s",
				rd->chr->chrnum, hit[1], hitroom, hit[1] - rd->prop->pos.y, rd->prop->pos.x, rd->prop->pos.y, rd->prop->pos.z,
				rd->deathground, rd->age60 / 60.0f, timeout);
		return;
	}

	// Look down for the first floor-like surface (skipping walls the body may be lying against)
	for (tries = 0; tries < 4 && !found; tries++) {
		to[1] = rd->prop->pos.y - 400.0f;

		if (!pdphysRaycast(from, to, hit, normal, &hitroom)) {
			break;
		}

		if (normal[1] > 0.7f) {
			found = true;
		} else {
			// Hit a wall: step the ray slightly away from it
			from[0] += normal[0] * 3.0f;
			from[2] += normal[2] * 3.0f;
			to[0] = from[0];
			to[2] = from[2];
		}
	}

	if (found && hit[1] < rd->deathground - 100.0f) {
		// PD stops players at ledges in its movement code, not with walls, so a body can be pushed off one
		sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d FELL off a ledge: settled %.0f below where it died, at (%.0f, %.0f, %.0f) room %d, after %.1f s%s",
				rd->chr->chrnum, rd->deathground - hit[1], rd->prop->pos.x, rd->prop->pos.y, rd->prop->pos.z,
				hitroom, rd->age60 / 60.0f, timeout);
	} else if (found) {
		sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d settled after %.1f s at (%.0f, %.0f, %.0f), pelvis %.1f above floor (room %d), death ground %.1f%s",
				rd->chr->chrnum, rd->age60 / 60.0f, rd->prop->pos.x, rd->prop->pos.y, rd->prop->pos.z,
				rd->prop->pos.y - hit[1], hitroom, rd->deathground, timeout);
	} else {
		sysLogPrintf(LOG_WARNING, "RAGDOLL: chr %d SANK: no floor below the pelvis at (%.0f, %.0f, %.0f), death ground %.1f, settled after %.1f s%s",
				rd->chr->chrnum, rd->prop->pos.x, rd->prop->pos.y, rd->prop->pos.z,
				rd->deathground, rd->age60 / 60.0f, timeout);
	}
}

static void ragdollTickAll(void)
{
	RD_CRASHCTX("tick", -1, -1);
	f32 joints[PDPHYS_MAX_RAGDOLL_PARTS][16];
	f32 dt = g_Vars.lvupdate60freal / 60.0f;
	s32 i;

	if (g_RagdollNumUsed == 0) {
		if (g_RagdollNumProps > 0) {
			ragdollPropsReleaseAll();
		}

		return;
	}

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];

		if (rd->state == RDSTATE_FREE) {
			continue;
		}

		// Drop ragdolls whose chr has gone away or been reused
		if (rd->chr->model != rd->model
				|| rd->chr->prop != rd->prop
				|| rd->prop == NULL
				|| (rd->chr->actiontype != ACT_DIE && rd->chr->actiontype != ACT_DEAD && !rd->isplayer
					&& !(rd->knockedout && (rd->chr->actiontype == ACT_DRUGGEDDROP || rd->chr->actiontype == ACT_DRUGGEDKO)))) {
			ragdollRelease(rd);
			continue;
		}

		// A corpse that fell out of the level goes away
		if (rd->fellout && !rd->isplayer && rd->chr->actiontype == ACT_DEAD
				&& !rd->chr->act_dead.fadenow && rd->chr->act_dead.fadetimer60 < 0) {
			rd->chr->act_dead.fadenow = true;
		}

		if (g_Vars.currentplayer) {
			// Bodies close to the camera are ticked (and so tested for being in
			// view) every frame. A chr whose rooms aren't drawn is otherwise only
			// ticked every few frames, and a body by a doorway popped in late.
			if (rdDistSqCoord(rd->world[0].m[3], &g_Vars.currentplayer->cam_pos) < RD_SQ(1200.0f)) {
				rd->prop->forceonetick = true;
			}

			// Drawn after its room's level geometry, not before: some floor
			// sections (by doorways) are drawn without a depth test, over
			// whatever the room drew before them, and a body lying on one
			// vanished under it. A standing guard only loses his feet.
			rd->prop->flags |= PROPFLAG_RENDERPOSTBG;
		}
	}

	ragdollScreamTick(); // a voice line taking over a scream's MP3
	rdWavTick();         // WAV sounds following their bodies

	if (dt <= 0.0f) {
		return;
	}

	ragdollWakeOnMovedProps();
	ragdollSyncProps(dt);

	// Muscles, and blasts that killed without pushing
	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];

		if (rd->state != RDSTATE_SIMULATING) {
			continue;
		}

		if (rd->pendingyeet && rd->age60 > 0) {
			ragdollOnExplosion(rd->chr, &rd->yeetpos, 300.0f, 1.0f);
			rd->pendingyeet = false;
		}

		// A body at rest (asleep in the physics world) needs nothing until
		// something wakes it: a shot, a blast, a door, another body
		rd->sleeping = rd->settled && !rd->grabbed && pdphysRagdollIsAsleep(rd->handle);

		if (rd->sleeping) {
			if (rd->firstsleep60 == 0) {
				rd->firstsleep60 = RD_MAX(rd->age60, 1);
			}

			continue;
		}

		ragdollDriveJoints(rd, dt);
		ragdollBleed(rd);

		// Water: every few ticks, is there a water surface over the pelvis
		// (or a little under it, for a body bobbing up)?
		if (((g_Vars.lvframe60 / 4) + i) % 3 == 0) {
			ragdollCheckWater(rd);
		}

		// The floor it's on decides how far it slides
		if (((g_Vars.lvframe60 / 4) + i) % 3 == 1) {
			ragdollUpdateFriction(rd);
		}

		// The rooms it collides with: every tick close to the player or
		// while it's moving fast (a long fall down a shaft goes through a
		// room's floor in a tick or two), every few ticks far away and slow
		// (staggered between bodies)
		{
			f32 d[3];

			rdVecSub(rd->world[0].m[3], rd->colroompos, d);

			if (!ragdollIsFar(rd) || rdVecLen(d) > RD_COLROOM_MOVE || ((g_Vars.lvframe60 / 2) + i) % 4 == 0) {
				ragdollUpdateCollisionRooms(rd);
				rdVecCopy(rd->world[0].m[3], rd->colroompos);
			}
		}

		// Pieces move: keep the corpse in the rooms its pieces are in (every
		// other tick while one is carried: it can cross a room in a few)
		if (rd->severed && (g_Vars.lvframe60 - rd->piecerooms60 >= (rd->grabbed ? 2 : 8)
					|| rd->piecerooms60 > g_Vars.lvframe60)) {
			rd->piecerooms60 = g_Vars.lvframe60;
			ragdollUpdatePieceRooms(rd);
		}
	}

	{
		RD_CRASHCTX("physicsstep", -1, -1);
#ifdef PD_GRAVITY
		ragdollFollowGravity(); // MOD: gravity
#endif
		pdphysStep(dt);
	}

	ragdollHandlePropImpacts();

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *rd = &g_Ragdolls[i];
		s32 n;
		s32 part;
		f32 old[3];

		if (rd->state != RDSTATE_SIMULATING) {
			continue;
		}

		rd->age60 += g_Vars.lvupdate60;
		ragdollCheckSeen(rd);

		// MOD: combat simulator ragdolls - a solid body doesn't move (on the
		// tick it turns solid, its pose is read once more below)
		if (rd->solid) {
			continue;
		}

		rdMpSolidCheck(rd);

		// Still asleep after the step: the pose hasn't changed
		if (rd->sleeping && pdphysRagdollIsAsleep(rd->handle)) {
			continue;
		}

		rd->sleeping = false;
		n = pdphysRagdollGetJoints(rd->handle, joints);

		for (part = 0; part < rd->numparts; part++) {
			s32 b = rd->bodyindex[part];

			if (b >= 0 && b < n) {
				memcpy(&rd->world[part], joints[b], sizeof(Mtxf));
			}
		}

#ifdef PD_GRAVITY
		// MOD: gravity - inverted, a body can fly out of the top of the level
		if (gravityScale() < 0.0f && rd->world[0].m[3][1] > ragdollLevelTopY() + RD_FLYOUT_MARGIN) {
			sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d%s flew out of the top of the level (pelvis y %.0f), stopping it%s",
					RD_RDCHRNUM(rd), rd->isplayer ? " (player)" : "", rd->world[0].m[3][1],
					rd->isplayer ? "" : " and removing the corpse");
			rd->fellout = true;
			ragdollFreeze(rd);
			continue;
		}
#endif

		// Fell out of the level: stop it for good (the corpse fades out once
		// the game has made it ACT_DEAD, see the release loop)
		if (rd->world[0].m[3][1] < RD_KILLFLOOR_Y) {
			sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d%s fell out of the level (pelvis y %.0f), stopping it%s",
					RD_RDCHRNUM(rd), rd->isplayer ? " (player)" : "", rd->world[0].m[3][1],
					rd->isplayer ? "" : " and removing the corpse");
			rd->fellout = true;
			ragdollFreeze(rd);
			continue;
		}

		ragdollPropagateRigidParts(rd);
		ragdollUpdateRenderPose(rd);
		ragdollCheckThud(rd);

		// When it first comes to rest (asleep, the death animation over): the
		// fall scream allows a push for a short while after (world.c)
		if (rd->firstsleep60 == 0 && !rd->blending && !rd->grabbed && pdphysRagdollIsAsleep(rd->handle)) {
			rd->firstsleep60 = RD_MAX(rd->age60, 1);
		}

		ragdollCheckScream(rd, dt);
		ragdollLedgeCatch(rd);

		// Move the prop with the pelvis so culling and room membership follow
		// (not the player's: their movement code owns it)
		if (rd->isplayer) {
			ragdollUpdateBodyRooms(rd);
		} else {
			rdCoordToVec(&rd->prop->pos, old);
			rdVecToCoord(rd->world[0].m[3], &rd->prop->pos);

			if (fabsf(rd->prop->pos.x - old[0]) + fabsf(rd->prop->pos.y - old[1]) + fabsf(rd->prop->pos.z - old[2]) > 0.5f) {
				ragdollUpdateBodyRooms(rd); // (not chr0f0220ac: that never drops old rooms)
				ragdollUpdateFloor(rd);
			}
		}

		// Settled: it stays in the physics world (asleep), others can land on
		// it and shots move it again. (The player's body skips the diagnostics:
		// they use the prop, which the player's code owns.)
		if (!rd->settled && !rd->blending
				&& (pdphysRagdollIsAsleep(rd->handle) || rd->age60 - (rd->isplayer ? 0 : rd->blendend60) >= RD_SETTLETIME60)) {
			if (!rd->isplayer) {
				ragdollLogSettled(rd);
			}

			rd->settled = true;
		}
	}

	ragdollEnforcePhysicalLimit(0);
}

/* ---- hooks used by the game code ---- */

s32 ragdollGetExtraChrSlots(void)
{
	return g_RagdollsEnabled ? RD_EXTRACHRSLOTS : 0;
}

bool ragdollIsChrRagdolled(struct chrdata *chr)
{
	return ragdollFindByChr(chr) != NULL;
}

bool ragdollIsChrSettled(struct chrdata *chr)
{
	struct ragdoll *rd = ragdollFindByChr(chr);

	// MOD: combat simulator ragdolls - nor does a simulant's respawn wait for its body
	return rd && (rd->settled || rd->state == RDSTATE_FROZEN || (chr->aibot && rd->age60 >= RD_MPBOTDEAD60));
}

/**
 * MOD: combat simulator ragdolls - chrDie: a simulant dies. Simulants skip
 * chrBeginDeath (which makes the guards' ragdolls) and get no death
 * animation: start one of the game's for the blend, as the player's body
 * does, and make the ragdoll.
 */
void ragdollOnBotDeath(struct chrdata *chr, struct coord *dir, s32 hitpart, struct gset *gset)
{
	RD_CRASHCTX("botdeath", RD_CHRNUM(chr), hitpart);
	s32 num = 0;

	if (!g_RagdollsEnabled || !g_Vars.normmplayerisrunning || chr == NULL || chr->aibot == NULL
			|| chr->actiontype != ACT_DIE || chr->model == NULL || chr->model->anim == NULL
			|| ragdollSkelFor(chr->model) == NULL || ragdollFindByChr(chr) != NULL) {
		return;
	}

	while (g_DeathAnimations[num] != 0) {
		num++;
	}

	if (num > 0) {
		s32 pick = (s32)rdRollRange(0.0f, (f32)num - 0.01f);

		modelSetAnimation(chr->model, g_DeathAnimations[pick], rdRollRange(0.0f, 1.0f) < 0.5f, 0, 0.5f, 0);
	}

	ragdollOnChrDeath(chr, dir, hitpart, gset, false);

	if (ragdollFindByChr(chr)) {
		sysLogPrintf(LOG_NOTE, "RAGDOLL: combat simulator: simulant chr %d died, its body is a ragdoll", chr->chrnum);
	}
}

// MOD: combat simulator ragdolls - chrTickDead: a simulant's body doesn't
// fade during its respawn countdown (it stays)
bool ragdollMpKeepsBody(struct chrdata *chr)
{
	return g_Vars.normmplayerisrunning && g_RagdollMpMaxBodies >= 0 && chr->aibot && ragdollFindByChr(chr) != NULL;
}

/**
 * MOD: combat simulator ragdolls - a player or simulant respawns into its old
 * chr and model. The body stays where it fell: the model (pose, cuts and
 * ragdoll) moves to a new corpse chr, and the combatant gets a fresh model of
 * the same body and head. False if it couldn't (no chr slot or memory).
 */
static bool rdHandOffBody(struct ragdoll *rd, struct player *player)
{
	struct chrdata *chr = rd->chr;
	struct model *old = rd->model;
	struct modeldef *headdef = NULL;
	struct model *fresh;
	struct prop *corpseprop;
	struct chrdata *corpse;
	struct coord pos;
	RoomNum rooms[8];

	if (old == NULL || chr->model != old || chr->prop == NULL || chrsGetNumFree() < 2) {
		return false;
	}

	// The head it wears (the Combat Simulator loads one per player): the
	// fresh model shares it
	if (old->definition->skel == &g_SkelChr) {
		struct modelnode *node = modelGetPart(old->definition, MODELPART_CHR_HEADSPOT);

		if (node) {
			union modelrwdata *rw = modelGetNodeRwData(old, node);
			headdef = rw ? rw->headspot.headmodeldef : NULL;
		}
	}

	if (chr->headnum > 0 && headdef == NULL && !g_HeadsAndBodies[chr->bodynum].unk00_01) {
		return false;
	}

	fresh = body0f02ce8c(chr->bodynum, chr->headnum, old->definition, headdef, false, NULL, player != NULL, false);

	if (fresh == NULL) {
		return false;
	}

	modelSetScale(fresh, old->scale);

	// The animation and root state carry over as on a reused model (an empty
	// animation state hung the game when the simulant next ticked)
	if (old->anim && fresh->anim) {
		*fresh->anim = *old->anim;
	}

	if (old->definition->rootnode && (old->definition->rootnode->type & 0xff) == MODELNODETYPE_CHRINFO) {
		union modelrwdata *from = modelGetNodeRwData(old, old->definition->rootnode);
		union modelrwdata *to = modelGetNodeRwData(fresh, fresh->definition->rootnode);

		if (from && to) {
			to->chrinfo = from->chrinfo;
		}
	}

	// The corpse: a plain dead chr with the old model
	rdVecToCoord(rd->world[0].m[3], &pos);
	roomsCopy(rd->isplayer ? rd->playerbaserooms : chr->prop->rooms, rooms);

	// chrAllocate resets the model's root, turn and animation speed. A body
	// still following its death animation would lurch towards the moved
	// animation: keep both as they were.
	{
		struct anim savedanim;
		struct modelrwdata_chrinfo savedroot;
		union modelrwdata *root = NULL;

		if (old->anim) {
			savedanim = *old->anim;
		}

		if (old->definition->rootnode && (old->definition->rootnode->type & 0xff) == MODELNODETYPE_CHRINFO) {
			root = modelGetNodeRwData(old, old->definition->rootnode);
		}

		if (root) {
			savedroot = root->chrinfo;
		}

		corpseprop = chrAllocate(old, &pos, rooms, 0, NULL);

		if (old->anim) {
			*old->anim = savedanim;
		}

		if (root) {
			root->chrinfo = savedroot;
		}
	}

	if (corpseprop == NULL || corpseprop->chr == NULL) {
		old->chr = chr;
		modelmgrFreeModel(fresh);
		return false;
	}

	corpse = corpseprop->chr;
	propActivateThisFrame(corpseprop);
	propEnable(corpseprop);

	corpse->headnum = chr->headnum;
	corpse->bodynum = chr->bodynum;
	corpse->race = chr->race;
	corpse->flags = 0;
	corpse->flags2 = 0;
	corpse->hidden2 |= CHRH2FLAG_SPAWNED;
	corpse->noblood = chr->noblood;
	corpse->team = chr->team; // the team highlight stays as it was

	// And its lighting (a new chr starts lit white: it flashed)
	memcpy(corpse->shadecol, chr->shadecol, sizeof(corpse->shadecol));
	memcpy(corpse->nextcol, chr->nextcol, sizeof(corpse->nextcol));
	corpse->floorcol = chr->floorcol;
	corpse->floortype = chr->floortype;
	corpse->floorroom = chr->floorroom;
	corpse->ailist = NULL;
	chrBeginDead(corpse);

	corpseprop->pos = pos;
	corpseprop->flags |= PROPFLAG_RENDERPOSTBG;

	// The player or simulant: the fresh model, set up as the chr code does
	modelSetAnim70(fresh, chr0f01f378);
	fresh->chr = chr;
	fresh->unk01 = 1;
	modelSetAnimPlaySpeed(fresh, PALUPF(var80062968), 0);
	modelSetRootPosition(fresh, &chr->prop->pos);
	chr->model = fresh;
	chr->prop->flags &= ~PROPFLAG_RENDERPOSTBG;

	// No matrices until it's drawn: not on screen until then (the shot and aim
	// checks read them)
	fresh->matrices = NULL;
	chr->prop->flags &= ~PROPFLAG_ONTHISSCREENTHISTICK;

	if (player) {
		player->model00d4 = fresh;
	}

	// The ragdoll goes with the body
	rd->chr = corpse;
	rd->prop = corpseprop;
	rd->isplayer = false;
	rd->playernum = -1;
	ragdollUpdateBodyRooms(rd);
	ragdollUpdateFloor(rd);
	g_RagdollChangeSerial++;

	// Already lying still: solid at once (asleep counts, whatever reactions
	// or the death camera held: letting go of those now moved the torso)
	if (ragdollSolidEnabled() && rd->state == RDSTATE_SIMULATING && rd->handle >= 0 && !rd->grabbed && !rd->riding
			&& !rd->fellout && ((rdMpAtRest(rd) && rd->restspeed <= RD_SOLID_MAXSPEED) || pdphysRagdollIsAsleep(rd->handle))) {
		rdSetSolid(rd, "was lying still at its respawn");
	}

	sysLogPrintf(LOG_NOTE, "RAGDOLL: combat simulator: chr %d respawns, its body stays (now chr %d)", chr->chrnum, corpse->chrnum);
	return true;
}

// MOD: combat simulator ragdolls - playerStartNewLife / botSpawn: the body
// stays (true) or goes (Max Bodies on Vanilla)
bool ragdollOnCombatantRespawn(struct chrdata *chr, struct player *player)
{
	RD_CRASHCTX("respawn", RD_CHRNUM(chr), -1);
	struct ragdoll *rd = ragdollFindByChr(chr);

	if (rd == NULL) {
		return false;
	}

	if (g_Vars.normmplayerisrunning && g_RagdollMpMaxBodies >= 0 && rdHandOffBody(rd, player)) {
		return true;
	}

	ragdollRelease(rd);
	return false;
}

void ragdollOnChrRemoved(struct chrdata *chr)
{
	RD_CRASHCTX("onchrremoved", RD_CHRNUM(chr), -1);
	struct ragdoll *rd = ragdollFindByChr(chr);
	s32 i, h;

	if (rd) {
		ragdollRelease(rd);
	}

	// The player's body is going (co-op respawn): its dropped gun too
	for (i = 0; i < MAX_PLAYERS; i++) {
		struct rdplayergun *pg = &g_RdPlayerGuns[i];

		if (pg->chr != chr) {
			continue;
		}

		for (h = 0; h < 2; h++) {
			struct prop *gun = pg->props[h];

			if (gun && gun->type == PROPTYPE_WEAPON && gun->weapon == pg->objs[h] && gun->parent == NULL) {
				objFreePermanently(&gun->weapon->base, true);
			}
		}

		memset(pg, 0, sizeof(*pg));
	}
}

bool ragdollModelHasPose(struct model *model)
{
	return ragdollFindByModel(model) != NULL;
}

/**
 * Called from modelUpdateChrNodeMtx/modelUpdatePositionNodeMtx. If the model
 * is a ragdoll, writes the node's matrix from the ragdoll pose and returns
 * true; otherwise returns false and the animation code runs as normal.
 */
bool ragdollUpdateNodeMtx(struct modelrenderdata *renderdata, struct model *model, struct modelnode *node)
{
	RD_CRASHCTX("updatenodemtx", -1, -1);
	struct ragdoll *rd;
	u32 type;

	if (g_RagdollSamplingAnim) {
		return false;
	}

	rd = ragdollFindByModel(model);

	if (rd == NULL) {
		return false;
	}

	type = node->type & 0xff;

	if (type == MODELNODETYPE_CHRINFO) {
		Mtxf *mtx = &model->matrices[node->rodata->chrinfo.mtxindex];
		Mtxf *parentmtx = node->parent ? modelFindNodeMtx(model, node->parent, 0) : renderdata->unk00;

		if (parentmtx) {
			mtx00015be4(parentmtx, &rd->rootrender, mtx);
		} else {
			mtx4Copy(&rd->rootrender, mtx);
		}

		return true;
	}

	if (type == MODELNODETYPE_POSITION) {
		s32 part = rdPartOfNode(rd->skel, node->rodata->position.part);
		struct coord one = {1.0f, 1.0f, 1.0f};

		if (part <= 0) {
			return false; // not a part (fingers, toes, the Skedar's tail tip): the animation's, from its part
		}

		// The DK Mode cheat's big head and arms: the chr renderer's joint hook
		// (chrHandleJointPositioned) scales them, but it's left out below, so
		// bodies went back to normal when they died. Same joints, same scale
		// (the body's joints and shapes match it: rdDkSpread).
		if (rd->dk) {
			s32 joint = node->rodata->position.mtxindex0;
			f32 dk = joint == 0 ? RD_DK_HEADSCALE : (joint == 2 || joint == 3) ? RD_DK_ARMSCALE : 1.0f;

			one.x = one.y = one.z = dk;
		}

		// The physics pose as it is, without the chr renderer's aiming hook
		// (it twisted the waist, shoulders and neck by the chr's aim)
		{
			void (*jointfunc)(s32 mtxindex, Mtxf *mtx) = g_ModelJointPositionedFunc;

			g_ModelJointPositionedFunc = NULL;
			modelPositionJointUsingQuatRot(renderdata, model, node, rd->localquat[part], &rd->renderpos[part], &one);
			g_ModelJointPositionedFunc = jointfunc;
		}

		if (part == 2 && ragdollHideHeadForViewer(rd)) {
			Mtxf *m = &model->matrices[node->rodata->position.mtxindex0];
			s32 c, r;

			for (c = 0; c < 3; c++) {
				for (r = 0; r < 3; r++) {
					m->m[c][r] *= 0.001f;
				}
			}
		}

		return true;
	}

	return false;
}

#endif // PD_PHYSICS

void ragdollSysInit(void)
{
#ifdef PD_PHYSICS
	// Physics is always initialised, so ragdolls can be switched on from the
	// Extended > Physics menu without restarting
	if (!g_RagdollsEnabled) {
		sysLogPrintf(LOG_NOTE, "PHYSICS: ragdolls currently off (Ragdoll.Ragdolls=0), can be enabled in Extended > Physics");
	}

	sysSetCrashInfoCallback(ragdollCrashInfo);

	if (!pdphysInit(ragdollLog)) {
		sysLogPrintf(LOG_ERROR, "PHYSICS: failed to initialise Jolt, ragdolls unavailable");
		return;
	}

	{
		f32 resty = 0.0f;
		s32 steps = 0;
		f32 ms = 0.0f;

		if (pdphysSelfTest(&resty, &steps, &ms)) {
			sysLogPrintf(LOG_NOTE, "PHYSICS: self-test passed (sphere at rest at y=%.2f after %d steps, %.2f ms)", resty, steps, ms);
		} else {
			sysLogPrintf(LOG_WARNING, "PHYSICS: self-test FAILED (sphere at y=%.2f after %d steps, %.2f ms)", resty, steps, ms);
		}
	}
#else
	sysLogPrintf(LOG_NOTE, "PHYSICS: built without physics support (PD_PHYSICS=OFF)");
#endif
}

void ragdollSysShutdown(void)
{
#ifdef PD_PHYSICS
	pdphysShutdown();
#endif
}

void ragdollTick(void)
{
#ifdef PD_PHYSICS
	if (!g_RagdollLevelLoaded || g_Vars.stagenum >= STAGE_TITLE) {
		return;
	}

	ragdollUpdateCeilings();

	// Grabbing bodies and moving objects (objects work without any ragdolls)
	if (g_Vars.lvupdate60freal > 0.0f) {
		ragdollGrabTick();
	}

	ragdollTickAll();
	ragdollExamineTick();
	ragdollTestSpawnTick();
#endif
}

#ifndef PD_PHYSICS
void ragdollStageLoad(void) {}
void ragdollStageUnload(void) {}
void ragdollOnChrDeath(struct chrdata *chr, struct coord *dir, s32 hitpart, struct gset *gset, bool knockout) {}
bool ragdollIsChrRagdolled(struct chrdata *chr) { return false; }
s32 ragdollIsHolding(void) { return false; }
void ragdollOnWitnessDeath(struct chrdata *guard, struct chrdata *victim) {}
s32 ragdollBodyBlocksShot(struct chrdata *shooter, struct prop *targetprop, struct coord *gunpos,
		struct coord *aimpos, struct coord *dir, struct gset *gset, struct coord *outhit) { return false; }
bool ragdollIsChrSettled(struct chrdata *chr) { return false; }
bool ragdollMpKeepsBody(struct chrdata *chr) { return false; }
s32 ragdollMaxActive(void) { return RD_MAXBODIES_LIMIT; }
bool ragdollBodiesVanilla(void) { return false; }
bool ragdollSolidEnabled(void) { return false; }
void ragdollOnBotDeath(struct chrdata *chr, struct coord *dir, s32 hitpart, struct gset *gset) {}
void ragdollOnGlassBroken(struct prop *prop) {}
bool ragdollOnCombatantRespawn(struct chrdata *chr, struct player *player) { return false; }
void ragdollOnChrRemoved(struct chrdata *chr) {}
void ragdollOnChrShot(struct chrdata *chr, s32 hitpart, struct coord *dir, struct gset *gset) {}
s32 ragdollGetExtraChrSlots(void) { return 0; }
void ragdollOnExplosion(struct chrdata *chr, struct coord *centre, f32 radius, f32 damage) {}
void ragdollOnChrYeet(struct chrdata *chr, struct coord *exppos) {}
s32 ragdollOnBikeHitChr(struct chrdata *chr, struct prop *bike, f32 speedx, f32 speedz, f32 recentdist) { return false; }
s32 ragdollOnSpecialDieEnd(struct chrdata *chr) { return false; }
Gfx *ragdollRenderCaps(Gfx *gdl, struct chrdata *chr) { return gdl; }
s32 ragdollIsAnyPieceOnScreen(struct chrdata *chr, f32 size) { return false; }
s32 ragdollHasCutPieces(struct chrdata *chr) { return false; }
s32 ragdollWantPlayerBody(void) { return false; }
void ragdollShotgunVolleyBegin(void) {}
void ragdollShotgunPellet(void) {}
void ragdollShotgunPelletHit(struct chrdata *chr, s32 hitpart, struct coord *dir, struct gset *gset) {}
void ragdollShotgunVolleyEnd(void) {}
void ragdollMeleeAttackBegin(void) {}
void ragdollMeleeAttackEnd(void) {}
void ragdollReaperGrind(void) {}
s32 ragdollBlocksDoor(struct doorobj *door) { return false; }
s32 ragdollDoorStuckOnBody(struct doorobj *door) { return false; }
void ragdollOnChrKnockOut(struct chrdata *chr, struct coord *dir, s32 hitpart, struct gset *gset) {}
s32 ragdollOnKnockOutFall(struct chrdata *chr) { return false; }
void ragdollOnChrPunched(struct chrdata *chr, s32 hitpart, struct coord *dir, struct gset *gset) {}
void ragdollAlertOthersOfDeath(struct chrdata *chr) { chrAlertOthersOfInjury(chr, true); }
void ragdollPlayerBodyTick(void) {}
void ragdollOnPlayerExplosion(struct prop *prop, struct coord *centre, f32 radius, f32 damage) {}
void ragdollOnPlayerHit(struct prop *prop, struct coord *dir, s32 hitpart, struct gset *gset) {}
void ragdollOnPlayerNewLife(void) {}
s32 ragdollGetDeathCam(struct coord *pos, struct coord *up, struct coord *look, struct coord *from) { return false; }
s32 ragdollHoldDeathCam(void) { return false; }
struct prop *ragdollAimAtDeadPlayerBegin(struct chrdata *chr, struct coord *saved) { return NULL; }
void ragdollAimAtDeadPlayerEnd(struct prop *target, struct coord *saved) {}
s32 ragdollShotNearCutPiece(struct chrdata *chr, struct coord *gunpos, struct coord *gundir, f32 radius) { return false; }
bool ragdollModelHasPose(struct model *model) { return false; }
bool ragdollUpdateNodeMtx(struct modelrenderdata *renderdata, struct model *model, struct modelnode *node) { return false; }
#endif

/**
 * MOD: physics presets - Extended > Physics > Preset. "Default" puts every
 * ragdoll/dismemberment setting back to its default (the values above, taken
 * when the settings are registered, before pd.ini is read); any change shows
 * "Custom". The Ragdolls switch and the debug key aren't part of a preset.
 */
struct rdpresetvar {
	bool isfloat;
	void *ptr;
	f32 def;
};

#define RD_PRESET_MAXVARS 48

static struct rdpresetvar g_RdPresetVars[RD_PRESET_MAXVARS];
static s32 g_RdNumPresetVars = 0;

static void rdPresetAdd(bool isfloat, void *ptr)
{
	if (g_RdNumPresetVars < RD_PRESET_MAXVARS) {
		g_RdPresetVars[g_RdNumPresetVars].isfloat = isfloat;
		g_RdPresetVars[g_RdNumPresetVars].ptr = ptr;
		g_RdPresetVars[g_RdNumPresetVars].def = isfloat ? *(f32 *)ptr : (f32)*(s32 *)ptr;
		g_RdNumPresetVars++;
	}
}

#define RD_PRESET_INT(key, ptr, min, max) do { configRegisterInt(key, ptr, min, max); rdPresetAdd(false, ptr); } while (0)
#define RD_PRESET_FLOAT(key, ptr, min, max) do { configRegisterFloat(key, ptr, min, max); rdPresetAdd(true, ptr); } while (0)

s32 ragdollPresetGet(void)
{
	s32 i;

	for (i = 0; i < g_RdNumPresetVars; i++) {
		struct rdpresetvar *v = &g_RdPresetVars[i];

		if (v->isfloat) {
			f32 d = *(f32 *)v->ptr - v->def;

			if (d > 0.005f || d < -0.005f) {
				return RD_PRESET_CUSTOM;
			}
		} else if (*(s32 *)v->ptr != (s32)v->def) {
			return RD_PRESET_CUSTOM;
		}
	}

#ifdef PD_GRAVITY
	if (!gravityIsDefault()) {
		return RD_PRESET_CUSTOM; // MOD: gravity (in the Physics menu too)
	}
#endif

	return RD_PRESET_DEFAULT;
}

void ragdollPresetApply(s32 preset)
{
	s32 i;

	if (preset != RD_PRESET_DEFAULT) {
		return; // Custom: keep what's set
	}

	for (i = 0; i < g_RdNumPresetVars; i++) {
		struct rdpresetvar *v = &g_RdPresetVars[i];

		if (v->isfloat) {
			*(f32 *)v->ptr = v->def;
		} else {
			*(s32 *)v->ptr = (s32)v->def;
		}
	}

#ifdef PD_GRAVITY
	gravityResetDefaults(); // MOD: gravity
#endif
}

PD_CONSTRUCTOR static void ragdollConfigInit(void)
{
	configRegisterInt("Ragdoll.Ragdolls", &g_RagdollsEnabled, 0, 1);
	RD_PRESET_FLOAT("Ragdoll.RagdollPushScale", &g_RagdollPushScale, 0.0f, 10.0f);
	configRegisterInt("Ragdoll.RagdollTestSpawnKey", &g_RagdollTestSpawnKey, 0, 1);
	configRegisterInt("Ragdoll.RagdollMpSolid", &g_RagdollMpSolid, RD_SOLID_OFF, RD_SOLID_BOTH); // MOD: combat simulator ragdolls
	RD_PRESET_INT("Ragdoll.RagdollMaxBodies", &g_RagdollMaxBodies, -1, RD_MAXBODIES_LIMIT);
	RD_PRESET_INT("Ragdoll.RagdollMaxPhysical", &g_RagdollMaxPhysical, 0, RD_MAXBODIES_LIMIT);
	RD_PRESET_INT("Ragdoll.RagdollMpMaxBodies", &g_RagdollMpMaxBodies, -1, RD_MAXBODIES_LIMIT); // MOD: combat simulator ragdolls
	RD_PRESET_INT("Ragdoll.RagdollMpMaxPhysical", &g_RagdollMpMaxPhysical, 0, RD_MAXBODIES_LIMIT);
	RD_PRESET_INT("Ragdoll.RagdollChrPush", &g_RagdollChrPush, 0, 1);
	RD_PRESET_INT("Ragdoll.RagdollGrab", &g_RagdollGrab, 0, 1);
	RD_PRESET_FLOAT("Ragdoll.RagdollGrabStrength", &g_RagdollGrabStrength, 0.25f, 4.0f);
	RD_PRESET_INT("Ragdoll.RagdollMuscles", &g_RagdollMuscles, 0, 1);
	RD_PRESET_FLOAT("Ragdoll.RagdollMuscleStrength", &g_RagdollMuscleStrength, 0.0f, 3.0f);
	RD_PRESET_FLOAT("Ragdoll.RagdollMuscleTime", &g_RagdollMuscleTime, 0.1f, 10.0f);
	RD_PRESET_FLOAT("Ragdoll.RagdollMuscleStrengthMin", &g_RagdollMuscleStrengthMin, 0.0f, 3.0f);
	RD_PRESET_FLOAT("Ragdoll.RagdollMuscleTimeMin", &g_RagdollMuscleTimeMin, 0.1f, 10.0f);
	RD_PRESET_INT("Ragdoll.RagdollAnimBlendChance", &g_RagdollAnimBlend, 0, 100); // (was Game.RagdollAnimBlend, on/off)
	RD_PRESET_INT("Ragdoll.RagdollHeadshotLifelessChance", &g_RagdollHeadshotLifelessChance, 0, 100);
	RD_PRESET_INT("Ragdoll.RagdollDeathCam", &g_RagdollDeathCam, 0, 1);
	RD_PRESET_FLOAT("Ragdoll.RagdollDeathCamSmooth", &g_RagdollDeathCamSmooth, 0.0f, 0.95f);
	RD_PRESET_FLOAT("Ragdoll.RagdollDeathCamBodyView", &g_RagdollDeathCamBodyView, 0.0f, 1.0f);
	configRegisterInt("Ragdoll.RagdollDeathCamControl", &g_RagdollDeathCamControl, 0, 1);  // ini only
	RD_PRESET_INT("Ragdoll.RagdollDeathCamHold", &g_RagdollDeathCamHold, 0, 1);
	RD_PRESET_INT("Ragdoll.RagdollReflexBraceChance", &g_RagdollReflexBraceChance, 0, 100);
	RD_PRESET_INT("Ragdoll.RagdollReflexHead", &g_RagdollReflexHead, 0, 1);
	RD_PRESET_INT("Ragdoll.RagdollReflexStumble", &g_RagdollReflexStumble, 0, 1);
	RD_PRESET_INT("Ragdoll.RagdollReflexClutchChance", &g_RagdollReflexClutchChance, 0, 100);
	RD_PRESET_INT("Ragdoll.RagdollHeadshotLimpChance", &g_RagdollHeadshotLimpChance, 0, 100);
	RD_PRESET_INT("Ragdoll.RagdollDismember", &g_RagdollDismember, 0, 1);
	RD_PRESET_FLOAT("Ragdoll.RagdollDismemberDamage", &g_RagdollDismemberDamage, 0.5f, 10.0f);
	RD_PRESET_INT("Ragdoll.RagdollDismemberChance", &g_RagdollDismemberChance, 0, 100);
	RD_PRESET_INT("Ragdoll.RagdollDismemberExplosions", &g_RagdollDismemberExplosions, 0, 1);
	RD_PRESET_INT("Ragdoll.RagdollDismemberTorso", &g_RagdollDismemberTorso, 0, 1);
	RD_PRESET_INT("Ragdoll.RagdollBodyAlerts", &g_RagdollBodyAlerts, 0, 1);
	RD_PRESET_INT("Ragdoll.RagdollBodyCover", &g_RagdollBodyCover, 0, 1);
	// ini only (no menu entry, not in the presets)
	configRegisterFloat("Ragdoll.RagdollDeadlyPunchPushScale", &g_RagdollDeadlyPunchPushScale, 0.0f, 10.0f);
	configRegisterFloat("Ragdoll.RagdollSlopeGrip", &g_RagdollSlopeGrip, 0.0f, 1.5f);
	configRegisterFloat("Ragdoll.RagdollScreamAirTime", &g_RagdollScreamAirTime, 0.0f, 10.0f);
	configRegisterFloat("Ragdoll.RagdollGlassBreakSpeed", &g_RagdollGlassBreakSpeed, 150.0f, 1500.0f);
	RD_PRESET_INT("Ragdoll.RagdollLedgeCatch", &g_RagdollLedgeCatch, 0, 1);
	RD_PRESET_FLOAT("Ragdoll.RagdollReflexStrength", &g_RagdollReflexStrength, 0.0f, 3.0f);
	RD_PRESET_FLOAT("Ragdoll.RagdollReflexStrengthMin", &g_RagdollReflexStrengthMin, 0.0f, 3.0f);
}
