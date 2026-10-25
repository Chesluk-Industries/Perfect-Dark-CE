#ifndef _IN_NAMEBANNER_H
#define _IN_NAMEBANNER_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

/**
 * MOD: name banner - the player's name in a box at the bottom left of the
 * HUD, styled like a pickup message. Based on the "Egg" banner in murkantor's
 * perfect_dark_netplay fork.
 *
 * Per player (Extended > Player N Game Options > Name Banner):
 *   Game.PlayerN.NameBanner        0 off (default), 1 on
 *   Game.PlayerN.NameBannerColour  NAMEBANNER_COLOUR_* preset
 *   Game.PlayerN.NameBannerRGB     custom colour, RRGGBB in hex (pd.ini)
 *   Game.PlayerN.NameBannerHighlightRGB  custom highlight, RRGGBB (pd.ini)
 *   Game.PlayerN.NameBannerText    name for solo/co-op (menu: Banner Name;
 *                                  empty = the agent file's name for player 1,
 *                                  "Player N" for the others)
 * In the Combat Simulator the name is the player's profile name.
 */

#define NAMEBANNER_COLOUR_GREEN  0
#define NAMEBANNER_COLOUR_TEAL   1
#define NAMEBANNER_COLOUR_RED    2
#define NAMEBANNER_COLOUR_BLUE   3
#define NAMEBANNER_COLOUR_GOLD   4
#define NAMEBANNER_COLOUR_CUSTOM 5
#define NAMEBANNER_NUMCOLOURS    6

#define NAMEBANNER_MAXTEXT 16

struct namebannercfg {
	s32 enabled;
	s32 colour;
	char rgb[8];          // RRGGBB
	char highlightrgb[8]; // RRGGBB
	char text[NAMEBANNER_MAXTEXT];
};

extern struct namebannercfg g_NameBannerCfg[4];

const char *nameBannerColourName(s32 index);

// player.c playerRenderHud, with the HUD drawn
Gfx *nameBannerRender(Gfx *gdl);
// lv.c, frames the HUD isn't drawn: lets a shown banner fade out
Gfx *nameBannerRenderHidden(Gfx *gdl);

#endif
