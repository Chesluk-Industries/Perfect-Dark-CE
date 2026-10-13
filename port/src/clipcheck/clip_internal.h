#ifndef _IN_CLIP_INTERNAL_H
#define _IN_CLIP_INTERNAL_H

/*
 * MOD: clipping checks - what clipcheck.c shares with its debug view
 * (clipcheck/debugview.c). Not for other modules: they use clipcheck.h.
 */

#include <PR/ultratypes.h>
#include "types.h"

#define CLIP_MAXROOMS 2048

// Debug view: what the last checks found (clipDbgMark in clipcheck.c)
enum {
	CLIPDBG_BLOCK,    // a drawn wall stopped the move (red)
	CLIPDBG_COVERED,  // a drawn surface with the game's collision at it, left to the game (white)
	CLIPDBG_STEP,     // a step edge with a walkable top (cyan)
	CLIPDBG_FLOOR,    // a drawn floor became the ground (green)
	CLIPDBG_CEILING,  // a drawn ceiling stopped a jump (blue)
	CLIPDBG_NUMKINDS
};

#define CLIPDBG_MAXMARKS 96

struct clipdbgmark {
	f32 pos[3];
	s32 kind;
	s32 frame;
};

// Each added room's drawn triangles (for the water level and the debug view)
struct cliproomtris {
	f32 *verts;  // 9 floats per triangle
	u8 *kinds;   // 0 floor, 1 ceiling, 2 wall; not solid (debug only): 3 water, 4 translucent without the game's collision
	s32 numtris;
};

/**
 * Glass panes are objects, drawn apart from the rooms. Most have collision of
 * their own (a block from their model's bounds), which the generated mode
 * keeps. Some don't (OBJFLAG_00000100 off: Defection's lobby panes by the
 * lift, pads 0x24d-0x250): the game relies on its wall tiles there, which
 * the generated mode leaves out. Those panes become walls of the rooms
 * they're in until they're shattered.
 */
struct clipglass {
	struct prop *prop;
	struct defaultobj *obj;
	s16 room;
	s16 live;
	s16 model; // solid by its model's triangles, not a box
	f32 verts[8][3];
};

// clipcheck.c
extern s32 g_ClipReady;
extern struct cliproomtris *g_ClipRoomTris;   // CLIP_MAXROOMS entries, or NULL
extern struct clipglass g_ClipGlass[];
extern s32 g_ClipNumGlass;
extern struct clipdbgmark g_ClipDbgMarks[CLIPDBG_MAXMARKS];
extern s32 g_ClipDbgBlockFrame;               // lvframe60 a drawn wall last blocked a move
extern f32 g_ClipDbgCapsule[4];               // the body capsule checked: low, top, radius, valid
extern s32 g_ClipDbgBudget;                   // triangles drawn per frame

// The rooms around the player (tight: the body, wide: further out)
s32 clipPlayerRoomList(f32 tight, f32 wide, f32 ylow, f32 yhigh, RoomNum *rooms, s32 max);

#endif
