#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <png.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <SDL/SDL.h>
#include <gccore.h>
#include <ogc/cond.h>

#include "utils/utf8.h"
#include "framebuffer/wii_loading.h"

#define LOADING_WIDTH 640
#define LOADING_HEIGHT 480
#define LOADING_STATUS_BASELINE 330
#define LOADING_STATUS_PIXELS 22
/* the rows the status line can touch */
#define LOADING_STATUS_TOP (LOADING_STATUS_BASELINE - 26)
#define LOADING_STATUS_BOTTOM (LOADING_STATUS_BASELINE + 10)
#define LOADING_STATUS_MAX 64

#define LOADING_DOT_INTERVAL_MS 400
/* above the main thread (64) so the dots move while start-up is busy, and
 * below SDL-wii's presentation thread (68) */
#define LOADING_THREAD_PRIO 66
#define LOADING_THREAD_STACK (64 * 1024)

static uint8_t *loading_image; /**< loading.png as RGB */
static uint8_t *loading_frame; /**< the image with the status line on it */
static FT_Library loading_library;
static FT_Face loading_face;
static SDL_Surface *loading_screen; /**< set once SDL has a video mode */
static int loading_canvas_width; /**< SDL's width; the image is centred */
static bool loading_drawn; /**< the whole frame is on the current screen */
static void *loading_sdl_xfb; /**< SDL-wii's frame buffer */
static uint32_t *loading_held_xfb; /**< shown instead while SDL starts */

static char loading_text[LOADING_STATUS_MAX];
static bool loading_working; /**< animate dots after the text */
static unsigned int loading_dots;

static lwp_t loading_thread = LWP_THREAD_NULL;
static mutex_t loading_mutex = LWP_MUTEX_NULL;
static cond_t loading_cond = LWP_COND_NULL;
static bool loading_quit;

/* white over the frame, with the glyph's coverage as alpha */
static void
loading_blend(const FT_Bitmap *bitmap, int x0, int y0)
{
	unsigned int row;
	unsigned int col;

	if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY)
		return;

	for (row = 0; row < bitmap->rows; row++) {
		int y = y0 + (int)row;

		if (y < LOADING_STATUS_TOP || y >= LOADING_STATUS_BOTTOM)
			continue;

		for (col = 0; col < bitmap->width; col++) {
			int x = x0 + (int)col;
			unsigned int cover =
				bitmap->buffer[row * bitmap->pitch + col];
			uint8_t *p;
			int c;

			if (x < 0 || x >= LOADING_WIDTH || cover == 0)
				continue;

			p = loading_frame + ((y * LOADING_WIDTH) + x) * 3;
			for (c = 0; c < 3; c++)
				p[c] += ((255 - p[c]) * cover) / 255;
		}
	}
}

/* draw text from x and return the pen position after it */
static int
loading_draw_text(const char *text, int x, bool draw)
{
	size_t len = strlen(text);
	size_t offset;

	for (offset = 0; offset < len; offset = utf8_next(text, len, offset)) {
		FT_GlyphSlot slot;

		if (FT_Load_Char(loading_face,
				 utf8_to_ucs4(text + offset, len - offset),
				 draw ? FT_LOAD_RENDER : FT_LOAD_DEFAULT) != 0)
			continue;

		slot = loading_face->glyph;
		if (draw)
			loading_blend(&slot->bitmap, x + slot->bitmap_left,
				      LOADING_STATUS_BASELINE - slot->bitmap_top);
		x += slot->advance.x >> 6;
	}

	return x;
}

static void
loading_render(void)
{
	static const char dots[] = "...";
	int x;

	memcpy(loading_frame + LOADING_STATUS_TOP * LOADING_WIDTH * 3,
	       loading_image + LOADING_STATUS_TOP * LOADING_WIDTH * 3,
	       (LOADING_STATUS_BOTTOM - LOADING_STATUS_TOP) * LOADING_WIDTH * 3);

	if (loading_face == NULL)
		return;

	/* centre the text alone so the dots do not shift it */
	x = (LOADING_WIDTH - loading_draw_text(loading_text, 0, false)) / 2;
	x = loading_draw_text(loading_text, x, true);
	if (loading_working)
		loading_draw_text(dots + 3 - loading_dots, x, true);
}

static int
loading_clamp(int v)
{
	return (v < 0) ? 0 : (v > 255) ? 255 : v;
}

/* two pixels as the video interface's Y'CbCr 4:2:2 word, Y1 Cb Y2 Cr */
static uint32_t
loading_yuv_pair(const uint8_t *a, const uint8_t *b)
{
	int r = a[0] + b[0];
	int g = a[1] + b[1];
	int bl = a[2] + b[2];
	int y1 = (66 * a[0] + 129 * a[1] + 25 * a[2] + 4224) >> 8;
	int y2 = (66 * b[0] + 129 * b[1] + 25 * b[2] + 4224) >> 8;
	int cb = (-38 * r - 74 * g + 112 * bl + 65792) >> 9;
	int cr = (112 * r - 94 * g - 18 * bl + 65792) >> 9;

	return ((uint32_t)loading_clamp(y1) << 24) |
	       ((uint32_t)loading_clamp(cb) << 16) |
	       ((uint32_t)loading_clamp(y2) << 8) |
	       (uint32_t)loading_clamp(cr);
}

static GXRModeObj *
loading_video_mode(void)
{
	GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);

	/* SDL-wii swaps this mode for its scaled version, as here */
	if (mode == &TVPal528IntDf)
		mode = &TVPal576IntDfScale;

	return mode;
}

/* the image pixel at x across a canvas of the given width, with the image
 * centred on it and its edge colours carried out to the sides */
static const uint8_t *
loading_canvas_pixel(const uint8_t *row, int x, int width)
{
	x -= (width - LOADING_WIDTH) / 2;
	if (x < 0)
		x = 0;
	else if (x >= LOADING_WIDTH)
		x = LOADING_WIDTH - 1;

	return row + x * 3;
}

/* What the video interface shows at x: the canvas squeezed to the frame
 * buffer's 640 pixels, as SDL-wii has the GPU do for a wider mode. */
static void
loading_xfb_pixel(const uint8_t *row, unsigned int x, uint8_t *rgb)
{
	unsigned int pos = x * loading_canvas_width * 16 / LOADING_WIDTH;
	const uint8_t *a = loading_canvas_pixel(row, pos >> 4,
						loading_canvas_width);
	const uint8_t *b = loading_canvas_pixel(row, (pos >> 4) + 1,
						loading_canvas_width);
	unsigned int f = pos & 15;
	int c;

	for (c = 0; c < 3; c++)
		rgb[c] = (a[c] * (16 - f) + b[c] * f) / 16;
}

/* Before SDL sets a video mode, draw straight into the frame buffer that
 * the video interface is displaying. */
static void
loading_show_xfb(int top, int bottom)
{
	GXRModeObj *mode = loading_video_mode();
	void *current = (loading_held_xfb != NULL) ?
		(void *)loading_held_xfb : VIDEO_GetCurrentFramebuffer();
	uint32_t *xfb;
	unsigned int y;
	unsigned int x;

	if (mode == NULL || current == NULL)
		return;

	xfb = MEM_PHYSICAL_TO_K0(MEM_VIRTUAL_TO_PHYSICAL(current));
	for (y = 0; y < mode->xfbHeight; y++) {
		int source = y * LOADING_HEIGHT / mode->xfbHeight;
		const uint8_t *row = loading_frame + source * LOADING_WIDTH * 3;
		uint32_t *out = xfb + (y * mode->fbWidth / 2);

		if (source < top || source >= bottom)
			continue;

		for (x = 0; x + 1 < mode->fbWidth && x < LOADING_WIDTH; x += 2) {
			uint8_t a[3];
			uint8_t b[3];

			loading_xfb_pixel(row, x, a);
			loading_xfb_pixel(row, x + 1, b);
			*out++ = loading_yuv_pair(a, b);
		}

		DCFlushRange(xfb + (y * mode->fbWidth / 2),
			     mode->fbWidth * VI_DISPLAY_PIX_SZ);
	}
}

static void
loading_show_sdl(int top, int bottom)
{
	int y;
	int x;

	if (loading_screen->w < LOADING_WIDTH ||
	    loading_screen->h != LOADING_HEIGHT ||
	    loading_screen->format->BytesPerPixel != 4)
		return;

	for (y = top; y < bottom; y++) {
		const uint8_t *row = loading_frame + y * LOADING_WIDTH * 3;
		uint32_t *out = (uint32_t *)(void *)
			((uint8_t *)loading_screen->pixels +
			 y * loading_screen->pitch);

		for (x = 0; x < loading_screen->w; x++) {
			const uint8_t *p = loading_canvas_pixel(row, x,
							       loading_screen->w);

			/* bytes A, R, G, B are what SDL-wii's texture
			 * upload reads */
			out[x] = 0xff000000u | (p[0] << 16) | (p[1] << 8) | p[2];
		}
	}

	SDL_UpdateRect(loading_screen, 0, top, loading_screen->w, bottom - top);
}

/* whether SDL-wii's frame buffer shows the loading screen yet, by its
 * corners, which also matches when they are black anyway */
static bool
loading_sdl_xfb_shows_frame(void)
{
	static const int points[][2] = {
		{ 16, 16 }, { 623, 16 }, { 16, 463 }, { 623, 463 },
	};
	GXRModeObj *mode = loading_video_mode();
	uint32_t *xfb;
	unsigned int point;

	if (mode == NULL || loading_sdl_xfb == NULL)
		return true;

	xfb = MEM_PHYSICAL_TO_K1(MEM_VIRTUAL_TO_PHYSICAL(loading_sdl_xfb));
	for (point = 0; point < sizeof(points) / sizeof(points[0]); point++) {
		int x = points[point][0] & ~1;
		int y = points[point][1];
		uint8_t rgb[3];
		int expect;

		loading_xfb_pixel(loading_frame + y * LOADING_WIDTH * 3, x, rgb);
		expect = (66 * rgb[0] + 129 * rgb[1] + 25 * rgb[2] + 4224) >> 8;
		uint32_t word = xfb[(y * mode->xfbHeight / LOADING_HEIGHT) *
				    mode->fbWidth / 2 + x / 2];
		int luma = word >> 24;

		if (luma < expect - 24 || luma > expect + 24)
			return false;
	}

	return true;
}

/* Wake SDL-wii's thread each frame until its frame buffer has the loading
 * screen, then display that buffer again; up to a second. */
static void
loading_hand_over_to_sdl(void)
{
	unsigned int frame;

	for (frame = 0; frame < 60; frame++) {
		SDL_UpdateRect(loading_screen, 0, 0, 1, 1);
		VIDEO_WaitVSync();
		if (loading_sdl_xfb_shows_frame())
			break;
	}

	if (loading_held_xfb != NULL) {
		VIDEO_SetNextFramebuffer(loading_sdl_xfb);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		VIDEO_WaitVSync();
		free(loading_held_xfb);
		loading_held_xfb = NULL;
	}
}

static void
loading_show(int top, int bottom)
{
	if (loading_screen != NULL)
		loading_show_sdl(top, bottom);
	else
		loading_show_xfb(top, bottom);
}

static void *
loading_animate(void *arg)
{
	struct timespec interval = {
		.tv_sec = 0,
		.tv_nsec = LOADING_DOT_INTERVAL_MS * 1000000L,
	};

	(void)arg;

	LWP_MutexLock(loading_mutex);
	while (!loading_quit) {
		LWP_CondTimedWait(loading_cond, loading_mutex, &interval);
		if (loading_quit || !loading_working)
			continue;

		loading_dots = (loading_dots % 3) + 1;
		loading_render();
		loading_show(LOADING_STATUS_TOP, LOADING_STATUS_BOTTOM);
	}
	LWP_MutexUnlock(loading_mutex);

	return NULL;
}

/* exported function documented in framebuffer/wii_loading.h */
void
wii_loading_start(const char *image_path, const char *font_path, int width)
{
	png_image png;

	loading_canvas_width = (width > LOADING_WIDTH) ? width : LOADING_WIDTH;

	/* Starting SDL's video replaces and blanks the frame buffer on screen,
	 * so do it before anything is drawn there; SDL_Init() skips it later. */
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
		return;

	memset(&png, 0, sizeof(png));
	png.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&png, image_path))
		return;

	png.format = PNG_FORMAT_RGB;
	if (png.width != LOADING_WIDTH || png.height != LOADING_HEIGHT) {
		png_image_free(&png);
		return;
	}

	loading_image = malloc(PNG_IMAGE_SIZE(png));
	loading_frame = malloc(PNG_IMAGE_SIZE(png));
	if (loading_image == NULL || loading_frame == NULL ||
	    !png_image_finish_read(&png, NULL, loading_image, 0, NULL)) {
		png_image_free(&png);
		wii_loading_finish();
		return;
	}
	png_image_free(&png);
	memcpy(loading_frame, loading_image, LOADING_WIDTH * LOADING_HEIGHT * 3);

	if (FT_Init_FreeType(&loading_library) != 0) {
		loading_library = NULL;
	} else if (FT_New_Face(loading_library, font_path, 0,
			       &loading_face) != 0) {
		loading_face = NULL;
	} else {
		FT_Set_Pixel_Sizes(loading_face, 0, LOADING_STATUS_PIXELS);
	}

	if (LWP_MutexInit(&loading_mutex, false) != 0 ||
	    LWP_CondInit(&loading_cond) != 0 ||
	    LWP_CreateThread(&loading_thread, loading_animate, NULL, NULL,
			     LOADING_THREAD_STACK, LOADING_THREAD_PRIO) != 0)
		loading_thread = LWP_THREAD_NULL;
}

/* exported function documented in framebuffer/wii_loading.h */
void
wii_loading_status(const char *status, bool working)
{
	SDL_Surface *screen;

	if (loading_image == NULL)
		return;

	if (loading_mutex != LWP_MUTEX_NULL)
		LWP_MutexLock(loading_mutex);

	strncpy(loading_text, status, sizeof(loading_text) - 1);
	loading_working = working;
	loading_dots = 1;
	loading_render();

	screen = SDL_GetVideoSurface();
	if (screen != loading_screen) {
		loading_screen = screen;
		loading_drawn = false;
	}

	if (loading_drawn) {
		loading_show(LOADING_STATUS_TOP, LOADING_STATUS_BOTTOM);
	} else {
		loading_show(0, LOADING_HEIGHT);
		loading_drawn = true;
		if (loading_screen != NULL)
			loading_hand_over_to_sdl();
	}

	if (loading_mutex != LWP_MUTEX_NULL)
		LWP_MutexUnlock(loading_mutex);
}

/* exported function documented in framebuffer/wii_loading.h */
void
wii_loading_hold(void)
{
	GXRModeObj *mode = loading_video_mode();

	if (loading_image == NULL || mode == NULL)
		return;

	LWP_MutexLock(loading_mutex);
	loading_sdl_xfb = VIDEO_GetCurrentFramebuffer();
	loading_held_xfb = SYS_AllocateFramebuffer(mode);
	if (loading_sdl_xfb != NULL && loading_held_xfb != NULL) {
		loading_show_xfb(0, LOADING_HEIGHT);
		VIDEO_SetNextFramebuffer(loading_held_xfb);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		VIDEO_WaitVSync();
		/* so only SDL drawing the loading screen there can match */
		VIDEO_ClearFrameBuffer(mode, MEM_PHYSICAL_TO_K1(
				MEM_VIRTUAL_TO_PHYSICAL(loading_sdl_xfb)),
				COLOR_BLACK);
	} else {
		free(loading_held_xfb);
		loading_held_xfb = NULL;
	}
	LWP_MutexUnlock(loading_mutex);
}

/* exported function documented in framebuffer/wii_loading.h */
void
wii_loading_finish(void)
{
	if (loading_thread != LWP_THREAD_NULL) {
		LWP_MutexLock(loading_mutex);
		loading_quit = true;
		LWP_CondSignal(loading_cond);
		LWP_MutexUnlock(loading_mutex);
		LWP_JoinThread(loading_thread, NULL);
		loading_thread = LWP_THREAD_NULL;
	}
	if (loading_held_xfb != NULL) {
		VIDEO_SetNextFramebuffer(loading_sdl_xfb);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		free(loading_held_xfb);
		loading_held_xfb = NULL;
	}
	if (loading_cond != LWP_COND_NULL) {
		LWP_CondDestroy(loading_cond);
		loading_cond = LWP_COND_NULL;
	}
	if (loading_mutex != LWP_MUTEX_NULL) {
		LWP_MutexDestroy(loading_mutex);
		loading_mutex = LWP_MUTEX_NULL;
	}
	if (loading_face != NULL) {
		FT_Done_Face(loading_face);
		loading_face = NULL;
	}
	if (loading_library != NULL) {
		FT_Done_FreeType(loading_library);
		loading_library = NULL;
	}
	free(loading_frame);
	loading_frame = NULL;
	free(loading_image);
	loading_image = NULL;
	loading_screen = NULL;
	loading_drawn = false;
}
