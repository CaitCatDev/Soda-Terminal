#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include <hb.h>
#include <hb-ft.h>

#include <fontconfig/fontconfig.h>

#include <soda-term/log.h>
#include <soda-term/font.h>

static const char *fc_result_to_string(FcResult result) {
	switch(result) {
		case FcResultMatch: return "FcResultMatch";
		case FcResultNoMatch: return "FcResultNoMatch";
		case FcResultTypeMismatch: return "FcResultTypeMismatch";
		case FcResultNoId: return "FcResultNoId";
		case FcResultOutOfMemory: return "FcResultOutOfMemory";
		default: return "Unknown";
	}
}

static const char *font_path_from_name(const char *name) {
	FcConfig *config = NULL;
	FcPattern *pattern, *font;
	FcResult res = 0;
	FcChar8 *filename = NULL;
	unsigned long len = 0;
	char *output = NULL;

	config = FcInitLoadConfigAndFonts();
	if(!config) {
		log_error("FcInitLoadConfigAndFonts failed %s\n", strerror(errno));
		return NULL;
	}

	pattern = FcNameParse((const FcChar8*)name);
	if(!pattern) {
		log_error("FcNameParse failed %s\n", strerror(errno));
		goto __error_config;
	}

	if(FcConfigSubstitute(config, pattern, FcMatchPattern) == FcFalse) {
		log_error("FcConfigSubstitute failed %s\n", strerror(errno));
		goto __error_pattern;
	}
	FcDefaultSubstitute(pattern);

	font = FcFontMatch(config, pattern, &res);
	if(!font) {
		log_error("FcFontMatch failed %s %s\n", fc_result_to_string(res), strerror(errno));
		goto __error_pattern;
	}
	
	res = FcPatternGetString(font, FC_FILE, 0, &filename);
	if(res != FcResultMatch) {
		log_error("FcPatternGetString failed %s %s\n", fc_result_to_string(res), strerror(errno));
	} else {
		len = strlen((char*)filename);
		output = calloc(1, len + 1);
		if(output) {
			memcpy(output, filename, len);
		} else {
			log_error("output name allocation failed\n");
		}
	}
	FcPatternDestroy(font);

__error_pattern:
	FcPatternDestroy(pattern);
__error_config:
	FcConfigDestroy(config);
	return output;
}

void soda_font_destroy(soda_font_t *font) {
	if(!font) return;

	/*TODO: Cache Management*/
	soda_glyph_t *tmp = font->cache;
	soda_glyph_t *next = NULL;
	for(; tmp; tmp = next) {
		next = tmp->next;
		free(tmp);
	}

	hb_font_destroy(font->hb_font);

	FT_Done_Face(font->face);
	FT_Done_FreeType(font->library);
	free(font);
}

void soda_glyph_insert(soda_glyph_t **head, soda_glyph_t *new) {
	if(*head == NULL) {
		*head = new;
		return;
	}

	for(soda_glyph_t *tmp = *head; tmp; tmp = tmp->next) {
		if(tmp->next == NULL) {
			tmp->next = new;
			return;
		}
	}
}

soda_glyph_t *soda_font_get_glyph(soda_font_t *font, uint32_t glyph_id, uint8_t bold, uint8_t italic) {
	soda_glyph_t *glyph = font->cache;
	for(; glyph; glyph = glyph->next) {
		if(glyph->glyph_id == glyph_id && glyph->bold == bold && glyph->italic == italic) {
			return glyph;
		}
	}

	FT_Face face = font->face;
	FT_Load_Glyph(face, glyph_id, FT_LOAD_DEFAULT);
	if(bold) {
		FT_GlyphSlot_Embolden(face->glyph);
	}
	if(italic) {
		FT_GlyphSlot_Oblique(face->glyph);
	}

	FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
	FT_GlyphSlot ft_glyph = face->glyph;

	soda_glyph_t *new = malloc(sizeof(soda_glyph_t) + ft_glyph->bitmap.rows * ft_glyph->bitmap.pitch);
	new->pitch = ft_glyph->bitmap.pitch;
	new->width = ft_glyph->bitmap.width;
	new->height = ft_glyph->bitmap.rows;
	new->bitmap_top = ft_glyph->bitmap_top;
	new->bitmap_left = ft_glyph->bitmap_left;
	new->bold = bold;
	new->italic = italic;
	memcpy(new->bitmap, ft_glyph->bitmap.buffer, new->pitch * new->height);
	new->glyph_id = glyph_id;
	new->next = NULL;
	soda_glyph_insert(&font->cache, new);

	return new;
}

soda_font_t *soda_font_from_name(const char *name, uint32_t px, hb_feature_t *features, uint32_t feature_count) {
	FT_Error error;
	soda_font_t *font;
	const char *filename;

	filename = font_path_from_name(name);
	if(!filename) {
		return NULL;
	}
	log_debug("Font File: %s\n", filename);

	font = calloc(1, sizeof(soda_font_t) + sizeof(hb_feature_t) * feature_count);
	if(!font) {
		log_error("font allocation failed\n");
		goto err_free_filename;
	}

	error = FT_Init_FreeType(&font->library);
	if(error) {
		log_error("FT_Init_FreeType failed: %s\n", FT_Error_String(error));
		goto err_free_font;
	}

	error = FT_New_Face(font->library, filename, 0, &font->face);
	if(error) {
		log_error("FT_New_Face failed: %s\n", FT_Error_String(error));
		goto err_done_freetype;
	}
	FT_Set_Pixel_Sizes(font->face, px, px);

	/*NotoSansMono Max Advanced is not the same as rest of font
	 *So use M glyph's advance this has the added benefit of making
	 *non monospaced also render better
	 */
	FT_Load_Char(font->face, 'M', FT_LOAD_DEFAULT);
	font->xadv = font->face->glyph->metrics.horiAdvance >> 6;
	font->yadv = font->face->size->metrics.height >> 6;
	font->ascent = font->face->size->metrics.ascender >> 6;
	font->hb_font = hb_ft_font_create_referenced(font->face);
	if(font->hb_font == NULL) {
		log_error("hb_ft_font_create_referenced failed: %s\n", strerror(errno));
		goto err_done_face;
	}

	hb_ft_font_set_load_flags(font->hb_font, FT_LOAD_DEFAULT);
	memcpy(font->features, features, feature_count * sizeof(hb_feature_t));
	font->feat_count = feature_count;

	free((char*)filename);
	return font;
	err_done_face:
	FT_Done_Face(font->face);
	err_done_freetype:
	FT_Done_FreeType(font->library);
	err_free_font:
	free(font);
	err_free_filename:
	free((char*)filename);
	return NULL;
}
