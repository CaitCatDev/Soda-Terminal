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
	uint32_t feat_count;
	hb_feature_t features[];
} soda_font_t;

void soda_font_destroy(soda_font_t *font);
soda_font_t *soda_font_from_name(const char *name, uint32_t px, hb_feature_t *features, uint32_t feature_count);

#endif
