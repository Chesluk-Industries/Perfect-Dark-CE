#ifndef _IN_RAGDOLL_H
#define _IN_RAGDOLL_H

#include <PR/ultratypes.h>

/**
 * MOD: ragdoll physics - game-side glue (port/src/ragdoll.c).
 *
 * Plan: enemies that actually die (ACT_DIE) become Jolt ragdolls.
 * Knockouts (tranquilisers, punches: ACT_DRUGGEDDROP etc.) keep their
 * original animations.
 *
 * Stage 1: Jolt is linked in, initialised at startup and smoke-tested.
 * Stage 2: each level's static collision is mirrored into Jolt.
 * Stage 3: dying humans become Jolt ragdolls.
 * Controlled by pd.ini: [Game] Ragdolls=1
 */

extern s32 g_RagdollsEnabled;
extern f32 g_RagdollPushScale;
extern f32 g_RagdollSlopeGrip;
extern f32 g_RagdollScreamAirTime; // Ragdoll.RagdollScreamAirTime: seconds in the air before a body screams (0 = never)
// MOD: ragdoll physics - the ragdolls' WAV sounds, mixed into the output by audio.c
s32 rdWavActive(void);
void rdWavMix(s16 *front, s16 *rear, u32 frames, s32 dolby);
extern f32 g_RagdollMuscleStrengthMin; // MOD: ragdoll ranges
extern f32 g_RagdollMuscleTimeMin;
extern f32 g_RagdollReflexStrengthMin;
extern s32 g_RagdollTestSpawnKey;
extern s32 g_RagdollMaxBodies;
extern s32 g_RagdollMaxPhysical;
extern s32 g_RagdollChrPush;
extern s32 g_RagdollGrab;
extern f32 g_RagdollGrabStrength;
extern s32 g_RagdollMuscles;
extern f32 g_RagdollMuscleStrength;
extern f32 g_RagdollMuscleTime;
extern s32 g_RagdollAnimBlend;
extern s32 g_RagdollReflexBraceChance;
extern s32 g_RagdollReflexHead;
extern s32 g_RagdollReflexStumble;
extern s32 g_RagdollReflexClutchChance;
extern s32 g_RagdollHeadshotLimpChance;
extern s32 g_RagdollHeadshotLifelessChance;
extern s32 g_RagdollDismember;
extern f32 g_RagdollDismemberDamage;
extern s32 g_RagdollDismemberChance;
extern s32 g_RagdollDismemberExplosions;
extern s32 g_RagdollDismemberTorso;
extern s32 g_RagdollBodyAlerts;
extern s32 g_RagdollBodyCover;
// MOD: combat simulator ragdolls - its own limits; resting bodies turn solid (RD_SOLID_*)
extern s32 g_RagdollMpMaxBodies;
extern s32 g_RagdollMpMaxPhysical;
extern s32 g_RagdollMpSolid;
extern f32 g_RagdollGlassBreakSpeed;
extern s32 g_RagdollLedgeCatch;
extern f32 g_RagdollReflexStrength;

#define RD_MAXBODIES_LIMIT 256
#define RD_MINBODIES 12 // the lowest Max Bodies above Vanilla (about what the original keeps)
#define RD_MINACTIVE 12 // and Max Active Bodies

#define RD_SOLID_OFF       0
#define RD_SOLID_COMBATSIM 1
#define RD_SOLID_BOTH      2

s32 ragdollMaxActive(void);
s32 ragdollBodiesVanilla(void);
s32 ragdollSolidEnabled(void);

// MOD: physics presets (Extended > Physics > Preset)
#define RD_PRESET_DEFAULT 0
#define RD_PRESET_CUSTOM  1
s32 ragdollPresetGet(void);
void ragdollPresetApply(s32 preset);

// Extra main mema heap (bytes) while ragdolls are on, for the larger chr
// vertex store (vtxstorereset.c): 12x CHRVTX/CHRCOL ~ 720 KB
#define RD_MAINMEMA_EXTRA (1024 * 1024)

// Extra per-frame display list / vertex buffer bytes (gfxmemory.c), x2 buffers
#define RD_EXTRA_GFX_RAGDOLLS (1024 * 1024)
#define RD_EXTRA_VTX_RAGDOLLS (256 * 1024)

void ragdollSysInit(void);
void ragdollSysShutdown(void);

// Stage 2: level collision. Load is called right after the stage's
// collision tiles are loaded (lvReset), unload when the stage stops (lvStop).
void ragdollStageLoad(void);
void ragdollStageUnload(void);

// Called once per frame from lvTick()
void ragdollTick(void);

// Stage 3: ragdolls. These are the hooks the game code calls.
struct chrdata;
struct coord;
struct gset;
struct model;
struct modelnode;
struct modelrenderdata;
struct prop;

// End of chrBeginDeath(): starts a ragdoll for real deaths of humans
void ragdollOnChrDeath(struct chrdata *chr, struct coord *dir, s32 hitpart, struct gset *gset, s32 knockout);

// The chr's body is driven by a ragdoll (simulating or settled)
s32 ragdollIsChrRagdolled(struct chrdata *chr);
// True while the player holds a body or an object with Use (Use mustn't do its usual things then)
s32 ragdollIsHolding(void);
// MOD: ragdoll physics - guards look bodies over (chrAlertOthersOfInjury: guard saw victim die)
void ragdollOnWitnessDeath(struct chrdata *guard, struct chrdata *victim);
// The AI list they do it with (in the global AI list table, gailists.c)
#define GAILIST_RD_EXAMINE_BODY 0x0030
extern unsigned char g_RdExamineBodyAilist[];
// MOD: bodies as cover - a held body in the way takes a character's shot at the player
s32 ragdollBodyBlocksShot(struct chrdata *shooter, struct prop *targetprop, struct coord *gunpos,
		struct coord *aimpos, struct coord *dir, struct gset *gset, struct coord *outhit);

// The ragdoll has come to rest (chrTickDie then switches to ACT_DEAD)
s32 ragdollIsChrSettled(struct chrdata *chr);
// glassDestroy: bodies on or by the pane wake up (solid ones too)
void ragdollOnGlassBroken(struct prop *prop);
// MOD: combat simulator ragdolls
// chrTickDead: a simulant's body stays as it respawns (no fade)
s32 ragdollMpKeepsBody(struct chrdata *chr);
// chrDie: a simulant dies
void ragdollOnBotDeath(struct chrdata *chr, struct coord *dir, s32 hitpart, struct gset *gset);
// botSpawn / playerStartNewLife: the body stays (as a corpse chr) or goes
struct player;
s32 ragdollOnCombatantRespawn(struct chrdata *chr, struct player *player);
// chrTickDie(), while the ragdoll is still dying: who sees the body (chrAlertOthersOfInjury)
void ragdollAlertOthersOfDeath(struct chrdata *chr);

// chrRemove(): the chr is going away
void ragdollOnChrRemoved(struct chrdata *chr);

// chrHit(): a shot hit a dead or dying chr
void ragdollOnChrShot(struct chrdata *chr, s32 hitpart, struct coord *dir, struct gset *gset);
s32 ragdollGetRoomGameFloors(s32 roomnum, const f32 **outverts);
void ragdollShotgunVolleyBegin(void);
void ragdollShotgunPellet(void);
void ragdollShotgunPelletHit(struct chrdata *chr, s32 hitpart, struct coord *dir, struct gset *gset);
void ragdollShotgunVolleyEnd(void);
// handTickAttack(): around the player's melee attacks, so the Reaper's blades
// can be told apart from its bullets
void ragdollMeleeAttackBegin(void);
void ragdollMeleeAttackEnd(void);
// handTickAttack(): the Reaper's blades are spinning - cut bodies they touch
void ragdollReaperGrind(void);
// doorsCalcFrac(): a body is where the door would be once closed (it stays open)
struct doorobj;
s32 ragdollBlocksDoor(struct doorobj *door);
// doorsCalcFrac(): a body has kept the door from closing for 2 s (reopen it)
s32 ragdollDoorStuckOnBody(struct doorobj *door);
// chrDamage(): this chr was knocked out (knockouts become ragdolls)
void ragdollOnChrKnockOut(struct chrdata *chr, struct coord *dir, s32 hitpart, struct gset *gset);
// chrTickDruggedComingUp(): the knockout fall starts; true if it's a ragdoll instead
s32 ragdollOnKnockOutFall(struct chrdata *chr);
// handInflictMeleeDamage(): a punch lands on a chr (Deadly Punch punches move bodies)
void ragdollOnChrPunched(struct chrdata *chr, s32 hitpart, struct coord *dir, struct gset *gset);


// explosionInflictDamage(): first frame of a blast reaching this chr
void ragdollOnExplosion(struct chrdata *chr, struct coord *centre, f32 radius, f32 damage);

// chrYeetFromPos(): a blast killed this chr (the game would play a scripted throw)
void ragdollOnChrYeet(struct chrdata *chr, struct coord *exppos);
// bbikeCalculateNewPositionWithPush(): the hoverbike ran into this chr
// (speedx/z: its speed per 60th; recentdist: how far it really moved in the
// last half second). With a long enough run-up it's run over (killed,
// knocked down as a ragdoll): true
s32 ragdollOnBikeHitChr(struct chrdata *chr, struct prop *bike, f32 speedx, f32 speedz, f32 recentdist);
// chrTickDie(): a scripted death animation (Villa snipers, Chicago railing) ended: becomes a ragdoll
s32 ragdollOnSpecialDieEnd(struct chrdata *chr);

// chrRender(), opaque pass: red caps over dismembered joints
#include <PR/gbi.h>
Gfx *ragdollRenderCaps(Gfx *gdl, struct chrdata *chr);

// chr.c visibility: a dismembered corpse is drawn while any cut piece is in view
s32 ragdollIsAnyPieceOnScreen(struct chrdata *chr, f32 size);

// Ragdoll death camera (player.c, solo/co-op): keep the dead player's body,
// turn it into a ragdoll and put the camera in its head
extern s32 g_RagdollDeathCam;
extern f32 g_RagdollDeathCamSmooth;
extern f32 g_RagdollDeathCamBodyView;
s32 ragdollWantPlayerBody(void);
void ragdollPlayerBodyTick(void);
s32 ragdollGetDeathCam(struct coord *pos, struct coord *up, struct coord *look, struct coord *from);
// player.c: the solo mission waits (death camera kept) until Start or Fire (Ragdoll.RagdollDeathCamHold)
s32 ragdollHoldDeathCam(void);
// chr.c, around a chr's AI and firing: a dead player's target position is their ragdoll's head
struct prop *ragdollAimAtDeadPlayerBegin(struct chrdata *chr, struct coord *saved);
void ragdollAimAtDeadPlayerEnd(struct prop *target, struct coord *saved);
extern s32 g_RagdollDeathCamHold;
void ragdollOnPlayerExplosion(struct prop *prop, struct coord *centre, f32 radius, f32 damage);
void ragdollOnPlayerHit(struct prop *prop, struct coord *dir, s32 hitpart, struct gset *gset);
// playerStartNewLife(): a co-op player respawns; their dead body (the same chr) stops being a ragdoll
void ragdollOnPlayerNewLife(void);

// chrTestHit(): cut pieces can be shot wherever they are
s32 ragdollHasCutPieces(struct chrdata *chr);
s32 ragdollShotNearCutPiece(struct chrdata *chr, struct coord *gunpos, struct coord *gundir, f32 radius);

// Extra chr/model slots to reserve per level (setup.c), for corpses and test spawns
s32 ragdollGetExtraChrSlots(void);

// Model matrix hooks (src/lib/model.c)
s32 ragdollModelHasPose(struct model *model);
s32 ragdollUpdateNodeMtx(struct modelrenderdata *renderdata, struct model *model, struct modelnode *node);

#endif
