#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "game/gamefile.h"
#include "game/player.h"
#include "lib/joy.h"
#include "video.h"
#include "input.h"
#include "config.h"
#include "audio.h" // MOD: surround 5.1
#include "namebanner.h" // MOD: name banner
#include "bodyview.h" // MOD: body awareness
#ifdef PD_GRAVITY
#include "gravity.h" // MOD: gravity
#endif
#ifdef PD_PHYSICS
#include "ragdoll.h" // MOD: ragdoll physics
#endif
#ifdef PD_CLIPCHECK
#include "clipcheck.h" // MOD: clipping checks
#endif

static s32 g_ExtMenuPlayer = 0;
static struct menudialogdef *g_ExtNextDialog = NULL;

static s32 g_BindIndex = 0;
static u32 g_BindContKey = 0;

static MenuItemHandlerResult menuhandlerSelectPlayer(s32 operation, struct menuitem *item, union handlerdata *data);

struct menuitem g_ExtendedSelectPlayerMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Player 1\n",
		0,
		menuhandlerSelectPlayer,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Player 2\n",
		0,
		menuhandlerSelectPlayer,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Player 3\n",
		0,
		menuhandlerSelectPlayer,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Player 4\n",
		0,
		menuhandlerSelectPlayer,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedSelectPlayerMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Select Player",
	g_ExtendedSelectPlayerMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerSelectPlayer(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_ExtMenuPlayer = item - g_ExtendedSelectPlayerMenuItems;
		((char *)g_ExtNextDialog->title)[7] = g_ExtMenuPlayer + '1';
		menuPushDialog(g_ExtNextDialog);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseEnabled(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return inputMouseIsEnabled();
	case MENUOP_SET:
		inputMouseEnable(data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseAimLock(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimmode;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimmode = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseLockMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"Always Off",
		"Always On",
		"Auto"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		inputSetMouseLockMode(data->checkbox.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = inputGetMouseLockMode();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMenuMouseControl(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_MenuMouseControl;
	case MENUOP_SET:
		g_MenuMouseControl = data->checkbox.value;
		if (!g_MenuMouseControl) {
			g_MenuUsingMouse = false;
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseSpeedX(s32 operation, struct menuitem *item, union handlerdata *data)
{
	f32 x, y;

	switch (operation) {
	case MENUOP_GETSLIDER:
		inputMouseGetSpeed(&x, &y);
		if (x < 0.f) {
			data->slider.value = 0;
		} else {
			data->slider.value = x * 100.f + 0.5f;
		}
		break;
	case MENUOP_SET:
		inputMouseGetSpeed(&x, &y);
		inputMouseSetSpeed((f32)data->slider.value / 100.f, y);
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2f", (f32)data->slider.value / 100.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseSpeedY(s32 operation, struct menuitem *item, union handlerdata *data)
{
	f32 x, y;

	switch (operation) {
	case MENUOP_GETSLIDER:
		inputMouseGetSpeed(&x, &y);
		if (y < 0.f) {
			data->slider.value = 0;
		} else {
			data->slider.value = y * 100.f + 0.5f;
		}
		break;
	case MENUOP_SET:
		inputMouseGetSpeed(&x, &y);
		inputMouseSetSpeed(x, (f32)data->slider.value / 100.f);
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2f", (f32)data->slider.value / 100.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseAimSpeedX(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		if (g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedx < 0.f) {
			data->slider.value = 0;
		} else if (g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedx > 10.f) {
			data->slider.value = 1000;
		} else {
			data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedx * 100.f + 0.5f;
		}
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedx = (f32)data->slider.value / 100.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2f", (f32)data->slider.value / 100.f);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseAimSpeedY(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		if (g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedy < 0.f) {
			data->slider.value = 0;
		} else if (g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedy > 10.f) {
			data->slider.value = 1000;
		} else {
			data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedy * 100.f + 0.5f;
		}
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedy = (f32)data->slider.value / 100.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2f", (f32)data->slider.value / 100.f);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRadialMenuSpeed(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		if (g_PlayerExtCfg[0].radialmenuspeed < 0.f) {
			data->slider.value = 0;
		} else if (g_PlayerExtCfg[0].radialmenuspeed > 10.f) {
			data->slider.value = 1000;
		} else {
			data->slider.value = g_PlayerExtCfg[0].radialmenuspeed * 100.f + 0.5f;
		}
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[0].radialmenuspeed = (f32)data->slider.value / 100.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2f", (f32)data->slider.value / 100.f);
	}

	return 0;
}

struct menuitem g_ExtendedMouseMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mouse Enabled",
		0,
		menuhandlerMouseEnabled,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mouse Aim Lock",
		0,
		menuhandlerMouseAimLock,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mouse Lock Mode",
		0,
		menuhandlerMouseLockMode,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mouse Menu Navigation",
		0,
		menuhandlerMenuMouseControl,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Mouse Speed X",
		3000,
		menuhandlerMouseSpeedX,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Mouse Speed Y",
		3000,
		menuhandlerMouseSpeedY,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Crosshair Speed X",
		1000,
		menuhandlerMouseAimSpeedX,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Crosshair Speed Y",
		1000,
		menuhandlerMouseAimSpeedY,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Radial Menu Speed",
		1000,
		menuhandlerRadialMenuSpeed,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedMouseMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Extended Mouse Options",
	g_ExtendedMouseMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerStickSpeed(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerStickDeadzone(s32 operation, struct menuitem *item, union handlerdata *data);

struct menuitem g_ExtendedStickMenuItems[] = {
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"LStick Scale X",
		20,
		menuhandlerStickSpeed,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"LStick Scale Y",
		20,
		menuhandlerStickSpeed,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"RStick Scale X",
		20,
		menuhandlerStickSpeed,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"RStick Scale Y",
		20,
		menuhandlerStickSpeed,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"LStick Deadzone X",
		32,
		menuhandlerStickDeadzone,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"LStick Deadzone Y",
		32,
		menuhandlerStickDeadzone,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"RStick Deadzone X",
		32,
		menuhandlerStickDeadzone,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"RStick Deadzone Y",
		32,
		menuhandlerStickDeadzone,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static MenuItemHandlerResult menuhandlerStickSpeed(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 idx = item - g_ExtendedStickMenuItems;
	const s32 stick = idx / 2;
	const s32 axis = idx % 2;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = inputControllerGetAxisScale(g_ExtMenuPlayer, stick, axis) * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		inputControllerSetAxisScale(g_ExtMenuPlayer, stick, axis, (f32)data->slider.value / 10.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerStickDeadzone(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 idx = item - (g_ExtendedStickMenuItems + 5);
	const s32 stick = idx / 2;
	const s32 axis = idx % 2;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = inputControllerGetAxisDeadzone(g_ExtMenuPlayer, stick, axis) * 32.f + 0.5f;
		break;
	case MENUOP_SET:
		inputControllerSetAxisDeadzone(g_ExtMenuPlayer, stick, axis, (f32)data->slider.value / 32.f);
		break;
	}

	return 0;
}

struct menudialogdef g_ExtendedStickMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Analog Stick Settings",
	g_ExtendedStickMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerVibration(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = inputRumbleGetStrength(g_ExtMenuPlayer) * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		inputRumbleSetStrength(g_ExtMenuPlayer, (f32)data->slider.value / 10.f);
		break;
	case MENUOP_CHECKHIDDEN:
	case MENUOP_CHECKDISABLED:
		if (!inputRumbleSupported(g_ExtMenuPlayer)) {
			return true;
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerAnalogMovement(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return inputControllerGetDualAnalog(g_ExtMenuPlayer);
	case MENUOP_SET:
		inputControllerSetDualAnalog(g_ExtMenuPlayer, data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerSwapSticks(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return inputControllerGetSticksSwapped(g_ExtMenuPlayer);
	case MENUOP_SET:
		inputControllerSetSticksSwapped(g_ExtMenuPlayer, data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerController(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char ctrlname[35];
	s32 ctrls[INPUT_MAX_CONNECTED_CONTROLLERS];
	const s32 numCtrls = inputGetConnectedControllers(ctrls);
	const s32 curCtrl = inputGetAssignedControllerId(g_ExtMenuPlayer);

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = numCtrls + 1; // first option is "None"
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->dropdown.value) {
			const s32 jid = ctrls[data->dropdown.value - 1];
			const char *name = inputGetConnectedControllerName(jid);
			strncpy(ctrlname, name, sizeof(ctrlname) - 1);
			return (intptr_t)ctrlname;
		} else {
			return (intptr_t)"None";
		}
	case MENUOP_SET:
		if (data->dropdown.value == 0) {
			// unassign controller
			inputAssignController(g_ExtMenuPlayer, -1);
			joyReset();
		} else if (data->dropdown.value <= numCtrls) {
			inputAssignController(g_ExtMenuPlayer, ctrls[data->dropdown.value - 1]);
			joyReset();
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		for (s32 i = 0; i < numCtrls; ++i) {
			if (curCtrl == ctrls[i]) {
				data->dropdown.value = i + 1;
				return 0;
			}
		}
		data->dropdown.value = 0;
		break;
	}

	return 0;
}

struct menuitem g_ExtendedControllerMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Controller",
		0,
		menuhandlerController,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Analog Movement",
		0,
		menuhandlerAnalogMovement,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Swap Sticks",
		0,
		menuhandlerSwapSticks,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Stick Settings...\n",
		0,
		(void *)&g_ExtendedStickMenuDialog,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Vibration",
		10,
		menuhandlerVibration,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static char g_ExtendedControllerMenuTitle[] = "Player 1 Controller Options";
struct menudialogdef g_ExtendedControllerMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)g_ExtendedControllerMenuTitle,
	g_ExtendedControllerMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerFullScreen(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return videoGetFullscreen();
	case MENUOP_SET:
		videoSetFullscreen(data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerFullScreenMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"Borderless",
		"Exclusive"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		videoSetFullscreenMode(data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = videoGetFullscreenMode();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCenterWindow(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return videoGetCenterWindow();
	case MENUOP_SET:
		videoSetCenterWindow(data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerVsync(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const s32 numOpts = 10;
	static const char *constOpts[] = {
		"Adaptive",
		"Off",
		"On"
	};
	static char dynOpt[20];
	s32 vblanks;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = numOpts;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->dropdown.value < ARRAYCOUNT(constOpts))
			return (intptr_t)constOpts[data->dropdown.value];
		vblanks = (s32)data->dropdown.value - 1;
		snprintf(dynOpt, sizeof(dynOpt), "On (%d frames)", vblanks);
		return (intptr_t)dynOpt;
	case MENUOP_SET:
		videoSetVsync(data->dropdown.value - 1);
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = videoGetVsync() + 1;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerFramerateLimit(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = videoGetFramerateLimit();
		break;
	case MENUOP_SET:
		videoSetFramerateLimit(data->slider.value);
		break;
	case MENUOP_GETSLIDERLABEL:
		// NOTE: data->slider.label length must not exceed 15.
		if (data->slider.value == 0) {
			strcpy(data->slider.label, "Off");
		} else {
			sprintf(data->slider.label, "%d FPS", data->slider.value);
		}
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMSAA(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 msaa;
	static const char *opts[] = {
		"Off",
		"2x (MSAA)",
		"4x (MSAA)",
		"8x (MSAA)",
		"16x (MSAA)"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		videoSetMSAA(1 << data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		msaa = videoGetMSAA();
		if (msaa < 2) {
			data->dropdown.value = 0;
		} else if (msaa < 4) {
			data->dropdown.value = 1;
		} else if (msaa < 8) {
			data->dropdown.value = 2;
		} else if (msaa < 16) {
			data->dropdown.value = 3;
		} else {
			data->dropdown.value = 4;
		}
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerResolution(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char resstring[32];
	static const char *rescustom = "Custom";
	displaymode mode;

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		if (videoGetFullscreen() && videoGetFullscreenMode() == 0) {
			return true;
		}
		break;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = videoGetNumDisplayModes();
		break;
	case MENUOP_GETOPTIONTEXT:
		videoGetDisplayMode(&mode, data->dropdown.value);
		if (mode.width == 0 && mode.height == 0) {
			return (intptr_t)rescustom;
		} else {
			snprintf(resstring, sizeof(resstring), "%dx%d", mode.width, mode.height);
		}
		return (intptr_t)resstring;
	case MENUOP_SET:
		videoSetDisplayMode(data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = videoGetDisplayModeIndex();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerTexFilter(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"Nearest",
		"Bilinear",
		"Three Point"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		videoSetTextureFilter(data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = videoGetTextureFilter();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerTexDetail(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return (videoGetDetailTextures() != 0);
	case MENUOP_SET:
		videoSetDetailTextures(data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerTexFilter2D(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return videoGetTextureFilter2D();
	case MENUOP_SET:
		videoSetTextureFilter2D(data->checkbox.value);
		g_TexFilter2D = videoGetTextureFilter2D() ? G_TF_BILERP : G_TF_POINT;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerAnisotropicFiltering(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = videoGetAnisotropicFilter();
		break;
	case MENUOP_SET:
		videoSetAnisotropicFilter(data->slider.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerDisplayFPS(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return videoGetDisplayFPS();
	case MENUOP_SET:
		videoSetDisplayFPS(data->checkbox.value);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGeMuzzleFlashes(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_BgunGeMuzzleFlashes;
	case MENUOP_SET:
		g_BgunGeMuzzleFlashes = data->checkbox.value;
		break;
	}

	return 0;
}

// MOD: split-screen glares - by joshuarwood (perfect_dark PR #760)
static MenuItemHandlerResult menuhandlerSplitscreenGlares(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_SplitscreenGlares;
	case MENUOP_SET:
		g_SplitscreenGlares = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerUncapTickrate(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return (g_TickRateDiv == 0);
	case MENUOP_SET:
		g_TickRateDiv = !data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCenterHUD(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"None",
		"4:3",
		"Wide"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_HudCenter = data->checkbox.value;
		if (g_HudCenter == HUDCENTER_NORMAL) {
			g_HudAlignModeL = G_ASPECT_CENTER_EXT;
			g_HudAlignModeR = G_ASPECT_CENTER_EXT;
		} else if (g_HudCenter == HUDCENTER_WIDE) {
			g_HudAlignModeL = G_ASPECT_LEFT_EXT | G_ASPECT_WIDE_EXT;
			g_HudAlignModeR = G_ASPECT_RIGHT_EXT | G_ASPECT_WIDE_EXT;
		}	else {
			g_HudAlignModeL = G_ASPECT_LEFT_EXT;
			g_HudAlignModeR = G_ASPECT_RIGHT_EXT;
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_HudCenter;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerScreenShake(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_ViShakeIntensityMult * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		g_ViShakeIntensityMult = (f32)data->slider.value / 10.f;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGlareBrightness(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = videoGetGlareBrightness() * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		videoSetGlareBrightness((f32)data->slider.value / 10.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerOverexposureScale(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = videoGetOverexposureScale() * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		videoSetOverexposureScale((f32)data->slider.value / 10.f);
		break;
	}

	return 0;
}

// MOD: Video.UnlimitedDecals (applies from the next level load)
static MenuItemHandlerResult menuhandlerUnlimitedDecals(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_UnlimitedDecals;
	case MENUOP_SET:
		g_UnlimitedDecals = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}


// MOD: decal edge clamp (Video.DecalClamp)
extern s32 g_DecalClamp;

static MenuItemHandlerResult menuhandlerDecalClamp(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_DecalClamp;
	case MENUOP_SET:
		g_DecalClamp = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

struct menuitem g_ExtendedVideoMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Full Screen",
		0,
		menuhandlerFullScreen,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Full Screen Mode",
		0,
		menuhandlerFullScreenMode,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Resolution",
		0,
		menuhandlerResolution,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Center Window",
		0,
		menuhandlerCenterWindow,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Anti-aliasing",
		0,
		menuhandlerMSAA,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Vsync",
		0,
		menuhandlerVsync,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE | MENUITEMFLAG_SLIDER_DEFERRED,
		(uintptr_t)"Framerate Limit",
		VIDEO_MAX_FPS,
		menuhandlerFramerateLimit,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Uncap Tickrate",
		0,
		menuhandlerUncapTickrate,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Display FPS",
		0,
		menuhandlerDisplayFPS,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Texture Filtering",
		0,
		menuhandlerTexFilter,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"GUI Texture Filtering",
		0,
		menuhandlerTexFilter2D,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Anisotropic Filtering",
		8,
		menuhandlerAnisotropicFiltering,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Detail Textures",
		0,
		menuhandlerTexDetail,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Unlimited Decals",
		0,
		menuhandlerUnlimitedDecals, // MOD
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Clamp Decals to Edges",
		0,
		menuhandlerDecalClamp, // MOD: decal edge clamp
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"HUD Centering",
		0,
		menuhandlerCenterHUD,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"GE64-style Muzzle Flashes",
		0,
		menuhandlerGeMuzzleFlashes,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Splitscreen Light Glares",
		0,
		menuhandlerSplitscreenGlares,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Explosion Shake",
		20,
		menuhandlerScreenShake,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Glare Brightness",
		10,
		menuhandlerGlareBrightness,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Overexposure Scale",
		10,
		menuhandlerOverexposureScale,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedVideoMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Extended Video Options",
	g_ExtendedVideoMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerDisableMpDeathMusic(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_MusicDisableMpDeath;
	case MENUOP_SET:
		g_MusicDisableMpDeath = data->checkbox.value;
		break;
	}

	return 0;
}

// MOD: surround 5.1 - output option, status, levels and speaker test

static bool surroundSelected(void)
{
	return g_SoundMode == SOUNDMODE_SURROUND;
}

// 5.1 output, levels and the speaker test need the Sound option on Surround
// and a device with 5.1 speakers (a stereo device just gets Dolby Surround)
static bool surround51Usable(void)
{
	return surroundSelected() && audioGetDeviceChannels() >= 6;
}

static MenuItemHandlerResult menuhandlerSurround51(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !surround51Usable();
	case MENUOP_GET:
		return audioGetSurround51() && audioGetDeviceChannels() >= 6;
	case MENUOP_SET:
		audioSetSurround51(data->checkbox.value);
		break;
	}

	return 0;
}

static char *menutextSurroundStatus(struct menuitem *item)
{
	static char text[96];

	if (audioGetOutputChannels() == 6) {
		snprintf(text, sizeof(text), "Output: 5.1 (device has %d channels)\n", audioGetDeviceChannels());
	} else {
		snprintf(text, sizeof(text), "Output: Stereo, Dolby Surround\n");
	}

	return text;
}

static MenuItemHandlerResult menuhandlerSurroundStatus(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return !surroundSelected();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerSurroundHint(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return surroundSelected();
	}

	return 0;
}

static char *menutextSpeakerTest(struct menuitem *item)
{
	static char text[64];
	const char *name = audioGetSpeakerTestName();

	if (name) {
		snprintf(text, sizeof(text), "Speaker Test: %s\n", name);
	} else {
		snprintf(text, sizeof(text), "Speaker Test\n");
	}

	return text;
}

static MenuItemHandlerResult menuhandlerSpeakerTest(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !surround51Usable();
	case MENUOP_SET:
		if (audioGetSpeakerTestName()) {
			audioStopSpeakerTest();
		} else {
			audioStartSpeakerTest();
		}
		break;
	}

	return 0;
}

// MOD: speaker volumes - item->param is the speaker (see audioGetSpeakerVolume)
static MenuItemHandlerResult menuhandlerSpeakerVolume(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		// Centre, subwoofer and rear only exist with 5.1 output
		return !surround51Usable() || (item->param >= 2 && audioGetOutputChannels() != 6);
	case MENUOP_GETSLIDER:
		data->slider.value = audioGetSpeakerVolume(item->param);
		break;
	case MENUOP_SET:
		audioSetSpeakerVolume(item->param, data->slider.value);
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d%%", (s32)data->slider.value);
		break;
	}

	return 0;
}

struct menuitem g_ExtendedAudioLevelsMenuItems[] = {
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Front Left", 150, menuhandlerSpeakerVolume },
	{ MENUITEMTYPE_SLIDER, 1, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Front Right", 150, menuhandlerSpeakerVolume },
	{ MENUITEMTYPE_SLIDER, 2, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Centre", 150, menuhandlerSpeakerVolume },
	{ MENUITEMTYPE_SLIDER, 3, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Subwoofer", 150, menuhandlerSpeakerVolume },
	{ MENUITEMTYPE_SLIDER, 4, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Rear Left", 150, menuhandlerSpeakerVolume },
	{ MENUITEMTYPE_SLIDER, 5, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Rear Right", 150, menuhandlerSpeakerVolume },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL }, // "Back"
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedAudioLevelsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Levels",
	g_ExtendedAudioLevelsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

// (a handler rather than OPENSDIALOG, so it can be greyed out)
static MenuItemHandlerResult menuhandlerAudioLevels(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !surround51Usable();
	case MENUOP_SET:
		menuPushDialog(&g_ExtendedAudioLevelsMenuDialog);
		break;
	}

	return 0;
}

struct menuitem g_ExtendedAudioMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Disable MP Death Music",
		0,
		menuhandlerDisableMpDeathMusic,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"5.1 Surround Output",
		0,
		menuhandlerSurround51,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		0,
		(uintptr_t)&menutextSurroundStatus,
		0,
		menuhandlerSurroundStatus,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Set Sound to Surround in the Audio options\n",
		0,
		menuhandlerSurroundHint,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Levels...\n",
		0,
		menuhandlerAudioLevels,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		(uintptr_t)&menutextSpeakerTest,
		0,
		menuhandlerSpeakerTest,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedAudioMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Extended Audio Options",
	g_ExtendedAudioMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

// MOD: surround 5.1 - the "Surround Setup" shortcut in the game's Audio
// Options (mainmenu.c), shown only while the Sound option is Surround
MenuItemHandlerResult menuhandlerSurroundSetup(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return !surroundSelected();
	case MENUOP_SET:
		menuPushDialog(&g_ExtendedAudioMenuDialog);
		break;
	}

	return 0;
}

// MOD: name banner - Extended > Player N Game Options > Name Banner
// The name on the right of "Banner Name". That slot (param3) takes a text ID
// or a function, never a literal string, even with MENUITEMFLAG_LITERAL_TEXT.
static char g_NameBannerMenuValue[NAMEBANNER_MAXTEXT];

static char *nameBannerMenuValue(struct menuitem *item)
{
	const char *text = g_NameBannerCfg[g_ExtMenuPlayer].text;

	if (text[0] != '\0') {
		snprintf(g_NameBannerMenuValue, sizeof(g_NameBannerMenuValue), "%s", text);
	} else if (g_ExtMenuPlayer == 0) {
		snprintf(g_NameBannerMenuValue, sizeof(g_NameBannerMenuValue), "(Agent File)");
	} else {
		snprintf(g_NameBannerMenuValue, sizeof(g_NameBannerMenuValue), "Player %d", g_ExtMenuPlayer + 1);
	}

	return g_NameBannerMenuValue;
}

static MenuItemHandlerResult menuhandlerNameBanner(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_NameBannerCfg[g_ExtMenuPlayer].enabled;
	case MENUOP_SET:
		g_NameBannerCfg[g_ExtMenuPlayer].enabled = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerNameBannerColour(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_NameBannerCfg[g_ExtMenuPlayer].enabled;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = NAMEBANNER_NUMCOLOURS;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)nameBannerColourName(data->dropdown.value);
	case MENUOP_SET:
		g_NameBannerCfg[g_ExtMenuPlayer].colour = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_NameBannerCfg[g_ExtMenuPlayer].colour;
		break;
	}

	return 0;
}

// The on-screen keyboard (as the Combat Simulator's Player Name)
static MenuItemHandlerResult menuhandlerNameBannerText(s32 operation, struct menuitem *item, union handlerdata *data)
{
	char *name = data->keyboard.string;
	char *text = g_NameBannerCfg[g_ExtMenuPlayer].text;
	s32 i;

	switch (operation) {
	case MENUOP_GETTEXT:
		for (i = 0; i < NAMEBANNER_MAXTEXT - 1 && text[i] != '\0'; i++) {
			name[i] = text[i];
		}

		while (i < NAMEBANNER_MAXTEXT) {
			name[i++] = '\0';
		}
		break;
	case MENUOP_SETTEXT:
		for (i = 0; i < NAMEBANNER_MAXTEXT - 1 && name[i] != '\0'; i++) {
			text[i] = name[i];
		}

		text[i] = '\0';

		while (i > 0 && text[i - 1] == ' ') {
			text[--i] = '\0';
		}
		break;
	}

	return 0;
}

static struct menuitem g_ExtendedNameBannerTextMenuItems[] = {
	{
		MENUITEMTYPE_KEYBOARD,
		NAMEBANNER_MAXTEXT - 1, // max length (0: the game's 10)
		0,
		0,
		0,
		menuhandlerNameBannerText,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_ExtendedNameBannerTextMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Banner Name",
	g_ExtendedNameBannerTextMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static struct menuitem g_ExtendedNameBannerMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Show Name Banner",
		0,
		menuhandlerNameBanner,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Colour",
		0,
		menuhandlerNameBannerColour,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_OPENSDIALOG,
		(uintptr_t)"Banner Name\n", // solo and co-op (the \n: a full line's height)
		(uintptr_t)&nameBannerMenuValue,
		(void *)&g_ExtendedNameBannerTextMenuDialog,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_ExtendedNameBannerMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Name Banner",
	g_ExtendedNameBannerMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

// MOD: body awareness - the player's own body in first person
static MenuItemHandlerResult menuhandlerBodyAwareness(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_BodyViewEnabled[g_ExtMenuPlayer];
	case MENUOP_SET:
		g_BodyViewEnabled[g_ExtMenuPlayer] = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerUseKeyReloads(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_PlayerExtCfg[g_ExtMenuPlayer].extcontrols;
	case MENUOP_GET:
		return g_PlayerExtCfg[g_ExtMenuPlayer].usereloads;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].usereloads = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrouchMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"Hold",
		"Analog",
		"Toggle",
		"Toggle + Analog"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].crouchmode = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_PlayerExtCfg[g_ExtMenuPlayer].crouchmode;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerFieldOfView(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].fovy + 0.5f;
		break;
	case MENUOP_SET:
		if (data->slider.value >= 15) {
			g_PlayerExtCfg[g_ExtMenuPlayer].fovy = data->slider.value;
			if (g_PlayerExtCfg[g_ExtMenuPlayer].fovzoom) {
				g_PlayerExtCfg[g_ExtMenuPlayer].fovzoommult = g_PlayerExtCfg[g_ExtMenuPlayer].fovy / 60.f;
				playerClampGunZoomFovY(g_ExtMenuPlayer);
			}
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairSway(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].crosshairsway * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshairsway = (f32)data->slider.value / 10.f;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairEdgeBoundary(s32 operation, struct menuitem* item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (s32)(g_PlayerExtCfg[g_ExtMenuPlayer].crosshairedgeboundary * 10.f + 0.5f);
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshairedgeboundary = (f32)data->slider.value / 10.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d", (s32)data->slider.value);
		break;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairR(s32 operation, struct menuitem* item, union handlerdata* data)
{
	u32 newColor;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour >> 24) & 0xFF;
		break;

	case MENUOP_SET:
		newColor = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour & 0xFFFFFF) | data->slider.value << 24;
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour = newColor;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairG(s32 operation, struct menuitem* item, union handlerdata* data)
{
	u32 newColor;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour >> 16) & 0xFF;
		break;

	case MENUOP_SET:
		newColor = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour & 0xFF00FFFF) | data->slider.value << 16;
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour = newColor;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairB(s32 operation, struct menuitem* item, union handlerdata* data)
{
	u32 newColor;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour >> 8) & 0xFF;
		break;

	case MENUOP_SET:
		newColor = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour & 0xFFFF00FF) | data->slider.value << 8;
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour = newColor;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairA(s32 operation, struct menuitem* item, union handlerdata* data)
{
	u32 newColor;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour & 0xFF;
		break;

	case MENUOP_SET:
		newColor = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour & 0xFFFFFF00) | data->slider.value;
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour = newColor;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairColorPreview(s32 operation, struct menuitem* item, union handlerdata* data)
{
	if (operation == MENUOP_GETCOLOUR) {
		data->label.colour1 = g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairSize(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].crosshairsize;
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshairsize = data->slider.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairHealth(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"Off",
		"On (Green)",
		"On (White)"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshairhealth = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_PlayerExtCfg[g_ExtMenuPlayer].crosshairhealth;
	}

	return 0;
}

struct menuitem g_ExtendedGameCrosshairColourMenuItems[] = {
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Red",
		255,
		menuhandlerCrosshairR,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Green",
		255,
		menuhandlerCrosshairG,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Blue",
		255,
		menuhandlerCrosshairB,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Alpha",
		255,
		menuhandlerCrosshairA,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_COLORBOX,
		0,
		0,
		0,
		0,
		menuhandlerCrosshairColorPreview,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedGameCrosshairColourMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Crosshair Colour",
	g_ExtendedGameCrosshairColourMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_ExtendedGameMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Crouch Mode",
		0,
		menuhandlerCrouchMode,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Vert FOV",
		170,
		menuhandlerFieldOfView,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Crosshair Sway",
		20,
		menuhandlerCrosshairSway,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Crosshair Edge Deadzone",
		10,
		menuhandlerCrosshairEdgeBoundary,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Crosshair Size",
		4,
		menuhandlerCrosshairSize,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_OPENSDIALOG,
		(uintptr_t)"Crosshair Colour\n",
		0,
		(void*)&g_ExtendedGameCrosshairColourMenuDialog,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Crosshair Colour by Health",
		0,
		menuhandlerCrosshairHealth,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Use Key Reloads",
		0,
		menuhandlerUseKeyReloads,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_OPENSDIALOG,
		(uintptr_t)"Name Banner\n", // MOD: name banner
		0,
		(void *)&g_ExtendedNameBannerMenuDialog,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Body Awareness", // MOD: body awareness
		0,
		menuhandlerBodyAwareness,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static char g_ExtendedGameMenuTitle[] = "Player 1 Game Options";
struct menudialogdef g_ExtendedGameMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)g_ExtendedGameMenuTitle,
	g_ExtendedGameMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerDoBind(s32 operation, struct menuitem *item, union handlerdata *data);

struct menuitem g_ExtendedBindKeyMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Press new key or button...\n",
		0,
		menuhandlerDoBind,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"ESC to cancel, DEL to remove binding\n",
		0,
		menuhandlerDoBind,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedBindKeyMenuDialog = {
	MENUDIALOGTYPE_SUCCESS,
	(uintptr_t)"Bind",
	g_ExtendedBindKeyMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

struct menubind {
	u32 ck;
	const char *name;
	const char *n64name;
};

static const struct menubind menuBinds[] = {
	{ CK_ZTRIG,  "Fire [ZT]\n",         "N64 Z Trigger\n" },
	{ CK_LTRIG,  "Fire Mode [LT]\n",    "N64 L Trigger\n"},
	{ CK_RTRIG,  "Aim Mode [RT]\n",     "N64 R Trigger\n" },
	{ CK_A,      "Use / Accept [A]\n",  "N64 A Button\n" },
	{ CK_B,      "Use / Cancel [B]\n",  "N64 B Button\n" },
	{ CK_START,  "Pause Menu [ST]\n",   "N64 Start\n" },
	{ CK_DPAD_U, "D-Pad Up [DU]\n",     "N64 D-Pad Up\n" },
	{ CK_DPAD_R, "D-Pad Right [DR]\n",  "N64 D-Pad Right\n" },
	{ CK_DPAD_L, "Prev Weapon [DL]\n",  "N64 D-Pad Left\n" },
	{ CK_DPAD_D, "Radial Menu [DD]\n",  "N64 D-Pad Down\n" },
	{ CK_C_U,    "Forward [CU]\n",      "N64 C-Up\n" },
	{ CK_C_D,    "Backward [CD]\n",     "N64 C-Down\n" },
	{ CK_C_R,    "Strafe Right [CR]\n", "N64 C-Right\n" },
	{ CK_C_L,    "Strafe Left [CL]\n",  "N64 C-Left\n" },
	{ CK_X,      "Reload [X]\n",        "N64 Ext X\n" },
	{ CK_Y,      "Next Weapon [Y]\n",   "N64 Ext Y\n" },
	{ CK_8000,   "Cycle Crouch [+]\n",  "N64 Ext 8000\n" },
	{ CK_4000,   "Half Crouch [+]\n",   "N64 Ext 4000\n" },
	{ CK_2000,   "Full Crouch [+]\n",   "N64 Ext 2000\n" },
	{ CK_ACCEPT, "UI Accept [+]\n",     "EXT UI Accept\n" },
	{ CK_CANCEL, "UI Cancel [+]\n",     "EXT UI Cancel\n" },
};

static const char *menutextBind(struct menuitem *item);
static MenuItemHandlerResult menuhandlerBind(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerResetBindsPC(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerResetBindsN64(s32 operation, struct menuitem *item, union handlerdata *data);

#define DEFINE_MENU_BIND() \
	{ \
		MENUITEMTYPE_DROPDOWN, \
		0, \
		0, \
		(uintptr_t)menutextBind, \
		0, \
		menuhandlerBind, \
	}

struct menuitem g_ExtendedBindsMenuItems[] = {
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Reset to PC Defaults\n",
		0,
		menuhandlerResetBindsPC,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Reset to N64 Defaults\n",
		0,
		menuhandlerResetBindsN64,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static MenuItemHandlerResult menuhandlerDoBind(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (!menuIsDialogOpen(&g_ExtendedBindKeyMenuDialog)) {
		return 0;
	}

	if (inputKeyPressed(VK_ESCAPE)) {
		menuPopDialog();
		return 0;
	}

	const s32 key = inputGetLastKey();
	if (key && key != VK_ESCAPE) {
		inputKeyBind(g_ExtMenuPlayer, g_BindContKey, g_BindIndex, (key == VK_DELETE ? 0 : key));
		menuPopDialog();
	}

	return 0;
}

static const char *menutextBind(struct menuitem *item)
{
	return g_PlayerExtCfg[g_ExtMenuPlayer].extcontrols ?
		menuBinds[item - g_ExtendedBindsMenuItems].name :
		menuBinds[item - g_ExtendedBindsMenuItems].n64name;
}

static MenuItemHandlerResult menuhandlerBind(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 idx = item - g_ExtendedBindsMenuItems;
	const u32 *binds;

	static char keyname[128];

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = INPUT_MAX_BINDS;
		break;
	case MENUOP_GETOPTIONTEXT:
		binds = inputKeyGetBinds(g_ExtMenuPlayer, menuBinds[idx].ck);
		if (binds && binds[data->dropdown.value]) {
			strncpy(keyname, inputGetKeyName(binds[data->dropdown.value]), sizeof(keyname) - 1);
			for (char *p = keyname; *p; ++p) {
				if (*p == '_') *p = ' ';
			}
			return (intptr_t)keyname;
		}
		return (intptr_t)"NONE";
	case MENUOP_SET:
		g_ExtendedBindKeyMenuItems[0].param2 = (uintptr_t)menuBinds[idx].name;
		g_BindIndex = data->dropdown.value;
		g_BindContKey = menuBinds[idx].ck;
		inputClearLastKey();
		menuPushDialog(&g_ExtendedBindKeyMenuDialog);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = 0;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerResetBindsPC(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		inputSetDefaultKeyBinds(g_ExtMenuPlayer, false);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerResetBindsN64(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		inputSetDefaultKeyBinds(g_ExtMenuPlayer, true);
	}

	return 0;
}

static char g_ExtendedBindsMenuTitle[] = "Player 1 Bindings";
struct menudialogdef g_ExtendedBindsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)g_ExtendedBindsMenuTitle,
	g_ExtendedBindsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS | MENUDIALOGFLAG_IGNOREBACK,
	NULL,
};

static MenuItemHandlerResult menuhandlerOpenControllerMenu(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_ExtNextDialog = &g_ExtendedControllerMenuDialog;
		menuPushDialog(&g_ExtendedSelectPlayerMenuDialog);
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerOpenGameMenu(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_ExtNextDialog = &g_ExtendedGameMenuDialog;
		menuPushDialog(&g_ExtendedSelectPlayerMenuDialog);
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerOpenBindsMenu(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_ExtNextDialog = &g_ExtendedBindsMenuDialog;
		menuPushDialog(&g_ExtendedSelectPlayerMenuDialog);
	}
	return 0;
}

#ifdef PD_CLIPCHECK
/* MOD: clipping checks - Extended > Physics > Debug: the player's collision (experimental) */
static MenuItemHandlerResult menuhandlerClipPlayerMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Vanilla", "Accurate", "Both" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t) opts[data->dropdown.value];
	case MENUOP_SET:
		g_ClipPlayerMode = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ClipPlayerMode;
		break;
	}

	return 0;
}

/* MOD: clipping checks - Extended > Physics > Debug > Collision View */
static MenuItemHandlerResult menuhandlerClipDebug(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Off", "Drawn Surfaces", "Game Collision", "Both" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t) opts[data->dropdown.value];
	case MENUOP_SET:
		g_ClipDebug = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ClipDebug;
		break;
	}

	return 0;
}
#endif

#ifdef PD_PHYSICS
/* MOD: ragdoll physics - Extended > Physics */

static MenuItemHandlerResult menuhandlerRagdolls(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_RagdollsEnabled;
	case MENUOP_SET:
		g_RagdollsEnabled = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

// MOD: ragdoll ranges - the low end of a ranged setting (each body rolls
// between this and the setting itself); raising it past the max raises the max
static MenuItemHandlerResult ragdollRangeMinSlider(s32 operation, union handlerdata *data, f32 *min, f32 *max, f32 base, bool disabled, const char *fmt)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return disabled;
	case MENUOP_GETSLIDER:
		data->slider.value = (*min - base) * 100.f + 0.5f;
		if (data->slider.value < 0) {
			data->slider.value = 0;
		}
		break;
	case MENUOP_SET:
		*min = base + (f32)data->slider.value / 100.f;
		if (*max < *min) {
			*max = *min;
		}
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, fmt, base + (f32)data->slider.value / 100.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollMuscleStrengthMin(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return ragdollRangeMinSlider(operation, data, &g_RagdollMuscleStrengthMin, &g_RagdollMuscleStrength, 0.0f, !g_RagdollsEnabled || !g_RagdollMuscles, "%.2fx");
}

static MenuItemHandlerResult menuhandlerRagdollMuscleTimeMin(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return ragdollRangeMinSlider(operation, data, &g_RagdollMuscleTimeMin, &g_RagdollMuscleTime, 0.1f, !g_RagdollsEnabled || !g_RagdollMuscles, "%.2f s");
}

static MenuItemHandlerResult menuhandlerRagdollReflexStrengthMin(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return ragdollRangeMinSlider(operation, data, &g_RagdollReflexStrengthMin, &g_RagdollReflexStrength, 0.0f, !g_RagdollsEnabled, "%.2fx");
}

static MenuItemHandlerResult menuhandlerRagdollPushScale(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GETSLIDER:
		data->slider.value = g_RagdollPushScale * 100.f + 0.5f;
		break;
	case MENUOP_SET:
		g_RagdollPushScale = (f32)data->slider.value / 100.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2fx", (f32)data->slider.value / 100.f);
		break;
	}

	return 0;
}

// MOD: combat simulator ragdolls - Max Bodies, solo/co-op and Combat
// Simulator. Slider: 0 = Vanilla (setting -1), 1-245 = 12-256, 246 = Unlimited (0)
#define RD_BODIESSLIDER_MAX (RD_MAXBODIES_LIMIT - RD_MINBODIES + 2)

static s32 rdMaxBodiesToSlider(s32 setting)
{
	if (setting < 0) {
		return 0;
	}

	if (setting == 0) {
		return RD_BODIESSLIDER_MAX;
	}

	return (setting < RD_MINBODIES ? RD_MINBODIES : setting) - RD_MINBODIES + 1;
}

static s32 rdMaxBodiesFromSlider(s32 value)
{
	return value <= 0 ? -1 : value >= RD_BODIESSLIDER_MAX ? 0 : value + RD_MINBODIES - 1;
}

static MenuItemHandlerResult rdMaxBodiesHandler(s32 *setting, s32 operation, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GETSLIDER:
		data->slider.value = rdMaxBodiesToSlider(*setting);
		break;
	case MENUOP_SET:
		*setting = rdMaxBodiesFromSlider(data->slider.value);
		break;
	case MENUOP_GETSLIDERLABEL:
		if (data->slider.value <= 0) {
			sprintf(data->slider.label, "Vanilla");
		} else if (data->slider.value >= RD_BODIESSLIDER_MAX) {
			sprintf(data->slider.label, "Unlimited");
		} else {
			sprintf(data->slider.label, "%d", rdMaxBodiesFromSlider(data->slider.value));
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollMaxBodies(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return rdMaxBodiesHandler(&g_RagdollMaxBodies, operation, data);
}

static MenuItemHandlerResult menuhandlerRagdollMpMaxBodies(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return rdMaxBodiesHandler(&g_RagdollMpMaxBodies, operation, data);
}

// MOD: combat simulator ragdolls - Resting Bodies Turn Solid
static MenuItemHandlerResult menuhandlerRagdollMpSolid(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Combat Simulator", "Both", "Off" };
	static const s32 values[] = { RD_SOLID_COMBATSIM, RD_SOLID_BOTH, RD_SOLID_OFF };
	s32 i;

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t) opts[data->dropdown.value];
	case MENUOP_SET:
		g_RagdollMpSolid = values[data->dropdown.value];
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = 0;

		for (i = 0; i < ARRAYCOUNT(values); i++) {
			if (values[i] == g_RagdollMpSolid) {
				data->dropdown.value = i;
			}
		}
		break;
	}

	return 0;
}

// MOD: combat simulator ragdolls - Max Active Bodies, solo/co-op and Combat
// Simulator. Slider: 0-244 = 12-256, 245 = Unlimited (setting 0)
#define RD_ACTIVESLIDER_MAX (RD_MAXBODIES_LIMIT - RD_MINACTIVE + 1)

static MenuItemHandlerResult rdMaxActiveHandler(s32 *setting, s32 operation, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GETSLIDER:
		data->slider.value = *setting <= 0 ? RD_ACTIVESLIDER_MAX : *setting < RD_MINACTIVE ? 0 : *setting - RD_MINACTIVE;
		break;
	case MENUOP_SET:
		*setting = data->slider.value >= RD_ACTIVESLIDER_MAX ? 0 : data->slider.value + RD_MINACTIVE;
		break;
	case MENUOP_GETSLIDERLABEL:
		if (data->slider.value >= RD_ACTIVESLIDER_MAX) {
			sprintf(data->slider.label, "Unlimited");
		} else {
			sprintf(data->slider.label, "%d", data->slider.value + RD_MINACTIVE);
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollMaxPhysical(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return rdMaxActiveHandler(&g_RagdollMaxPhysical, operation, data);
}

static MenuItemHandlerResult menuhandlerRagdollMpMaxPhysical(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return rdMaxActiveHandler(&g_RagdollMpMaxPhysical, operation, data);
}

static MenuItemHandlerResult menuhandlerRagdollChrPush(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GET:
		return g_RagdollChrPush;
	case MENUOP_SET:
		g_RagdollChrPush = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollGrab(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GET:
		return g_RagdollGrab;
	case MENUOP_SET:
		g_RagdollGrab = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollGrabStrength(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled || !g_RagdollGrab;
	case MENUOP_GETSLIDER:
		data->slider.value = (g_RagdollGrabStrength - 0.25f) * 100.f + 0.5f;
		break;
	case MENUOP_SET:
		g_RagdollGrabStrength = 0.25f + (f32)data->slider.value / 100.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2fx", 0.25f + (f32)data->slider.value / 100.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollMuscles(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GET:
		return g_RagdollMuscles;
	case MENUOP_SET:
		g_RagdollMuscles = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollMuscleStrength(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled || !g_RagdollMuscles;
	case MENUOP_GETSLIDER:
		data->slider.value = g_RagdollMuscleStrength * 100.f + 0.5f;
		break;
	case MENUOP_SET:
		g_RagdollMuscleStrength = (f32)data->slider.value / 100.f;
		if (g_RagdollMuscleStrengthMin > g_RagdollMuscleStrength) {
			g_RagdollMuscleStrengthMin = g_RagdollMuscleStrength; // MOD: ragdoll ranges
		}
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2fx", (f32)data->slider.value / 100.f);
		break;
	}

	return 0;
}

// Slider 0-990 = 0.10-10.00 seconds
static MenuItemHandlerResult menuhandlerRagdollMuscleTime(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled || !g_RagdollMuscles;
	case MENUOP_GETSLIDER:
		data->slider.value = (g_RagdollMuscleTime - 0.1f) * 100.f + 0.5f;
		if (data->slider.value < 0) {
			data->slider.value = 0;
		}
		break;
	case MENUOP_SET:
		g_RagdollMuscleTime = 0.1f + (f32)data->slider.value / 100.f;
		if (g_RagdollMuscleTimeMin > g_RagdollMuscleTime) {
			g_RagdollMuscleTimeMin = g_RagdollMuscleTime; // MOD: ragdoll ranges
		}
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2f s", 0.1f + (f32)data->slider.value / 100.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult ragdollPercentSlider(s32 operation, union handlerdata *data, s32 *value, bool disabled)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return disabled;
	case MENUOP_GETSLIDER:
		data->slider.value = *value;
		break;
	case MENUOP_SET:
		*value = data->slider.value;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d%%", data->slider.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollBraceChance(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return ragdollPercentSlider(operation, data, &g_RagdollReflexBraceChance, !g_RagdollsEnabled);
}

static MenuItemHandlerResult menuhandlerRagdollClutchChance(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return ragdollPercentSlider(operation, data, &g_RagdollReflexClutchChance, !g_RagdollsEnabled);
}

static MenuItemHandlerResult menuhandlerRagdollHeadshotLimp(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return ragdollPercentSlider(operation, data, &g_RagdollHeadshotLimpChance, !g_RagdollsEnabled);
}

// Of the no-reaction headshots, how many also take no push from the shot
static MenuItemHandlerResult menuhandlerRagdollHeadshotLifeless(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return ragdollPercentSlider(operation, data, &g_RagdollHeadshotLifelessChance, !g_RagdollsEnabled || g_RagdollHeadshotLimpChance <= 0);
}

// 0% off, 100% every death; in between, each death rolls: animation first, or straight to the ragdoll
static MenuItemHandlerResult menuhandlerRagdollAnimBlend(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return ragdollPercentSlider(operation, data, &g_RagdollAnimBlend, !g_RagdollsEnabled);
}

static MenuItemHandlerResult menuhandlerRagdollReflexStumble(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GET:
		return g_RagdollReflexStumble;
	case MENUOP_SET:
		g_RagdollReflexStumble = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollReflexHead(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GET:
		return g_RagdollReflexHead;
	case MENUOP_SET:
		g_RagdollReflexHead = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollReflexStrength(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GETSLIDER:
		data->slider.value = g_RagdollReflexStrength * 100.f + 0.5f;
		break;
	case MENUOP_SET:
		g_RagdollReflexStrength = (f32)data->slider.value / 100.f;
		if (g_RagdollReflexStrengthMin > g_RagdollReflexStrength) {
			g_RagdollReflexStrengthMin = g_RagdollReflexStrength; // MOD: ragdoll ranges
		}
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2fx", (f32)data->slider.value / 100.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollDeathCam(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GET:
		return g_RagdollDeathCam;
	case MENUOP_SET:
		g_RagdollDeathCam = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollDeathCamSmooth(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled || !g_RagdollDeathCam;
	case MENUOP_GETSLIDER:
		data->slider.value = g_RagdollDeathCamSmooth * 100.f + 0.5f;
		break;
	case MENUOP_SET:
		g_RagdollDeathCamSmooth = (f32)data->slider.value / 100.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d%%", data->slider.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollDeathCamBodyView(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled || !g_RagdollDeathCam;
	case MENUOP_GETSLIDER:
		data->slider.value = g_RagdollDeathCamBodyView * 100.f + 0.5f;
		break;
	case MENUOP_SET:
		g_RagdollDeathCamBodyView = (f32)data->slider.value / 100.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d%%", data->slider.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollDeathCamHold(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled || !g_RagdollDeathCam;
	case MENUOP_GET:
		return g_RagdollDeathCamHold;
	case MENUOP_SET:
		g_RagdollDeathCamHold = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollTestSpawnKey(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GET:
		return g_RagdollTestSpawnKey;
	case MENUOP_SET:
		g_RagdollTestSpawnKey = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerDismember(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GET:
		return g_RagdollDismember;
	case MENUOP_SET:
		g_RagdollDismember = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerDismemberChance(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return ragdollPercentSlider(operation, data, &g_RagdollDismemberChance, !g_RagdollsEnabled || !g_RagdollDismember);
}

// Slider 0-95 = damage 0.5-10.0
static MenuItemHandlerResult menuhandlerDismemberDamage(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled || !g_RagdollDismember;
	case MENUOP_GETSLIDER:
		data->slider.value = (g_RagdollDismemberDamage - 0.5f) * 10.f + 0.5f;
		if (data->slider.value < 0) {
			data->slider.value = 0;
		}
		break;
	case MENUOP_SET:
		g_RagdollDismemberDamage = 0.5f + (f32)data->slider.value / 10.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.1f", 0.5f + (f32)data->slider.value / 10.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerDismemberExplosions(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled || !g_RagdollDismember;
	case MENUOP_GET:
		return g_RagdollDismemberExplosions;
	case MENUOP_SET:
		g_RagdollDismemberExplosions = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerDismemberTorso(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled || !g_RagdollDismember;
	case MENUOP_GET:
		return g_RagdollDismemberTorso;
	case MENUOP_SET:
		g_RagdollDismemberTorso = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

struct menuitem g_ExtendedDismemberMenuItems[] = {
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Dismemberment", 0, menuhandlerDismember },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Explosions Dismember", 0, menuhandlerDismemberExplosions },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Torso Can Split", 0, menuhandlerDismemberTorso },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Dismemberment Chance", 100, menuhandlerDismemberChance },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Damage Needed", 95, menuhandlerDismemberDamage },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL }, // "Back"
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedDismemberMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Dismemberment Options",
	g_ExtendedDismemberMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerRagdollLedgeCatch(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GET:
		return g_RagdollLedgeCatch;
	case MENUOP_SET:
		g_RagdollLedgeCatch = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

struct menuitem g_ExtendedReactionsMenuItems[] = {
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reflex: Stumble", 0, menuhandlerRagdollReflexStumble },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reflex: Head Guard", 0, menuhandlerRagdollReflexHead },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reflex: Catch on Ledge", 0, menuhandlerRagdollLedgeCatch },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Reflex Strength Min", 300, menuhandlerRagdollReflexStrengthMin },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Reflex Strength Max", 300, menuhandlerRagdollReflexStrength },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Brace Fall Chance", 100, menuhandlerRagdollBraceChance },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Clutch Wound Chance", 100, menuhandlerRagdollClutchChance },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Headshot: No Reaction", 100, menuhandlerRagdollHeadshotLimp },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Headshot: Lifeless Drop", 100, menuhandlerRagdollHeadshotLifeless },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL }, // "Back"
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedReactionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Reaction Options",
	g_ExtendedReactionsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_ExtendedDeathCamMenuItems[] = {
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Immersive Death Camera", 0, menuhandlerRagdollDeathCam },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Manual Skip", 0, menuhandlerRagdollDeathCamHold },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Smoothing", 95, menuhandlerRagdollDeathCamSmooth },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Body Awareness", 100, menuhandlerRagdollDeathCamBodyView },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL }, // "Back"
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedDeathCamMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Death Camera Options",
	g_ExtendedDeathCamMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerRagdollBodyAlerts(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled;
	case MENUOP_GET:
		return g_RagdollBodyAlerts;
	case MENUOP_SET:
		g_RagdollBodyAlerts = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRagdollBodyCover(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_RagdollsEnabled || !g_RagdollGrab;
	case MENUOP_GET:
		return g_RagdollBodyCover;
	case MENUOP_SET:
		g_RagdollBodyCover = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

struct menuitem g_ExtendedInteractionMenuItems[] = {
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Characters Push Bodies", 0, menuhandlerRagdollChrPush },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Grab Bodies and Objects (Hold Use)", 0, menuhandlerRagdollGrab },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Held Bodies Stop Bullets", 0, menuhandlerRagdollBodyCover },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Guards React to Bodies", 0, menuhandlerRagdollBodyAlerts },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Push Strength", 1000, menuhandlerRagdollPushScale },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Grab Strength", 375, menuhandlerRagdollGrabStrength },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL }, // "Back"
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedInteractionMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Ragdoll Interactions",
	g_ExtendedInteractionMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_ExtendedBehaviourMenuItems[] = {
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Animation Blend", 100, menuhandlerRagdollAnimBlend },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Body Stiffness", 0, menuhandlerRagdollMuscles },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Stiffness Level Min", 300, menuhandlerRagdollMuscleStrengthMin },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Stiffness Level Max", 300, menuhandlerRagdollMuscleStrength },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Stiffness Fade Time Min", 990, menuhandlerRagdollMuscleTimeMin },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Stiffness Fade Time Max", 990, menuhandlerRagdollMuscleTime },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reactions...\n", 0, (void *)&g_ExtendedReactionsMenuDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Dismemberment...\n", 0, (void *)&g_ExtendedDismemberMenuDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Death Camera...\n", 0, (void *)&g_ExtendedDeathCamMenuDialog },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL }, // "Back"
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedBehaviourMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Ragdoll Behaviours",
	g_ExtendedBehaviourMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_ExtendedPhysicsDebugMenuItems[] = {
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"H Key Spawns Dummy", 0, menuhandlerRagdollTestSpawnKey },
#ifdef PD_CLIPCHECK
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Player Collision (Experimental)", 0, menuhandlerClipPlayerMode }, // MOD: clipping checks
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Collision View", 0, menuhandlerClipDebug }, // MOD: clipping checks
#endif
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL }, // "Back"
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedPhysicsDebugMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Physics Debug",
	g_ExtendedPhysicsDebugMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/* MOD: physics presets - Extended > Physics > Preset: Default puts every
   ragdoll/dismemberment setting back to its default; any change shows Custom */
static MenuItemHandlerResult menuhandlerRagdollPreset(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Default", "Custom" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t) opts[data->dropdown.value];
	case MENUOP_SET:
		ragdollPresetApply(data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = ragdollPresetGet();
		break;
	}

	return 0;
}

// MOD: menu layout - Extended > Physics > Performance
struct menuitem g_ExtendedPhysicsPerformanceMenuItems[] = {
	// MOD: combat simulator ragdolls - separate limits for solo/co-op and the Combat Simulator
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LABEL_ALTCOLOUR, (uintptr_t)"Singleplayer / Co-Op\n", 0, NULL },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Max Bodies", RD_BODIESSLIDER_MAX, menuhandlerRagdollMaxBodies },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Max Active Bodies", RD_ACTIVESLIDER_MAX, menuhandlerRagdollMaxPhysical },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LABEL_ALTCOLOUR, (uintptr_t)"Combat Simulator\n", 0, NULL },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Max Bodies", RD_BODIESSLIDER_MAX, menuhandlerRagdollMpMaxBodies },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Max Active Bodies", RD_ACTIVESLIDER_MAX, menuhandlerRagdollMpMaxPhysical },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Resting Bodies Turn Solid", 0, menuhandlerRagdollMpSolid },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL }, // "Back"
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedPhysicsPerformanceMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Physics Performance",
	g_ExtendedPhysicsPerformanceMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_ExtendedPhysicsMenuItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Preset", 0, menuhandlerRagdollPreset },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Ragdolls", 0, menuhandlerRagdolls },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Ragdoll Behaviours...\n", 0, (void *)&g_ExtendedBehaviourMenuDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Ragdoll Interactions...\n", 0, (void *)&g_ExtendedInteractionMenuDialog },
#ifdef PD_GRAVITY
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Gravity...\n", 0, (void *)&g_GravityMenuDialog }, // MOD: gravity
#endif
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Performance...\n", 0, (void *)&g_ExtendedPhysicsPerformanceMenuDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Debug...\n", 0, (void *)&g_ExtendedPhysicsDebugMenuDialog },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL }, // "Back"
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedPhysicsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Extended Physics Options",
	g_ExtendedPhysicsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
#endif

struct menuitem g_ExtendedMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Video\n",
		0,
		(void *)&g_ExtendedVideoMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Audio\n",
		0,
		(void *)&g_ExtendedAudioMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mouse\n",
		0,
		(void *)&g_ExtendedMouseMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Controller\n",
		0,
		menuhandlerOpenControllerMenu,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Game\n",
		0,
		menuhandlerOpenGameMenu,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Key Bindings\n",
		0,
		menuhandlerOpenBindsMenu,
	},
#ifdef PD_PHYSICS
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Physics\n",
		0,
		(void *)&g_ExtendedPhysicsMenuDialog, // MOD: ragdoll physics
	},
#elif defined(PD_GRAVITY)
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Gravity\n",
		0,
		(void *)&g_GravityMenuDialog, // MOD: gravity (in Physics when that's built)
	},
#endif
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Extended Options",
	g_ExtendedMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

void updateMaxAnisotropyLevel()
{
	for (int i = 0; i < ARRAYCOUNT(g_ExtendedVideoMenuItems); ++i) {
		struct menuitem *item = &g_ExtendedVideoMenuItems[i];
		const char *text = menuResolveParam2Text(item);
		
		if (text && strstr(text, "Anisotropic Filtering") != NULL) {
			item->param3 = videoGetMaxAnisotropyLevel();
			break;
		}
	}

}

void optionsMenuInit()
{
	updateMaxAnisotropyLevel();
}
