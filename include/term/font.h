#ifndef __TERM_FONT_H__
#define __TERM_FONT_H__

#include <stdint.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>
#include <hb-ft.h>

typedef struct {
	FT_Library library;
	FT_Face face;
	hb_font_t *hb_font;
	uint32_t px_sz;
	uint32_t ascent;
	uint32_t xadv;
	uint32_t yadv;
} term_font_t; 

void term_font_destroy(term_font_t *font);
term_font_t *term_font_from_name(const char *name, uint32_t px);

#endif
