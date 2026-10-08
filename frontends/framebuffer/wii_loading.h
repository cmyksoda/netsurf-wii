#ifndef NETSURF_FRAMEBUFFER_WII_LOADING_H
#define NETSURF_FRAMEBUFFER_WII_LOADING_H

#include <stdbool.h>

/**
 * Load the loading screen; if the image cannot be read it is never shown.
 *
 * \param image_path A 640x480 PNG.
 * \param font_path The font for the status line.
 * \param width The width SDL will run at; the image is centered across it.
 */
void wii_loading_start(const char *image_path, const char *font_path,
		int width);

/**
 * Show the loading screen with a status line, centered under the logo.
 *
 * \param status The status, without trailing dots.
 * \param working Whether to animate dots after it.
 */
void wii_loading_status(const char *status, bool working);

/**
 * Keep showing the loading screen while SDL sets its video mode, which
 * would otherwise blank the screen.
 */
void wii_loading_hold(void);

/**
 * Release the loading screen once the browser draws its own screen.
 */
void wii_loading_finish(void);

#endif
