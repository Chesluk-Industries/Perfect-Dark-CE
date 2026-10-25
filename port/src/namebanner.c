/**
 * MOD: name banner - the player's name in a box at the bottom left of the
 * HUD, styled like a pickup message: it wipes in once the mission timer runs,
 * holds with a light running round its border, and wipes out for cutscenes
 * and when the HUD goes away. Off by default.
 *
 * Based on the "Egg" banners in murkantor's perfect_dark_netplay fork
 * (port/src/net/net.c netEggRender), made a per-player setting showing the
 * player's own name: the Combat Simulator profile name, or in solo/co-op the
 * Banner Name (default: the agent file's name for player 1).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "game/game_1531a0.h"
#include "game/hudmsg.h"
#include "game/menugfx.h"
#include "game/options.h"
#include "lib/vi.h"
#include "gbiex.h"
#include "platform.h"
#include "config.h"
#include "system.h"
#include "utils.h"
#include "namebanner.h"

struct namebannercfg g_NameBannerCfg[4] = {
	{ 0, NAMEBANNER_COLOUR_GREEN, "00FF00", "80FF80", "" },
	{ 0, NAMEBANNER_COLOUR_GREEN, "00FF00", "80FF80", "" },
	{ 0, NAMEBANNER_COLOUR_GREEN, "00FF00", "80FF80", "" },
	{ 0, NAMEBANNER_COLOUR_GREEN, "00FF00", "80FF80", "" },
};

static const struct {
	const char *name;
	u32 base;      // border and text, 0xRRGGBB
	u32 highlight; // the light running round the border
} g_NameBannerColours[NAMEBANNER_NUMCOLOURS] = {
	{ "Green",       0x00ff00, 0x80ff80 }, // the pickup messages'
	{ "Teal/Purple", 0x02ac8a, 0x7c02f5 },
	{ "Red",         0xff3030, 0xffa0a0 },
	{ "Blue",        0x3090ff, 0xa0d8ff },
	{ "Gold",        0xffc020, 0xfff0a0 },
	{ "Custom",      0, 0 },              // NameBannerRGB / NameBannerHighlightRGB
};

const char *nameBannerColourName(s32 index)
{
	return index >= 0 && index < NAMEBANNER_NUMCOLOURS ? g_NameBannerColours[index].name : "";
}

enum { NB_HIDDEN, NB_FADEIN, NB_HOLD, NB_FADEOUT };

// "RRGGBB" (a leading # or 0x is fine too); fallback if it isn't one
static u32 nameBannerParseRgb(const char *str, u32 fallback)
{
	char *end;
	u32 value;

	while (*str == ' ' || *str == '#') {
		str++;
	}

	if (str[0] == '\0') {
		return fallback;
	}

	value = strtoul(str, &end, 16);

	return end != str ? (value & 0xffffff) : fallback;
}

// Ticks the mission timer must run before the banner comes in (it lands just
// after the mission starts, not over the intro)
#define NB_INTRO_DELAY 30
// The mission timer only moves in whole 1/60 s steps: above 60 fps it sits
// still for a few frames in normal play. Still for longer than this = paused
// (a cutscene, the end screen).
#define NB_PAUSE_SLACK 20

struct nbanim {
	s32 phase;
	s32 phasestart;   // lvframe60 the phase began
	s32 readyframe;   // lvframe60 the mission timer was first seen running, -1 while paused
	s32 lasttime;     // last bondviewlevtime60 seen
	s32 lastframe;    // lvframe60 of that
	s32 lastadvframe; // lvframe60 the timer last went up, -1 = never
	bool logged;
};

static struct nbanim g_NameBannerAnim[MAX_PLAYERS] = {
	{ NB_HIDDEN, 0, -1, 0, -1, -1, false },
	{ NB_HIDDEN, 0, -1, 0, -1, -1, false },
	{ NB_HIDDEN, 0, -1, 0, -1, -1, false },
	{ NB_HIDDEN, 0, -1, 0, -1, -1, false },
};

// The name to show (empty: none), up to the first newline
static void nameBannerGetName(struct namebannercfg *cfg, char *out, s32 outlen)
{
	const char *src = NULL;
	char fallback[16];
	s32 i;

	if (g_Vars.normmplayerisrunning && g_Vars.currentplayerstats) {
		// The Combat Simulator: the player's profile name
		s32 mpindex = g_Vars.currentplayerstats->mpindex;

		if (mpindex >= 0 && mpindex < MAX_PLAYERS) {
			src = g_PlayerConfigsArray[mpindex].base.name;
		}
	} else if (cfg->text[0] != '\0') {
		// Solo and co-op: the name set for this player (Extended > Game)
		src = cfg->text;
	} else if (cfg == &g_NameBannerCfg[0]) {
		src = g_GameFile.name; // none set: the agent file's for player 1, "Player N" for the others
	} else {
		snprintf(fallback, sizeof(fallback), "Player %d", (s32)(cfg - g_NameBannerCfg) + 1);
		src = fallback;
	}

	out[0] = '\0';

	if (src == NULL) {
		return;
	}

	for (i = 0; i < outlen - 1 && src[i] != '\0' && src[i] != '\n'; i++) {
		out[i] = src[i];
	}

	out[i] = '\0';

	// Nothing but spaces: none
	while (i > 0 && out[i - 1] == ' ') {
		out[--i] = '\0';
	}
}

static Gfx *nameBannerDraw(Gfx *gdl, bool want)
{
	s32 pn = g_Vars.currentplayernum;
	struct nbanim *anim;
	struct namebannercfg *cfg;
	char name[NAMEBANNER_MAXTEXT + 1];
	char measure[NAMEBANNER_MAXTEXT + 2];
	u32 base, highlight, bordercol, textcol, hicol;
	s32 lvf = g_Vars.lvframe60;
	bool show;

	if (pn < 0 || pn >= MAX_PLAYERS || g_Vars.currentplayer == NULL || g_Vars.currentplayerstats == NULL) {
		return gdl;
	}

	anim = &g_NameBannerAnim[pn];
	cfg = &g_NameBannerCfg[g_Vars.currentplayerstats->mpindex & 3];

	// Off and not still fading out: nothing to do (the original HUD as it was)
	if (!cfg->enabled && anim->phase == NB_HIDDEN) {
		return gdl;
	}

	if (!cfg->enabled || g_Vars.stagenum >= STAGE_TITLE || !g_CharsHandelGothicSm || !g_FontHandelGothicSm) {
		want = false;
	}

	nameBannerGetName(cfg, name, sizeof(name));

	if (name[0] == '\0') {
		want = false;

		if (anim->phase == NB_HIDDEN) {
			return gdl;
		}
	}

	// A new level (lvframe60 starts again): start over. A cutscene or the end
	// screen: don't come in, but let a shown banner wipe out.
	if (lvf < anim->phasestart || g_InCutscene || g_MainIsEndscreen) {
		if (lvf < anim->phasestart) {
			anim->phasestart = lvf;
			anim->phase = NB_HIDDEN;
		}

		anim->readyframe = -1;
		anim->lasttime = g_Vars.currentplayer->bondviewlevtime60;
		anim->lastframe = lvf;
		anim->lastadvframe = -1;
	}

	// The mission timer: shown only while it runs
	if (lvf != anim->lastframe) {
		s32 t = g_Vars.currentplayer->bondviewlevtime60;
		bool running;

		if (t > anim->lasttime) {
			anim->lastadvframe = lvf;
		}

		anim->lasttime = t;
		anim->lastframe = lvf;

		running = anim->lastadvframe >= 0 && lvf - anim->lastadvframe <= NB_PAUSE_SLACK;

		if (!running) {
			anim->readyframe = -1;
		} else if (anim->readyframe < 0) {
			// Running again after a pause: one clean wipe in
			anim->readyframe = lvf;
			anim->phase = NB_HIDDEN;
			anim->phasestart = lvf;
		}
	}

	show = want && anim->readyframe >= 0 && lvf - anim->readyframe >= NB_INTRO_DELAY;

	if (show && (anim->phase == NB_HIDDEN || anim->phase == NB_FADEOUT)) {
		anim->phase = NB_FADEIN;
		anim->phasestart = lvf;

		if (!anim->logged) {
			anim->logged = true;
			sysLogPrintf(LOG_NOTE, "NAMEBANNER: player %d shows \"%s\" (%s)", pn + 1, name,
					g_Vars.normmplayerisrunning ? "profile name" : cfg->text[0] ? "Banner Name" : pn == 0 ? "agent file name" : "default");
		}
	} else if (!show) {
		if (anim->phase == NB_HOLD) {
			anim->phase = NB_FADEOUT;
			anim->phasestart = lvf;
		} else if (anim->phase == NB_FADEIN) {
			anim->phase = NB_HIDDEN; // cut short while coming in: just go
		}
	}

	if (anim->phase == NB_HIDDEN || name[0] == '\0') {
		return gdl;
	}

	if (cfg->colour >= 0 && cfg->colour < NAMEBANNER_COLOUR_CUSTOM) {
		base = g_NameBannerColours[cfg->colour].base;
		highlight = g_NameBannerColours[cfg->colour].highlight;
	} else {
		base = nameBannerParseRgb(cfg->rgb, 0x00ff00);
		highlight = nameBannerParseRgb(cfg->highlightrgb, 0x80ff80);
	}

	// As the pickup messages: a faint border, brighter text
	bordercol = (base << 8) | 0x40;
	textcol = (base << 8) | 0xa0;
	hicol = (highlight << 8) | 0xff;

	{
		s32 lineh = 0, tw = 0, discard = 0;
		s32 viewleft = viGetViewLeft() / g_ScaleX;
		s32 viewbottom = viGetViewTop() + viGetViewHeight();
		s32 screenw = viGetWidth();
		s32 screenh = viGetHeight();
		bool aspectfix = PLAYERCOUNT() < 2 || (PLAYERCOUNT() == 2 && optionsGetScreenSplit() == SCREENSPLIT_HORIZONTAL);
		s32 x, y, bx1, by1, bx2, by2;
		f32 animtime, dur;
		s32 elapsed = lvf - anim->phasestart;

		// Height from one line ("name\n", as a pickup message), width from the name
		snprintf(measure, sizeof(measure), "%s\n", name);
		textMeasure(&lineh, &discard, measure, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);
		textMeasure(&discard, &tw, name, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);

		// One line above where the pickup messages come up
		x = viewleft + 27;
		y = viewbottom - 2 * lineh - 24;

		bx1 = x - 3;
		by1 = y - 3;
		bx2 = x + tw + 3;
		by2 = y + lineh + 2;

		// The pickup messages' wipe time (also the fade in and out)
		animtime = (sqrtf((f32)(tw * tw + lineh * lineh)) + 132.0f) / PALUPF(7.0f);
		dur = animtime > 30.0f ? 30.0f : animtime;

		gdl = text0f153628(gdl);

		if (aspectfix) {
			gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeL); // as the pickup messages (follows HUD Centering)
		}

		if (anim->phase != NB_HOLD) {
			// Wiping in from the top left, or out from the bottom right
			bool in = anim->phase == NB_FADEIN;
			f32 a = (f32)elapsed / dur;

			a = a > 1.0f ? 1.0f : a;

			if (in) {
				textSetDiagonalBlend(x, y, (f32)elapsed * PALUPF(7.0f), DIAGMODE_FADEIN);
			} else {
				textSetDiagonalBlend(x + tw, y + lineh, (animtime - (f32)elapsed) * PALUPF(7.0f), DIAGMODE_FADEOUT);
			}

			gdl = hudmsgRenderBox(gdl, bx1, by1, bx2, by2, 1.0f, bordercol, in ? a : 1.0f - a);

			if (in ? a > 0.0f : a < 1.0f) {
				gdl = textRenderProjected(gdl, &x, &y, name, g_CharsHandelGothicSm, g_FontHandelGothicSm, textcol, screenw, screenh, 0, 0);
			}

			textResetBlends();

			if ((f32)elapsed >= animtime) {
				anim->phase = in ? NB_HOLD : NB_HIDDEN;
				anim->phasestart = lvf;
			}
		} else {
			// Shown: the box and name, and a bright light running round the
			// border (top, right, bottom, left)
			s32 bw = bx2 - bx1;
			s32 bh = by2 - by1;
			s32 perim = 2 * (bw + bh);
			s32 seg = 10;
			s32 p = perim > 0 ? (lvf * 2) % perim : 0;
			s32 sx1, sy1, sx2, sy2;

			gdl = hudmsgRenderBox(gdl, bx1, by1, bx2, by2, 1.0f, bordercol, 1.0f);
			gdl = textRenderProjected(gdl, &x, &y, name, g_CharsHandelGothicSm, g_FontHandelGothicSm, textcol, screenw, screenh, 0, 0);

			if (p < bw) {
				sx1 = bx1 + p; sy1 = by1;
				sx2 = sx1 + seg; sy2 = by1 + 2;
				if (sx2 > bx2) sx2 = bx2;
			} else if (p < bw + bh) {
				sx1 = bx2 - 1; sy1 = by1 + (p - bw);
				sx2 = bx2 + 1; sy2 = sy1 + seg;
				if (sy2 > by2) sy2 = by2;
			} else if (p < 2 * bw + bh) {
				sx2 = bx2 - (p - bw - bh); sy1 = by2 - 1;
				sx1 = sx2 - seg; sy2 = by2 + 1;
				if (sx1 < bx1) sx1 = bx1;
			} else {
				sx1 = bx1; sy2 = by2 - (p - 2 * bw - bh);
				sx2 = bx1 + 2; sy1 = sy2 - seg;
				if (sy1 < by1) sy1 = by1;
			}

			gdl = menugfxDrawFilledRect(gdl, sx1, sy1, sx2, sy2, hicol, hicol);
		}

		if (aspectfix) {
			gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
		}

		gdl = text0f153780(gdl);
	}

	return gdl;
}

Gfx *nameBannerRender(Gfx *gdl)
{
	return nameBannerDraw(gdl, true);
}

Gfx *nameBannerRenderHidden(Gfx *gdl)
{
	return nameBannerDraw(gdl, false);
}

PD_CONSTRUCTOR static void nameBannerConfigInit(void)
{
	s32 i;

	for (i = 0; i < 4; i++) {
		configRegisterInt(strFmt("Game.Player%d.NameBanner", i + 1), &g_NameBannerCfg[i].enabled, 0, 1);
		configRegisterInt(strFmt("Game.Player%d.NameBannerColour", i + 1), &g_NameBannerCfg[i].colour, 0, NAMEBANNER_NUMCOLOURS - 1);
		configRegisterString(strFmt("Game.Player%d.NameBannerRGB", i + 1), g_NameBannerCfg[i].rgb, sizeof(g_NameBannerCfg[i].rgb) - 1);
		configRegisterString(strFmt("Game.Player%d.NameBannerHighlightRGB", i + 1), g_NameBannerCfg[i].highlightrgb, sizeof(g_NameBannerCfg[i].highlightrgb) - 1);
		configRegisterString(strFmt("Game.Player%d.NameBannerText", i + 1), g_NameBannerCfg[i].text, NAMEBANNER_MAXTEXT - 1);
	}
}
