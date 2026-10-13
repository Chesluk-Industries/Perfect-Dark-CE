/**
 * MOD: ragdoll physics - guards looking bodies over (Ragdoll.RagdollBodyAlerts).
 *
 * The game only tells guards about a death while it's happening (and the
 * module when a body is moved, ragdollCheckSeen), so a guard walking past an
 * old body never noticed it. Here an unalerted guard running the standard
 * unalerted list (GAILIST_UNALERTED) who sees a body of his side, or a death,
 * runs the list below: out of a sitting or operating animation first, then
 * the game's own reaction (surprised, jog over, kneel, look it over), a look
 * around, and a search for whoever did it. In the game only guards a level
 * marks went over to bodies. Guards on a level's own AI lists (a level may
 * do something special when a body is found) and alert guards are left alone.
 */

#include "rd_internal.h"
#include "stagesetup.h" // the AI list commands

#ifdef PD_PHYSICS

#define RD_EXAMINE_RANGE  1500.0f // a guard notices a body this close (in view, line of sight)
#define RD_EXAMINE_RAISE  50.0f   // looked for a little above the body (it's on the floor)
#define RD_EXAMINE_PERPASS 2      // at most this many guards start on one body per check

// Checks made while he's on his way and at the body: something happened -> alert
#define RD_EXAMINE_CHECKS(seen) \
	if_saw_injury(0x00, /*goto*/ 0x70) \
	chr_toggle_p1p2(CHR_SELF) \
	set_target_chr(CHR_P1P2) \
	if_hears_target(/*goto*/ 0x70) \
	if_target_outside_my_yvisang(/*goto*/ seen) \
	if_can_see_target(/*goto*/ 0x70) \
	label(seen)

u8 g_RdExamineBodyAilist[] = {
	// Idling (the level's sitting or operating animation): out of it first,
	// as the game's own lists do. Sitting, he stands up (and is no longer
	// killed by one shot, nor dies with the chair death)
	if_self_flag_bankx_eq(CHRFLAG1_DOINGIDLEANIMATION, FALSE, BANK_1, /*goto*/ 0x12)
	unset_self_flag_bankx(CHRFLAG1_DOINGIDLEANIMATION, BANK_1)
	set_chr_special_death_animation(CHR_SELF, SPECIALDIE_NONE)
	if_chr_idle_action_eq(IDLEACTION_SITTING_TYPING, /*goto*/ 0x11)
	if_chr_idle_action_eq(IDLEACTION_SITTING_DORMANT, /*goto*/ 0x11)
	stop_chr
	goto_next(0x17)

	label(0x11)
	set_chr_maxdamage(CHR_SELF, 40)
	object_do_animation(ANIM_025A, 0xff, 0x02, 0xffff)
	chr_do_animation(ANIM_STAND_UP_FROM_SITTING, 0, -1, CHRANIMFLAG_MOVEWHENINVIS, 16, CHR_SELF, 2)
	restart_timer

	beginloop(0x10)
		if_chr_stopped(/*goto*/ 0x17)
		if_timer_gt(120, /*goto*/ 0x17)
	endloop(0x10)

	// Surprised, if he can be (as the game's own reaction)
	label(0x12)
	if_self_flag_bankx_eq(CHRFLAG0_SURPRISABLE, FALSE, BANK_0, /*goto*/ 0x17)
	say_quip(CHR_BOND, QUIP_SURPRISED, 0xff, 0x03, 0xff, BANK_0, 0x00, 0x00)
	restart_timer
	if_self_flag_bankx_eq(CHRFLAG1_DOINGIDLEANIMATION, TRUE, BANK_1, /*goto*/ 0x76)
	chr_do_animation(ANIM_SURPRISED_0202, 0, -1, 0, 16, CHR_SELF, 2)

	beginloop(0x76)
		if_timer_gt(60, /*goto*/ 0x17)
	endloop(0x76)

	// Over to the body (where he can't get to it, he looks it over from here)
	label(0x17)
	try_jog_to_chr(CHR_SEEDIE, /*goto*/ 0x7e)
	goto_next(0x7f)

	beginloop(0x7e)
		if_saw_injury(0x00, /*goto*/ 0x70)
		chr_toggle_p1p2(CHR_SELF)
		set_target_chr(CHR_P1P2)
		if_target_outside_my_yvisang(/*goto*/ 0x13)
		if_can_see_target(/*goto*/ 0x70)
		label(0x13)
		if_distance_to_chr_lt(150, CHR_SEEDIE, /*goto*/ 0x7f)
		if_chr_stopped(/*goto*/ 0x7f)
	endloop(0x7e)

	// Kneel and look it over
	label(0x7f)
	restart_timer
	kneel

	beginloop(0x7c)
		RD_EXAMINE_CHECKS(0x14)
		if_timer_gt(150, /*goto*/ 0x7d)
	endloop(0x7c)

	label(0x7d)
	say_quip(CHR_BOND, QUIP_INSPECTBODY, 0xff, 0x03, 0xff, BANK_0, 0x00, 0x00)
	restart_timer

	beginloop(0x7b)
		RD_EXAMINE_CHECKS(0x15)
		if_timer_gt(90, /*goto*/ 0x7a)
	endloop(0x7b)

	// Up, and look around: whoever did it may still be about
	label(0x7a)
	stop_chr
	restart_timer

	beginloop(0x79)
		RD_EXAMINE_CHECKS(0x18)
		if_timer_gt(20, /*goto*/ 0x78)
	endloop(0x79)

	label(0x78)
	say_quip(CHR_BOND, QUIP_HEARNOISE, 0xff, 0x03, 0xff, BANK_0, 0x00, 0x00)
	restart_timer
	call_rng
	if_rand_gt(128, /*goto*/ 0x77)
	chr_do_animation(ANIM_LOOK_AROUND_025B, 0, 193, CHRANIMFLAG_COMPLETED | CHRANIMFLAG_SLOWUPDATE, 16, CHR_SELF, 2)
	goto_next(0x75)

	label(0x77)
	chr_do_animation(ANIM_LOOK_AROUND_025C, 0, 294, CHRANIMFLAG_COMPLETED | CHRANIMFLAG_SLOWUPDATE, 16, CHR_SELF, 2)

	label(0x75)

	beginloop(0x74)
		RD_EXAMINE_CHECKS(0x19)
		if_timer_gt(200, /*goto*/ 0x73)
	endloop(0x74)

	// Done (as the game ends its examination): the player close to being
	// seen -> alert, otherwise go searching
	label(0x73)
	set_target_chr(CHR_BOND)
	if_nearly_in_targets_sight(30, /*goto*/ 0x71)
	if_chr_death_animation_finished(CHR_COOP, /*goto*/ 0x72)
	set_target_chr(CHR_COOP)
	if_nearly_in_targets_sight(30, /*goto*/ 0x71)

	label(0x72)
	set_target_chr(CHR_P1P2)
	if_self_flag_bankx_eq(CHRFLAG1_DOINGIDLEANIMATION, FALSE, BANK_1, /*goto*/ 0x13)
	set_ailist(CHR_SELF, GAILIST_WAKEUP)

	label(0x13)
	set_ailist(CHR_SELF, GAILIST_SEARCH_FOR_PLAYER)

	// Nearly seen the player
	label(0x71)
	set_alertness(100)
	set_chr_hiddenflag(CHR_TARGET, CHRHFLAG_ALMOSTUNCOVERED)
	set_ailist(CHR_SELF, GAILIST_UNALERTED)

	// Heard, saw or was hit by something: alert (the unalerted list takes it
	// from there, as when the game's examination is interrupted)
	label(0x70)
	set_alertness(100)
	set_chr_hiddenflag(CHR_TARGET, CHRHFLAG_ALMOSTUNCOVERED)
	set_ailist(CHR_SELF, GAILIST_UNALERTED)
	endlist
};

// The AI list commands include one called "return"
#undef return

static bool rdExaminedBy(struct ragdoll *rd, struct chrdata *guard)
{
	s32 i;

	for (i = 0; i < rd->numexaminers; i++) {
		if (rd->examiners[i] == guard->chrnum) {
			return true;
		}
	}

	return false;
}

// Can this guard go and look at the body?
static bool rdCanExamine(struct chrdata *guard, struct chrdata *victim)
{
	u8 *unalerted = ailistFindById(GAILIST_UNALERTED);
	s32 race;

	if (!g_RagdollsEnabled || !g_RagdollBodyAlerts || guard == NULL || victim == NULL || guard == victim
			|| guard->prop == NULL || guard->model == NULL || guard->aibot
			|| (guard->chrflags & CHRCFLAG_HIDDEN) || !(guard->prop->flags & PROPFLAG_ENABLED)
			|| guard->ailist == NULL || guard->ailist != unalerted
			|| guard->alertness >= 100
			|| chrHasFlag(guard, CHRFLAG1_PREWARNED, BANK_1)) {
		return false;
	}

	switch (guard->actiontype) {
	case ACT_DIE:
	case ACT_DEAD:
	case ACT_ARGH:
	case ACT_PREARGH:
	case ACT_DRUGGEDDROP:
	case ACT_DRUGGEDKO:
	case ACT_DRUGGEDCOMINGUP:
	case ACT_SURRENDER:
		return false;
	}

	race = CHRRACE(guard);

	if (race == RACE_ROBOT || race == RACE_DRCAROLL || race == RACE_EYESPY) {
		return false;
	}

	return chrCompareTeams(guard, victim, COMPARE_FRIENDS);
}

static void rdStartExamining(struct chrdata *guard, struct ragdoll *rd, const char *why)
{
	u8 *list = ailistFindById(GAILIST_RD_EXAMINE_BODY);

	if (list == NULL) {
		return;
	}

	guard->chrseedie = rd->chr->chrnum;
	guard->ailist = list;
	guard->aioffset = 0;

	if (rd->numexaminers < ARRAYCOUNT(rd->examiners)) {
		rd->examiners[rd->numexaminers++] = guard->chrnum;
	}

	sysLogPrintf(LOG_NOTE, "RAGDOLL: chr %d goes to look at chr %d's body (%s, %.0f away)",
			guard->chrnum, RD_RDCHRNUM(rd), why,
			sqrtf(rdDistSqCoord(rd->world[0].m[3], &guard->prop->pos)));
}

/**
 * chrAlertOthersOfInjury: guard saw victim die, or saw the body (moved) -
 * the game's reaction would start on his next AI tick
 */
void ragdollOnWitnessDeath(struct chrdata *guard, struct chrdata *victim)
{
	struct ragdoll *rd = ragdollFindByChr(victim);

	if (rd == NULL || rd->isplayer || rdExaminedBy(rd, guard) || !rdCanExamine(guard, victim)) {
		return;
	}

	rdStartExamining(guard, rd, RD_ISDYING(rd) ? "saw it fall" : "saw it moved");
}

/**
 * Old bodies (dead, or knocked out as ragdolls): one body per tick, the
 * unalerted guards near it who can see it and haven't looked it over yet go
 * to it
 */
void ragdollExamineTick(void)
{
	RD_CRASHCTX("examine", -1, -1);
	static s32 next = 0;
	struct ragdoll *rd = NULL;
	struct coord pos;
	f32 top;
	s32 numslots = chrsGetNumSlots();
	s32 started = 0;
	s32 i, part;

	if (!g_RagdollBodyAlerts || g_RagdollNumUsed == 0 || g_Vars.lvupdate60 == 0
			|| g_Vars.normmplayerisrunning || ailistFindById(GAILIST_RD_EXAMINE_BODY) == NULL) {
		return;
	}

	for (i = 0; i < RD_MAXRAGDOLLS; i++) {
		struct ragdoll *r = &g_Ragdolls[next];

		next = (next + 1) % RD_MAXRAGDOLLS;

		if (r->state != RDSTATE_FREE && !r->isplayer && !r->fellout && r->chr && r->prop
				&& r->chr->prop == r->prop && RD_ISLYING(r)
				&& r->numexaminers < ARRAYCOUNT(r->examiners)) {
			rd = r;
			break;
		}
	}

	if (rd == NULL) {
		return;
	}

	// A little above the body (the pelvis is a hand's width off the floor)
	top = rd->world[0].m[3][1];

	for (part = 1; part <= 2 && part < rd->numparts; part++) {
		if (rd->bodyindex[part] >= 0 && rd->world[part].m[3][1] > top) {
			top = rd->world[part].m[3][1];
		}
	}

	pos.x = rd->world[0].m[3][0];
	pos.y = top + RD_EXAMINE_RAISE;
	pos.z = rd->world[0].m[3][2];

	for (i = 0; i < numslots && started < RD_EXAMINE_PERPASS; i++) {
		struct chrdata *guard = &g_ChrSlots[i];

		if (guard->model == NULL || guard->prop == NULL
				|| rdDistSqCoord(rd->world[0].m[3], &guard->prop->pos) > RD_SQ(RD_EXAMINE_RANGE)
				|| rdExaminedBy(rd, guard) || !rdCanExamine(guard, rd->chr)) {
			continue;
		}

		if (chrCanWitness(guard, &pos, rd->prop->rooms)) {
			rdStartExamining(guard, rd, "found it");
			started++;
		}
	}
}

#endif // PD_PHYSICS
