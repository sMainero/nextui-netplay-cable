/*
 * A 5x7 bitmap font, and just enough drawing to put a line of text into a
 * core's framebuffer.
 *
 * The shim cannot use minarch's font: api.c is compiled into the executable,
 * which is built without -rdynamic, so none of its GFX_ symbols are reachable
 * from a loaded core. Carrying ~400 bytes of glyph data is the cheap way to say
 * something legible on a paused frame.
 *
 * Glyphs are 7 rows of 5 bits, MSB-left in the low five bits of each byte.
 */

#ifndef OVERLAY_H
#define OVERLAY_H

#include <stddef.h>
#include <stdint.h>

#define OVL_GLYPH_W 5
#define OVL_GLYPH_H 7
#define OVL_ADVANCE 6 /* glyph plus one column of spacing */

/* Pixel formats, matching libretro's enum values. */
typedef enum {
	OVL_FMT_0RGB1555 = 0,
	OVL_FMT_XRGB8888 = 1,
	OVL_FMT_RGB565   = 2,
} OVL_Format;

typedef struct {
	void*      pixels;
	unsigned   width;
	unsigned   height;
	size_t     pitch;
	OVL_Format format;
} OVL_Target;

/* Halve every channel, so a held frame reads as inactive rather than hung. */
void OVL_dim(const OVL_Target* t);

/* Advance width: what the string occupies including the blank columns each
 * glyph reserves. Right for layout and wrapping. */
int OVL_textWidth(const char* text, int scale);

/* Ink extents, for optical centring. Advance width overstates a string ending
 * in '.' - a period paints two of its five columns - so centring on it leaves
 * the text visibly left of centre. *bearing is the blank space before the first
 * ink; the return is the width of the ink itself. */
int OVL_textInk(const char* text, int scale, int* bearing);

void OVL_drawText(const OVL_Target* t, int x, int y, const char* text, int scale);

void OVL_fillRect(const OVL_Target* t, int x, int y, int w, int h);

/* Break text on spaces so it fits max_w. A Game Boy is 160px wide, where a full
 * sentence does not fit on one line at any usable scale. Returns line count. */
#define OVL_MAX_LINE  48
int OVL_wrap(const char* text, int scale, int max_w,
             char lines[][OVL_MAX_LINE], int max_lines);

#endif
