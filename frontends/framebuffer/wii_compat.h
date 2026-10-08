#ifndef NETSURF_FRAMEBUFFER_WII_COMPAT_H
#define NETSURF_FRAMEBUFFER_WII_COMPAT_H

struct gui_file_table;

struct gui_file_table *wii_get_file_table(void);

/**
 * The width to run SDL at: 848 when the Wii is set to 16:9, else 640.
 */
int wii_screen_width(void);

#endif
