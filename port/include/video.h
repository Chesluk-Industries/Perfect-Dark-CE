#ifndef _IN_VIDEO_H
#define _IN_VIDEO_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

// maximum framerate; if the game runs faster than this, things will break
#if PAL
#define VIDEO_MAX_FPS 200
#else
#define VIDEO_MAX_FPS 240
#endif

typedef struct {
	s32 width;
	s32 height;
} displaymode;

s32 videoInit(void);
void videoStartFrame(void);
void videoSubmitCommands(Gfx *cmds);
void videoClearScreen(void);
void videoEndFrame(void);

void *videoGetWindowHandle(void);

void videoUpdateNativeResolution(s32 w, s32 h);
s32 videoGetNativeWidth(void);
s32 videoGetNativeHeight(void);

s32 videoGetWidth(void);
s32 videoGetHeight(void);
f32 videoGetAspect(void);
s32 videoGetFullscreen(void);
s32 videoGetFullscreenMode(void);
s32 videoGetMaximizeWindow(void);
void videoSetMaximizeWindow(s32 fs);
s32 videoGetCenterWindow(void);
void videoSetCenterWindow(s32 center);
u32 videoGetTextureFilter(void);
s32 videoGetTextureFilter2D(void);
u32 videoGetAnisotropicFilter(void);
u32 videoGetMaxAnisotropyLevel(void);
s32 videoGetDetailTextures(void);
s32 videoGetDisplayModeIndex(void);
s32 videoGetDisplayMode(displaymode *out, const s32 index);
s32 videoGetNumDisplayModes(void);
s32 videoGetVsync(void);
s32 videoGetFramerateLimit(void);
s32 videoGetDisplayFPS(void);
s32 videoGetMSAA(void);
f32 videoGetGlareBrightness(void);
f32 videoGetOverexposureScale(void);

f32 videoGetAverageFPS(void);

void videoSetWindowOffset(s32 x, s32 y);
void videoSetFullscreen(s32 fs);
void videoSetFullscreenMode(s32 mode);
void videoSetTextureFilter(u32 filter);
void videoSetTextureFilter2D(s32 filter);
void videoSetAnisotropicFilter(u32 filter);
void videoSetDetailTextures(s32 detail);
void videoSetDisplayMode(const s32 index);
void videoSetVsync(const s32 vsync);
void videoSetFramerateLimit(const s32 limit);
void videoSetDisplayFPS(const s32 displayfps);
void videoSetMSAA(const s32 msaa);
void videoSetGlareBrightness(f32 bright);
void videoSetOverexposureScale(f32 scale);

s32 videoCreateFramebuffer(u32 w, u32 h, s32 upscale, s32 autoresize);
void videoSetFramebuffer(s32 target);
void videoResetFramebuffer(void);
void videoCopyFramebuffer(s32 dst, s32 src, s32 left, s32 top);
void videoResizeFramebuffer(s32 target, u32 w, u32 h, s32 upscale, s32 autoresize);
s32 videoFramebuffersSupported(void);

void videoResetTextureCache(void);
void videoFreeCachedTexture(const void *texptr);
void videoFreeCachedTextures(const void *start, const void *end);

void videoShutdown(void);

// MOD: no limit on bullet holes/blood - Video.UnlimitedDecals (video.c,
// used by wallhitreset.c)
extern s32 g_UnlimitedDecals;

// Its extra per-frame display list / vertex buffer bytes (gfxmemory.c), x2 buffers
#define DECALS_EXTRA_GFX (2048 * 1024)
#define DECALS_EXTRA_VTX (512 * 1024)

// MOD: extra memp heap (MB) added to Game.MemorySize (main.c): the decals'
// budgets, and the ragdoll module's corpse budgets when it's built in
#define EXTRA_MEMP_MB 16

#endif
