/*
 * Copyright 2010 Vincent Sanders <vince@simtec.co.uk>
 *
 * Framebuffer windowing toolkit on screen keyboard.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <limits.h>

#include <libnsfb.h>
#include <libnsfb_plot.h>
#include <libnsfb_event.h>
#include <libnsfb_cursor.h>

#include "utils/log.h"
#include "netsurf/browser_window.h"

#include "framebuffer/gui.h"
#include "framebuffer/fbtk.h"
#include "framebuffer/image_data.h"

#include "widget.h"

struct kbd_button_s {
	int x;
	int y;
	int w;
	int h;
	const char *t;
	enum nsfb_key_code_e keycode;
	const char *st; /**< label while shift is latched */
	int shift_keycode; /**< may be a character with no NSFB_KEY_ name */
};

/* Shift is latched for one key rather than held, since a pointer cannot hold
 * one key while pressing another. Printable keys send finished characters,
 * so the result does not depend on fbtk's own (UK) shift table. */
#define K(x, y, w, t, code, st, scode) \
	{ (x), (y), (w), 18, (t), (code), (st), (scode) }
#define LETTER(x, y, l, u) \
	K((x), (y), 20, #l, NSFB_KEY_##l, #u, NSFB_KEY_##l - ('a' - 'A'))

static struct kbd_button_s kbdbase[] = {
	K(  0,  0, 20, "`", NSFB_KEY_BACKQUOTE, "~", '~'),
	K( 20,  0, 20, "1", NSFB_KEY_1, "!", '!'),
	K( 40,  0, 20, "2", NSFB_KEY_2, "@", '@'),
	K( 60,  0, 20, "3", NSFB_KEY_3, "#", '#'),
	K( 80,  0, 20, "4", NSFB_KEY_4, "$", '$'),
	K(100,  0, 20, "5", NSFB_KEY_5, "%", '%'),
	K(120,  0, 20, "6", NSFB_KEY_6, "^", '^'),
	K(140,  0, 20, "7", NSFB_KEY_7, "&", '&'),
	K(160,  0, 20, "8", NSFB_KEY_8, "*", '*'),
	K(180,  0, 20, "9", NSFB_KEY_9, "(", '('),
	K(200,  0, 20, "0", NSFB_KEY_0, ")", ')'),
	K(220,  0, 20, "-", NSFB_KEY_MINUS, "_", '_'),
	K(240,  0, 20, "=", NSFB_KEY_EQUALS, "+", '+'),
	K(260,  0, 40, "Bksp", NSFB_KEY_BACKSPACE, "Bksp", NSFB_KEY_BACKSPACE),

	K(  0, 18, 30, "Tab", NSFB_KEY_TAB, "Tab", NSFB_KEY_TAB),
	LETTER( 30, 18, q, Q),
	LETTER( 50, 18, w, W),
	LETTER( 70, 18, e, E),
	LETTER( 90, 18, r, R),
	LETTER(110, 18, t, T),
	LETTER(130, 18, y, Y),
	LETTER(150, 18, u, U),
	LETTER(170, 18, i, I),
	LETTER(190, 18, o, O),
	LETTER(210, 18, p, P),
	K(230, 18, 20, "[", NSFB_KEY_LEFTBRACKET, "{", '{'),
	K(250, 18, 20, "]", NSFB_KEY_RIGHTBRACKET, "}", '}'),
	K(270, 18, 30, "\\", NSFB_KEY_BACKSLASH, "|", '|'),

	K(  0, 36, 35, "Caps", NSFB_KEY_CAPSLOCK, "CAPS", NSFB_KEY_CAPSLOCK),
	LETTER( 35, 36, a, A),
	LETTER( 55, 36, s, S),
	LETTER( 75, 36, d, D),
	LETTER( 95, 36, f, F),
	LETTER(115, 36, g, G),
	LETTER(135, 36, h, H),
	LETTER(155, 36, j, J),
	LETTER(175, 36, k, K),
	LETTER(195, 36, l, L),
	K(215, 36, 20, ";", NSFB_KEY_SEMICOLON, ":", ':'),
	K(235, 36, 20, "'", NSFB_KEY_QUOTE, "\"", '"'),
	K(255, 36, 45, "Enter", NSFB_KEY_RETURN, "Enter", NSFB_KEY_RETURN),

	K(  0, 54, 45, "Shift", NSFB_KEY_LSHIFT, "SHIFT", NSFB_KEY_LSHIFT),
	LETTER( 45, 54, z, Z),
	LETTER( 65, 54, x, X),
	LETTER( 85, 54, c, C),
	LETTER(105, 54, v, V),
	LETTER(125, 54, b, B),
	LETTER(145, 54, n, N),
	LETTER(165, 54, m, M),
	K(185, 54, 20, ",", NSFB_KEY_COMMA, "<", '<'),
	K(205, 54, 20, ".", NSFB_KEY_PERIOD, ">", '>'),
	K(225, 54, 20, "/", NSFB_KEY_SLASH, "?", '?'),
	K(245, 54, 55, "Shift", NSFB_KEY_LSHIFT, "SHIFT", NSFB_KEY_LSHIFT),

	K( 40, 72, 180, "", NSFB_KEY_SPACE, "", NSFB_KEY_SPACE),
	K(220, 72, 20, "\xe2\x86\x90", NSFB_KEY_LEFT, "\xe2\x86\x90", NSFB_KEY_LEFT),
	K(240, 72, 20, "\xe2\x86\x91", NSFB_KEY_UP, "\xe2\x86\x91", NSFB_KEY_UP),
	K(260, 72, 20, "\xe2\x86\x93", NSFB_KEY_DOWN, "\xe2\x86\x93", NSFB_KEY_DOWN),
	K(280, 72, 20, "\xe2\x86\x92", NSFB_KEY_RIGHT, "\xe2\x86\x92", NSFB_KEY_RIGHT),
};

#define KEYCOUNT (sizeof(kbdbase) / sizeof(kbdbase[0]))

/* dark gap around every key, so each reads as its own key on a TV */
#define OSK_KEY_GAP 2

static fbtk_widget_t *osk;
static fbtk_widget_t *osk_keys[KEYCOUNT];
static bool osk_shifted;
static bool osk_caps;
static unsigned int osk_held_shift; /**< physical Shift keys held, as bits */
static void (*osk_changed)(void);

/* Caps Lock applies to letters only, and a latched Shift reverses it. */
static bool
osk_key_shifted(const struct kbd_button_s *key)
{
	bool letter = key->keycode >= NSFB_KEY_a && key->keycode <= NSFB_KEY_z;

	if (key->keycode == NSFB_KEY_CAPSLOCK)
		return osk_caps;
	return (osk_shifted || osk_held_shift != 0) != (osk_caps && letter);
}

static void
osk_update_labels(void)
{
	unsigned int kloop;

	for (kloop = 0; kloop < KEYCOUNT; kloop++) {
		fbtk_set_text(osk_keys[kloop],
			      osk_key_shifted(&kbdbase[kloop]) ?
			      kbdbase[kloop].st : kbdbase[kloop].t);
	}
}

static void
osk_set_mapped(bool mapped)
{
	if (osk == NULL)
		return;

	if (!mapped && osk_shifted) {
		osk_shifted = false;
		osk_update_labels();
	}

	if (mapped) {
		fbtk_set_zorder(osk, INT_MIN);
	}
	fbtk_set_mapping(osk, mapped);

	if (osk_changed != NULL)
		osk_changed();
}

static int
osk_close(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	if (cbi->event->type != NSFB_EVENT_KEY_UP ||
	    cbi->event->value.keycode != NSFB_KEY_MOUSE_1)
		return 0;

	osk_set_mapped(false);

	return 0;
}

static int
osk_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	nsfb_event_t event;
	struct kbd_button_s *kbd_button = cbi->context;

	/* Only the primary button types; a wheel over the keyboard must not. */
	if (cbi->event->value.keycode != NSFB_KEY_MOUSE_1)
		return 0;

	if (kbd_button->keycode == NSFB_KEY_LSHIFT ||
	    kbd_button->keycode == NSFB_KEY_CAPSLOCK) {
		/* toggle on the press so the labels change sooner */
		if (cbi->event->type == NSFB_EVENT_KEY_DOWN) {
			if (kbd_button->keycode == NSFB_KEY_LSHIFT)
				osk_shifted = !osk_shifted;
			else
				osk_caps = !osk_caps;
			osk_update_labels();
		}
		return 0;
	}

	event.type = cbi->event->type;
	event.value.keycode = osk_key_shifted(kbd_button) ?
		(enum nsfb_key_code_e)kbd_button->shift_keycode :
		kbd_button->keycode;
	if ((event.value.keycode >= ' ') && (event.value.keycode < 0x7f))
		event.value.keycode += NSFB_KEY_CHARACTER;
	fbtk_input(widget, &event);

	if (cbi->event->type == NSFB_EVENT_KEY_UP) {
		if (osk_shifted) {
			osk_shifted = false;
			osk_update_labels();
		}
		if (kbd_button->keycode == NSFB_KEY_RETURN)
			osk_set_mapped(false);
	}

	return 0;
}

/* Every key is cut from one shared grid and inset by half the gap, so the
 * same dark gap separates all of them and the keyboard's outer edge. */
static fbtk_widget_t *
osk_create_key(int x, int y, int w, int h, int xscale, int yscale, int units,
	       fbtk_callback click, void *context)
{
	int x0 = OSK_KEY_GAP / 2 + (x * xscale) / units;
	int x1 = OSK_KEY_GAP / 2 + ((x + w) * xscale) / units;
	int y0 = OSK_KEY_GAP / 2 + (y * yscale) / units;
	int y1 = OSK_KEY_GAP / 2 + ((y + h) * yscale) / units;

	return fbtk_create_text_button(osk,
				       x0 + OSK_KEY_GAP / 2,
				       y0 + OSK_KEY_GAP / 2,
				       x1 - x0 - OSK_KEY_GAP,
				       y1 - y0 - OSK_KEY_GAP,
				       FB_FRAME_COLOUR,
				       FB_COLOUR_BLACK,
				       click,
				       context);
}

/* exported function documented in fbtk.h */
void 
fbtk_enable_oskb(fbtk_widget_t *fbtk)
{
	fbtk_widget_t *widget;
	unsigned int kloop;
	int maxx = 0;
	int maxy = 0;
	int xscale;
	int yscale;
	int wh;
	fbtk_widget_t *root = fbtk_get_root_widget(fbtk);

	for (kloop=0; kloop < KEYCOUNT; kloop++) {
		if ((kbdbase[kloop].x + kbdbase[kloop].w) > maxx)
			maxx=kbdbase[kloop].x + kbdbase[kloop].w;
		if ((kbdbase[kloop].y + kbdbase[kloop].h) > maxy)
			maxy=kbdbase[kloop].y + kbdbase[kloop].h;
	}

	/* keys keep their 4:3 height on a wider screen, rather than growing
	 * to fill half of it */
	xscale = fbtk_get_width(root) - OSK_KEY_GAP;
	yscale = fbtk_get_height(root) * 4 / 3;
	if (yscale > fbtk_get_width(root))
		yscale = fbtk_get_width(root);
	yscale -= OSK_KEY_GAP;

	/* scale window height apropriately */
	wh = (maxy * yscale) / maxx + OSK_KEY_GAP;

	osk = fbtk_create_window(root, 0, fbtk_get_height(root) - wh, 0, wh, 0xff202020);

	for (kloop=0; kloop < KEYCOUNT; kloop++) {
		widget = osk_create_key(kbdbase[kloop].x, kbdbase[kloop].y,
					kbdbase[kloop].w, kbdbase[kloop].h,
					xscale, yscale, maxx,
					osk_click, &kbdbase[kloop]);
		fbtk_set_text(widget, kbdbase[kloop].t);
		osk_keys[kloop] = widget;
	}

	widget = osk_create_key(0, 72, 40, 18, xscale, yscale, maxx,
				osk_close, NULL);
	fbtk_set_text(widget, "Hide");
}

/* exported function documented in fbtk.h */
void 
map_osk(void)
{
	/* callers can ask on both press and release; repaint only once */
	if ((osk == NULL) || (osk->mapped && (osk->prev == NULL)))
		return;

	osk_set_mapped(true);
}

/* exported function documented in fbtk.h */
void
unmap_osk(void)
{
	if ((osk != NULL) && osk->mapped)
		osk_set_mapped(false);
}

/* exported function documented in fbtk.h */
void
fbtk_osk_key_event(nsfb_event_t *event)
{
	bool down = (event->type == NSFB_EVENT_KEY_DOWN);
	unsigned int held = osk_held_shift;

	switch (event->value.keycode) {
	case NSFB_KEY_LSHIFT:
		held = down ? (held | 1) : (held & ~1u);
		break;

	case NSFB_KEY_RSHIFT:
		held = down ? (held | 2) : (held & ~2u);
		break;

	case NSFB_KEY_CAPSLOCK:
		if (down) {
			osk_caps = !osk_caps;
			osk_update_labels();
		}
		return;

	default:
		return;
	}

	if ((held != 0) != (osk_held_shift != 0)) {
		osk_held_shift = held;
		osk_update_labels();
	} else {
		osk_held_shift = held;
	}
}

/* exported function documented in fbtk.h */
void
fbtk_set_osk_callback(void (*changed)(void))
{
	osk_changed = changed;
}

/* exported function documented in fbtk.h */
int
fbtk_osk_top(void)
{
	if ((osk == NULL) || (osk->mapped == false))
		return INT_MAX;

	return fbtk_get_absy(osk);
}

/*
 * Local Variables:
 * c-basic-offset:8
 * End:
 */
