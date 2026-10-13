#ifndef _IN_RAGDOLL_RD_INTERNAL_H
#define _IN_RAGDOLL_RD_INTERNAL_H

/**
 * MOD: ragdoll physics - what the ragdoll module's files share (port/src/ragdoll.c
 * and port/src/ragdoll/). Not for the rest of the game: see port/include/ragdoll.h.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "lib/model.h"
#include "lib/mtx.h"
#include "game/quaternion.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/game_0b0fd0.h"
#include "game/bg.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/camera.h"
#include "game/env.h"
#include "game/lv.h"
#include "lib/collision.h"
#include "lib/ailist.h"
#include "input.h"
#include "game/playermgr.h"
#include "game/propsnd.h"
#include "platform.h"
#include "config.h"
#include "system.h"
#include "ragdoll.h"
#include "game/cheats.h"
#ifdef PD_CLIPCHECK
#include "clipcheck.h" // MOD: ragdoll physics - water surfaces
#endif
#include "lib/rng.h"
#include "gbiex.h"
#include "game/gfxmemory.h"
#include "game/tex.h"
#include "game/sparks.h"
#include "game/splat.h"

#ifdef PD_PHYSICS
#include "pdphysics.h"
#endif

/* ---- small helpers ---- */

#define RD_MIN(a, b) ((a) < (b) ? (a) : (b))
#define RD_MAX(a, b) ((a) > (b) ? (a) : (b))
#define RD_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define RD_CLAMPLO(x, lo) do { if ((x) < (lo)) (x) = (lo); } while (0) // in place
#define RD_CLAMPHI(x, hi) do { if ((x) > (hi)) (x) = (hi); } while (0)
#define RD_CLAMPTO(x, lo, hi) do { RD_CLAMPLO(x, lo); RD_CLAMPHI(x, hi); } while (0)
#define RD_SQ(x) ((x) * (x))
#define RD_FRAMESTAMP() (g_Vars.lvframe60 ? g_Vars.lvframe60 : 1) // a frame number that is never 0 (0 = unset)
// A body guards react to: a dead one, or a ragdolled knockout (lying there just the same)
#define RD_ISDYING(rd)  ((rd)->chr->actiontype == ACT_DIE || ((rd)->knockedout && (rd)->chr->actiontype == ACT_DRUGGEDDROP))
#define RD_ISLYING(rd)  ((rd)->chr->actiontype == ACT_DEAD || ((rd)->knockedout && (rd)->chr->actiontype == ACT_DRUGGEDKO))

// ragdoll.c: vectors as f32[3], rotations as Mtxf (column-major: m[col][row])
f32 rdRollRange(f32 lo, f32 hi); // a value in [lo, hi] for one body
void rdVecSub(const f32 *a, const f32 *b, f32 *out);
f32 rdVecLen(const f32 *v);
void rdVecNorm(f32 *v);
void rdVecMad(const f32 *a, const f32 *dir, f32 scale, f32 *out);
void rdRotTransposeMul(Mtxf *a, Mtxf *b, Mtxf *out);
void rdRotMul(Mtxf *a, Mtxf *b, Mtxf *out);
void rdRotApply(Mtxf *a, const f32 *v, f32 *out);
void rdRotApplyT(Mtxf *a, const f32 *v, f32 *out);

static inline f32 rdVecDot(const f32 *a, const f32 *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline f32 rdVecLenSq(const f32 *v)
{
	return rdVecDot(v, v);
}

static inline void rdVecCopy(const f32 *a, f32 *out)
{
	out[0] = a[0];
	out[1] = a[1];
	out[2] = a[2];
}

static inline void rdVecSet(f32 *out, f32 x, f32 y, f32 z)
{
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

static inline void rdVecScale(const f32 *a, f32 s, f32 *out)
{
	out[0] = a[0] * s;
	out[1] = a[1] * s;
	out[2] = a[2] * s;
}

static inline void rdVecDiv(f32 *v, f32 d) // in place (not a multiply by 1/d: the rounding differs)
{
	v[0] /= d;
	v[1] /= d;
	v[2] /= d;
}

static inline void rdVecAdd(const f32 *a, const f32 *b, f32 *out)
{
	out[0] = a[0] + b[0];
	out[1] = a[1] + b[1];
	out[2] = a[2] + b[2];
}

static inline void rdVecCross(const f32 *a, const f32 *b, f32 *out)
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static inline void rdIVecCopy(const s32 *a, s32 *out)
{
	out[0] = a[0];
	out[1] = a[1];
	out[2] = a[2];
}

static inline void rdIVecSet(s32 *out, s32 x, s32 y, s32 z)
{
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

// Squared distances (no sqrt)
static inline f32 rdDistSq(const f32 *a, const f32 *b)
{
	f32 d[3];

	rdVecSub(a, b, d);
	return rdVecLenSq(d);
}

static inline f32 rdDistSqCoord(const f32 *v, const struct coord *c)
{
	f32 dx = v[0] - c->x;
	f32 dy = v[1] - c->y;
	f32 dz = v[2] - c->z;

	return dx * dx + dy * dy + dz * dz;
}

// The collision geo record after this one (NULL for a type we don't know)
static inline struct geo *rdGeoNext(struct geo *geo)
{
	switch (geo->type) {
	case GEOTYPE_TILE_I: return (struct geo *) ((u8 *) geo + (uintptr_t) (geo->numvertices - 0x40) * 6 + 0x18e);
	case GEOTYPE_TILE_F: return (struct geo *) ((u8 *) geo + (uintptr_t) (geo->numvertices - 0x40) * 12 + 0x310);
	case GEOTYPE_BLOCK:  return (struct geo *) ((u8 *) geo + sizeof(struct geoblock));
	case GEOTYPE_CYL:    return (struct geo *) ((u8 *) geo + sizeof(struct geocyl));
	}

	return NULL;
}

// struct coord <-> f32[3]
static inline void rdCoordToVec(const struct coord *c, f32 *out)
{
	out[0] = c->x;
	out[1] = c->y;
	out[2] = c->z;
}

static inline void rdVecToCoord(const f32 *v, struct coord *out)
{
	out->x = v[0];
	out->y = v[1];
	out->z = v[2];
}

#define RD_DISMEMBER_MINDAMAGE 0.5f // the menu slider's minimum


// ragdoll.c
extern s32 g_RagdollsEnabled;
extern f32 g_RagdollPushScale;
extern f32 g_RagdollMuscleStrengthMin; // MOD: ragdoll ranges
extern f32 g_RagdollMuscleTimeMin;
extern f32 g_RagdollReflexStrengthMin;
extern s32 g_RagdollTestSpawnKey;
extern s32 g_RagdollChrPush;
extern s32 g_RdPropMeshLogged;
extern s32 g_RagdollGrab;
extern f32 g_RagdollGrabStrength;
extern s32 g_RagdollMuscles;
extern f32 g_RagdollMuscleStrength;
extern f32 g_RagdollMuscleTime;
extern s32 g_RagdollAnimBlend;
extern s32 g_RagdollDeathCam;
extern f32 g_RagdollDeathCamBodyView;
extern s32 g_RagdollDeathCamControl;
extern s32 g_RagdollDeathCamHold;
#define RD_DEATHLOOK_YAW   1.22f // the dead player's neck turns the head this far each way (70 degrees) ...
#define RD_DEATHLOOK_PITCH 0.61f // ... and nods it this far up or down (35 degrees);
#define RD_DEATHLOOK_MAXYAW   2.62f // the view goes on turning past that (the eyes), up to 150 degrees
#define RD_DEATHLOOK_MAXPITCH 1.40f // ... and 80 degrees up or down
extern f32 g_RagdollDeathCamSmooth;
extern s32 g_RagdollReflexBraceChance;
extern s32 g_RagdollReflexHead;
extern s32 g_RagdollReflexStumble;
extern s32 g_RagdollDismember;
extern f32 g_RagdollDismemberDamage;
extern s32 g_RagdollDismemberChance;
extern s32 g_RagdollDismemberExplosions;
extern s32 g_RagdollDismemberTorso;
extern s32 g_RagdollLedgeCatch;
extern s32 g_RagdollBodyAlerts;
extern s32 g_RagdollBodyCover;
extern f32 g_RagdollGlassBreakSpeed;
extern f32 g_RagdollDeadlyPunchPushScale;
extern bool g_RdCreatingKnockout;
extern bool g_RdDismemberAtDeath;
extern f32 g_RagdollReflexStrength;

#ifdef PD_PHYSICS

// Ragdoll death camera, per player
struct rdplayercam {
	s32 life;         // player->lifestarttime60 of the death this is for (co-op respawns)
	s32 waitframes;   // ticks waited for the game to build the body
	s32 bodyframes;   // ticks since the body exists
	bool failed;      // no body/ragdoll this life: vanilla camera
	bool made;        // the ragdoll exists
	bool camvalid;
	s32 hitframe;     // lvframe60 of the last shot/hit on the player (the killing one knocks the body back)
	struct coord hitdir;
	s32 hitpart;
	struct gset hitgset;
	bool hithasgset;
	s32 blastframe;   // lvframe60 of a blast that hit the player (applied to the body once it exists)
	f32 blastpos[3];
	f32 blastradius;
	f32 blastdamage;
	f32 pos[3];
	f32 look[3];
	f32 up[3];
	f32 nudgeprev[3]; // MOD: death camera control - the chest's position last tick
	bool nudgeprevset;
	bool holdreleased; // Ragdoll.RagdollDeathCamHold: Start or Fire was pressed, the mission can end
	bool stickout;     // death camera control: the move stick was pushed out last tick
	s32 twitchwait60;  // ... ticks until the next twitch
};

// The gun the player's body dropped on death. It lives in the gun memory
// with the body, so it goes when the body does (co-op respawn).
struct rdplayergun {
	struct chrdata *chr;
	struct prop *props[2];
	struct weaponobj *objs[2];
};

// One shotgun shot (its pellets): it only takes a limb off if every pellet
// hits the same limb of the same body
#define RD_VOLLEY_MAXPELLETS 12

struct rdvolley {
	bool active;
	s32 pellets;                  // pellets fired so far
	s32 hits;                     // pellets that hit a chr
	bool pellethit;               // the current pellet already counted
	struct chrdata *chr;
	s32 hitparts[RD_VOLLEY_MAXPELLETS];
	struct coord dir;
	struct gset gset;
};

/**
 * Growable vertex/index buffers used while converting one room.
 */
struct meshbuilder {
	f32 *verts;
	s32 numverts;
	s32 maxverts;
	s32 *indices;
	s32 numtris;
	s32 maxtris;
};

struct geostats {
	s32 tilei;
	s32 tilef;
	s32 blocks;
	s32 cyls;
	s32 unknown;
	s32 tileswall;
	s32 tilesfloor;
	s32 tilesother;
};
#define RD_GROUP_FLOORS 0 // physics level body groups (pdphysLevelAddRoomGroup)
#define RD_GROUP_WALLS 1
#define RD_GROUP_RENDER 2 // walls and ceilings from the render geometry

// Moved-prop detection (ragdollWakeOnMovedProps): last position/rotation of
// every object and door, by prop index
struct rdpropsnap {
	f32 v[6];
	f32 liftvel[3]; // lifts: velocity last tick (units/s)
	bool valid;
};


/*
 * Human skeleton (g_SkelChr) parts, verified from a runtime dump of a
 * Defection guard:
 *
 *   0 pelvis (CHRINFO root)   1 waist/chest   2 neck/head
 *   3/4 shoulders  5/6 elbows  7/8 hands      (3,5,7 one side; 4,6,8 other)
 *   9/10 hips      11/12 knees 13/14 feet     (9,11,13 one side; 10,12,14 other)
 *
 * Bones run along each joint's local X axis; elbows and knees bend around
 * the joint's local Y axis.
 */
#define RD_NUMPARTS     19       // most parts a skeleton has (rd->numparts: this one's)
#define RD_MAXRAGDOLLS  256      // hard cap on ragdoll records (bodies)

/*
 * Skeletons. Parts 0-14 mean the same in every skeleton (the human layout
 * above), so the reflexes, the head and the limbs work alike; a skeleton can
 * add parts after them (the Skedar's tail). nodepart is the model's
 * POSITION node part number for each (0 = the CHRINFO root).
 */
struct rdskel {
	const char *name;
	s32 numparts;
	bool human;                  // human proportions: hand-tuned shapes, reflexes, dismemberment
	s8 nodepart[RD_NUMPARTS];
	s8 parent[RD_NUMPARTS];
	s8 hasbody[RD_NUMPARTS];     // 0: moves rigidly with its parent (hands, feet)
	s8 child[RD_NUMPARTS];       // the part its bone runs to, -1 = none
	// Natural bending direction of each hinge, as the sign of
	// cross(parent bone, child bone) . joint local Y; 2 = a hinge that
	// bends the way it's bent at death
	s8 hingesign[RD_NUMPARTS];
	f32 mass[RD_NUMPARTS];       // kg at RD_MASS_REFTORSO (0: from the shape's size)
	f32 torque[RD_NUMPARTS];     // muscle torque (N*m) per joint at full strength
	f32 cutradius[RD_NUMPARTS];  // cap size per cut joint (fraction of the torso length; non-human: 1 = as thick as the part), 0 = can't be cut
};

enum {
	RDSTATE_FREE,
	RDSTATE_SIMULATING,
	RDSTATE_FROZEN,
};

#define RD_MAXCAPS 40

struct rdcapvtx {
	s16 mtx;         // model matrix index the vertex is drawn with
	s16 v[3];        // its coordinates (as in the mesh)
};

struct rdcap {
	s32 part;        // skeleton part the centre/spray is fixed to
	f32 centre[3];   // fan centre, in that part's frame (model units, rest pose)
	f32 normal[3];   // outwards (spray direction)
	s32 numedges;    // border of the hole (fan triangles with the centre)
	struct rdcapvtx (*edges)[2];
};

struct rddlpatch {
	struct modelrwdata_dl *rwdata;
	Gfx *origgdl;
	Vtx *origvtx;
	Gfx *gdl;
	Vtx *vtx;
};

struct ragdoll {
	s32 state;
	const struct rdskel *skel;         // which skeleton (the human's or the Skedar's)
	s32 numparts;                      // skel->numparts
	f32 hitbox[RD_NUMPARTS][6];        // each part's hit boxes (union, in its joint frame, model units: x/y/z min, max)
	s8 hashitbox[RD_NUMPARTS];
	f32 tip[RD_NUMPARTS][3];           // world: farthest point of what hangs off a part with no part after it (fingers, toes)
	s8 hastip[RD_NUMPARTS];
	u8 thudsample;                     // Skedar: alternates its two footstep sounds
	f32 partradius[RD_NUMPARTS];       // non-human: each part's physics shape radius (units)
	struct chrdata *chr;
	struct model *model;
	struct prop *prop;
	s32 handle;                        // physics handle while simulating
	s32 bodyindex[RD_NUMPARTS];        // physics part index, or -1
	s32 hitpartbody[20];               // HITPART_* -> physics part index
	f32 scale;                         // model scale (world units per model unit)
	Mtxf world[RD_NUMPARTS];           // joint frames, world space, no scale
	struct coord localpos[RD_NUMPARTS];// joint position relative to parent joint (model units)
	Mtxf localrot[RD_NUMPARTS];        // fixed local rotation of parts without a body
	f32 localquat[RD_NUMPARTS][4];     // render-time local rotation (PD quaternion)
	Mtxf rootrender;                   // root render matrix (world, with scale)
	s32 age60;
	u32 serial;
	f32 deathground;                   // chr->ground at the moment of death
	s32 lastpush60;                    // g_Vars.lvframe60 of the last shot push
	bool settled;                      // has come to rest once (chr is ACT_DEAD)
	f32 prevy[2];                      // pelvis/chest height last tick (thud detection)
	f32 prevvy[2];                     // their vertical speed last tick
	s32 lastthud60;                    // g_Vars.lvframe60 of the last thud
	s32 numthuds;
	s16 mtxindex[RD_NUMPARTS];         // model matrix index of each part
	bool powered;                      // motors follow the death animation
	f32 pushscale;                     // push strength (Ragdoll.RagdollPushScale when it died)
	f32 musclestrength;                // this body's rolled settings (rdRollRange): stiffness level,
	f32 muscletime;                    // ... stiffness fade time (s),
	f32 reflexstrength;                // ... reflex strength
	s32 lastexplosion60;               // g_Vars.lvframe60 of the last explosion push
	bool pendingyeet;                  // killed by an explosion; push it if the blast didn't
	struct coord yeetpos;              // where that explosion was
	bool reflexes;                     // fresh death, protective reflexes allowed
	bool driven;                       // joint motors currently on
	f32 brace;                         // 0-1 how far the arms are reaching out
	f32 prevchest[3];                  // chest position last tick (velocity)
	f32 roomsprevpos[3];               // MOD: clipping checks - pelvis when the collision rooms were last set
	bool roomsprevvalid;
	Mtxf headrel;                      // head rotation relative to the chest at death
	bool headguard;                    // the neck holds the head (only right after death, not after a wake-up)
	f32 lookyaw;                       // the dead player looking around: head turn from rest (radians; + = left)
	f32 lookpitch;                     // ... and nod (+ = down)
	s8 hitskel[20];                    // HITPART_* -> skeleton part (no body walking)
	bool canbrace;                     // rolled Ragdoll.RagdollReflexBraceChance at death
	bool stumble;                      // stumble reflex active
	s32 stumblesteps;                  // steps to take
	s32 stumbleleg;                    // 0/1 which leg steps first
	f32 movedir[2];                    // horizontal direction it was moving at death
	s32 clutcharm;                     // arm (skeleton part 3/4) holding the wound, -1 = none
	s32 clutchpart;                    // skeleton part that was hit
	u32 severed;                       // dismemberment: bit per part cut from its parent
	struct rdcap caps[RD_MAXCAPS];     // red caps over the cut ends
	s32 numcaps;
	struct rddlpatch *patches;         // display lists separated at the cuts
	s32 numpatches;
	struct coord renderpos[RD_NUMPARTS]; // render-time joint offsets (cut parts move freely)
	s32 bleed60;
	s32 splat60;
	s32 numdisccaps;                   // caps that had to fall back to a disc
	f32 torso;                         // spine length at death (world units)
	bool blending;                     // following the death animation
	s32 blendbreak60;                  // age when something interrupted the animation, -1 = not
	f32 offset[3];                     // knock-back added to the animation's path (units)
	f32 offvel[3];                     // ... and its velocity (decays)
	s32 twitchpart;                    // blend: skeleton part a shot is jolting (with everything below it), -1 = none
	s32 twitchuntil60;                 // ... until this age
	f32 twitchdir[3];                  // ... the shot's direction (unit)
	s32 blendend60;                    // age when the animation blend ended (reflexes start from there)
	f32 blendfloor;                    // blend: the floor the animation stands on (moved with a lift)
	f32 blendfloor0;                   // blend: the floor under it when it died (measured: chr->ground can be stale on lifts)
	bool blendfloorset;                // ... measured yet
	f32 liftoff[2];                    // blend: how far a lift has carried the animation sideways (x, z)
	s32 bliftframe;                    // blend: lvframe60 a moving lift last carried it, 0 = never
	s32 legcut60;                      // blend: age a leg was cut off, -1 = not
	f32 colroompos[3];                 // pelvis when its collision rooms were last worked out
	bool inwater;                      // floating (water surface at watery)
	f32 watery;
	f32 contacts[PDPHYS_MAX_RAGDOLL_PARTS]; // last tick's new contact speeds per physics part
	s32 thudpart;                      // physics part of the last thud's impact
	bool isplayer;                     // the dead player's own body (death camera)
	bool knockedout;                   // a knockout: alive (ACT_DRUGGEDDROP/KO), not dead
	s32 playernum;
	RoomNum piecerooms[RD_NUMPARTS][8];  // rooms of each cut piece
	s32 piecerooms60;                    // lvframe60 the pieces' rooms were last updated
	f32 pieceprev[RD_NUMPARTS][3];       // each cut piece's position when its rooms were last tracked
	RoomNum playerbaserooms[8];        // player body: the player's own rooms when the body was made
	RoomNum bodyroom;                  // the room the pelvis is in (<= 0 = unknown yet)
	f32 prevpelvis[3];                 // pelvis position when its rooms were last tracked
	s32 propwake60;                    // g_Vars.lvframe60 when a moving lift/object/door last woke it (0 = never)
	bool fellout;                      // fell below RD_KILLFLOOR_Y: frozen for good, NPCs fade out
	struct prop *riding;               // lift it's frozen on and carried by (ragdollCarryOnLift), or NULL
	s32 liftnear60;                    // g_Vars.lvframe60 when a moving lift was last under it (no thuds)
	f32 liftvel[3];                    // that lift's velocity (units/s)
	bool grabbed;                      // held by the player (Use)
	s32 impactsplats;                  // blood splats left by hard landings
	bool sleeping;                     // settled and asleep in the physics world: nothing to update
	f32 friction;                      // friction set for the surface under it (0 = not set yet)
	f32 seenpos[3];                    // body alerts: pelvis when guards last started checking whether they can see it
	bool seenset;
	bool rescanning;                   // ... moved since: guards are being checked again
	f32 seenprev[3];                   // ... pelvis last tick
	s32 seenstill60;                   // ... how long it hasn't moved
	s16 examiners[8];                  // guards who have come to look the body over (chrnums)
	s32 numexaminers;
	s32 actorthud60;                   // g_Vars.lvframe60 of the last thud against a character or the player
	s32 catchpart;                     // ledge catch: physics part holding on
	s32 catchuntil60;                  // ... until this age (0 = not holding)
	f32 catchpos[3];                   // ... where
	s32 numcatches;
	s32 lastcatch60;
	f32 catchprevy;                    // pelvis height last tick (falling?)
	s32 scream60;                      // age60 of its last scream (0 = never)
	f32 screamair;                     // seconds it has been in the air (flying or falling)
	f32 screamprev[3];                 // pelvis last tick
	bool screamprevset;
	s32 firstsleep60;                  // age60 it first fell asleep (came to rest) after the death animation, 0 = not yet
};

#define RD_CRASHCTX(what, chrnum, part) \
	s32 rdctx__ __attribute__((cleanup(rdCtxLeave))) = rdCtxEnter(what, chrnum, part); (void) rdctx__

#define RD_CHRNUM(chr) ((chr) ? (s32)(chr)->chrnum : -1)
#define RD_RDCHRNUM(rd) ((rd) && (rd)->chr ? (s32)(rd)->chr->chrnum : -1)

#define RD_BLEND_TWITCH60 48       // animation blend: a shot part is knocked out of the animation this long
#define RD_BLEND_TWITCHDIST 60.0f  // and this far along the shot at first (fading back)
#define RD_BLEND_TWITCHMAXANG 0.9f // the swing is at most this much (radians)

/* ---- props: doors, lifts, furniture ---- */

/*
 * Props near simulating ragdolls are mirrored into Jolt as kinematic bodies,
 * built from the same geometry the game's own collision uses
 * (propUpdateGeometry). Each tick the geometry is compared with last tick's:
 * if it only moved (lifts, sliding doors) the body is moved with it, if its
 * shape changed (swinging doors, things breaking) the body is rebuilt.
 */
#define RD_MAXPROPS      128

#define RD_PROPFIT_MAXVERTS 96 // doors keep up to this many vertices of how they were built (to follow them turning)

struct ragdollprop {
	struct prop *prop;
	s32 handle;
	f32 origin[3]; // first vertex of the geometry, world space
	u32 sig;       // shape signature (geometry relative to origin)
	bool seen;
	s32 lastimpact60;   // a body last ran into it (pushes, glass)
	s32 nbuild;         // > 0: its vertices as built (a door), for following it as it turns
	u32 layout;         // piece/vertex counts it was built from
	f32 buildorigin[3]; // the physics body's origin when built
	f32 buildcentre[3];
	f32 build[RD_PROPFIT_MAXVERTS * 3];
};
#define RD_LIFT_MAXRELSPEED 350.0f // units/s: a body on a moving lift never moves faster than this relative to it

struct rdobjgrab {
	bool active;
	struct prop *prop;
	f32 dist;
	f32 local[3];   // the point that was grabbed, in the object's own frame
};

/* What each file has for the others */

// ragdoll.c
extern bool g_RagdollLevelLoaded;
extern struct chrdata *g_RagdollCreatingPlayer;
s32 rdPartOfNode(const struct rdskel *skel, s32 nodepart);
extern struct ragdoll g_Ragdolls[RD_MAXRAGDOLLS];
s32 rdCtxEnter(const char *what, s32 chrnum, s32 part);
void rdCtxLeave(s32 *token);
extern s32 g_RagdollNumUsed;
extern u32 g_RagdollChangeSerial;
extern bool g_RagdollSamplingAnim;
struct ragdoll *ragdollFindByChr(struct chrdata *chr);
struct ragdoll *ragdollFindByHandle(s32 handle);
void ragdollOnChrDeath(struct chrdata *chr, struct coord *dir, s32 hitpart, struct gset *gset, bool knockout);
void ragdollOnExplosion(struct chrdata *chr, struct coord *centre, f32 radius, f32 damage);
s32 ragdollWakeFrozen(struct ragdoll *rd);

// ragdoll/create.c
bool ragdollCapturePose(struct ragdoll *rd, struct chrdata *chr);
f32 rdTorsoLength(struct ragdoll *rd);
s32 ragdollCreatePhysics(struct ragdoll *rd, const f32 *vel);

// ragdoll/level.c
void mbReset(struct meshbuilder *mb);
void mbFree(struct meshbuilder *mb);
void ragdollBuildRoom(struct meshbuilder *mbfloor, struct meshbuilder *mbwall, s32 roomnum, struct geostats *stats);
extern s32 g_RdCeilingRooms;
extern s32 g_RdCeilingTris;
extern s32 g_RdInvisibleRooms;
extern s32 g_RdInvisibleTiles;
extern s32 g_RdInvisLowered;
void rdFreeRoomArrays(void);
bool rdEnsureRoomArrays(void);
bool rdObjBboxCorners(struct defaultobj *obj, f32 (*out)[3]);
void rdFreeRoomTris(void);
void ragdollUpdateCeilings(void);

// ragdoll/props.c
extern struct rdpropsnap *g_RdPropSnaps;
extern struct ragdollprop g_RagdollProps[RD_MAXPROPS];
struct ragdollprop *ragdollPropByHandle(s32 ph);
extern s32 g_RagdollNumProps;
void ragdollPropsReleaseAll(void);
void ragdollSyncProps(f32 dt);
void ragdollWakeOnMovedProps(void);
void ragdollHandlePropImpacts(void);
s32 ragdollBlocksDoor(struct doorobj *door);
s32 ragdollDoorStuckOnBody(struct doorobj *door);
void ragdollKillKnockedOut(struct ragdoll *rd, f32 impact);
struct ragdoll *ragdollFindPlayerRagdoll(s32 playernum);

// ragdoll/motion.c
void ragdollDriveJoints(struct ragdoll *rd, f32 dt);

// ragdoll/player.c
extern struct rdplayercam g_RdPlayerCam[MAX_PLAYERS];
extern struct rdplayergun g_RdPlayerGuns[MAX_PLAYERS];
s32 ragdollWantPlayerBody(void);
void ragdollPlayerBodyTick(void);
void ragdollOnPlayerExplosion(struct prop *prop, struct coord *centre, f32 radius, f32 damage);
void ragdollOnPlayerHit(struct prop *prop, struct coord *dir, s32 hitpart, struct gset *gset);
s32 ragdollGetDeathCam(struct coord *pos, struct coord *up, struct coord *look, struct coord *from);
bool ragdollHideHeadForViewer(struct ragdoll *rd);

// ragdoll/dismember.c
extern struct rdvolley g_RdVolley;
struct modelnode *ragdollWalkNext(struct model *model, struct modelnode *node);
void ragdollApplyRelations(struct model *model);
s32 ragdollPieceOf(struct ragdoll *rd, s32 part);
void ragdollRestoreSeams(struct ragdoll *rd);
void ragdollFreeCaps(struct ragdoll *rd);
s32 ragdollHasCutPieces(struct chrdata *chr);
s32 ragdollShotNearCutPiece(struct chrdata *chr, struct coord *gunpos, struct coord *gundir, f32 radius);
void ragdollMeleeAttackBegin(void);
void ragdollMeleeAttackEnd(void);
void ragdollReaperGrind(void);
void ragdollTryDismemberShot(struct ragdoll *rd, s32 hitpart, struct coord *dir, struct gset *gset);
void ragdollShotgunVolleyBegin(void);
void ragdollShotgunPellet(void);
void ragdollShotgunPelletHit(struct chrdata *chr, s32 hitpart, struct coord *dir, struct gset *gset);
void ragdollShotgunVolleyEnd(void);
void ragdollTryDismemberBlast(struct ragdoll *rd, struct coord *centre, f32 radius);
Gfx *ragdollRenderCaps(Gfx *gdl, struct chrdata *chr);
void ragdollBleed(struct ragdoll *rd);

// ragdoll/rooms.c
void ragdollUpdateBodyRooms(struct ragdoll *rd);
void ragdollUpdateCollisionRooms(struct ragdoll *rd);
void ragdollUpdatePieceRooms(struct ragdoll *rd);
s32 ragdollIsAnyPieceOnScreen(struct chrdata *chr, f32 size);

// ragdoll/world.c
void ragdollBodyHitsActor(struct ragdollprop *rp, pdphysPropImpact *pi);
void ragdollCheckSeen(struct ragdoll *rd);
void ragdollLedgeCatch(struct ragdoll *rd);
void ragdollCheckThud(struct ragdoll *rd);
void ragdollCheckScream(struct ragdoll *rd, f32 dt);
void ragdollScreamTick(void);

// wavsound.c: sounds from WAV files in data/sounds/, played at a body
#define RDWAV_SCREAM 0
bool rdWavAvailable(s32 which);
void rdWavPlay(s32 which, struct prop *prop, s16 likesound);
void rdWavTick(void);
void rdWavStopAll(void);
void ragdollUpdateFriction(struct ragdoll *rd);
void ragdollCheckWater(struct ragdoll *rd);

// ragdoll/examine.c
void ragdollExamineTick(void);

// ragdoll/grab.c
bool rdObjGrabbable(struct prop *prop);
void rdObjGrabReset(void);
bool rdObjIsHeld(struct prop *prop);
s32 ragdollIsHolding(void);
void ragdollGrabTick(void);

// ragdoll/testspawn.c
void ragdollTestSpawnTick(void);

/* ---- shared inline helpers that need struct ragdoll ---- */

// The skeleton part with a physics body behind a HITPART_* (hands -> forearms, feet -> shins), or -1
static inline s32 rdHitpartPart(const struct ragdoll *rd, s32 hitpart)
{
	s32 sp = (hitpart >= 0 && hitpart < 20) ? rd->hitskel[hitpart] : -1;

	while (sp > 0 && rd->bodyindex[sp] < 0) {
		sp = rd->skel->parent[sp];
	}

	return sp;
}

#endif // PD_PHYSICS

#endif
