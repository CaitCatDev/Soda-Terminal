#ifdef __linux__
	#define _XOPEN_SOURCE 600
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H

#include <hb.h>

#include <xkbcommon/xkbcommon.h>

#include <soda-term/vt.h>
#include <soda-term/log.h>
#include <soda-term/font.h>
#include <soda-term/display.h>

#define FORMAT_ARGB8888 0
#define FORMAT_XRGB8888 1
#define TERM_BRACKTED_PASTE_START_STR "\x1b[200~"
#define TERM_BRACKTED_PASTE_END_STR "\x1b[201~"


typedef struct soda_ctx {
	int running;
	int dirty;


	vt_ctx_t *vt;
	soda_shm_buffer_t *buffer;

	struct xkb_keymap *keymap;
	struct xkb_state *state;

	soda_font_t *font;
	bool no_harfbuzz;

	soda_display_t *display;
	int32_t width, height;
	int32_t x, y;
	uint32_t button_state;
} soda_ctx_t;

int allocate_shm_file(int32_t size) {
	char template[] = "/xxxx-sdvt-shm";
	int fd = -1;
	int res = -1;

	do {
		for(uint32_t i = 1; i < 5; i++) {
			template[i] = '0' + rand() % 10;
		}
		fd = shm_open(template, O_RDWR | O_CREAT | O_EXCL, S_IWUSR | S_IRUSR);
	} while(fd < 0 && errno == EEXIST);
	if(fd < 0) {
		log_error("shm_open: %s\n", strerror(errno));
		return -1;
	}
	shm_unlink(template);

	
	do {
	 res = posix_fallocate(fd, 0, size);
	} while(res < 0 && errno == EINTR);
	if(res < 0) {
		log_error("posix_fallocate: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

void soda_buffer_deinit(soda_shm_buffer_t *buffer) {
	if(!(buffer->fd < 0)) {
		close(buffer->fd);
	}

	if(buffer->data) {
		munmap(buffer->data, buffer->size);
	}
	free(buffer);
}

void soda_shm_buffer_freed(void *buffer) {
	soda_shm_buffer_t *shm = buffer;

	shm->in_use = false;
	if(shm->free_after_use) {
		soda_buffer_deinit(buffer);
	}
}

soda_shm_buffer_t *soda_buffer_init(int32_t width, int32_t height, int32_t stride, int32_t size, uint32_t format) {
	soda_shm_buffer_t *buffer = calloc(1, sizeof(soda_shm_buffer_t));
	int fd = 0;

	if(buffer == NULL) {
		return NULL;
	}

	fd = allocate_shm_file(size);
	if(fd < 0) {
		free(buffer);
		return NULL;
	}

	buffer->fd = fd;
	buffer->format = format;
	buffer->size = size;
	buffer->stride = stride;
	buffer->width = width;
	buffer->height = height;
	buffer->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	buffer->buffer_freed = soda_shm_buffer_freed;
	if(buffer->data == MAP_FAILED) {
		close(fd);
		free(buffer);
		return NULL;
	}

	return buffer;
}

static inline void put_pixel(soda_shm_buffer_t *buffer, int32_t x, int32_t y, uint32_t px) {
	uint32_t *data = (uint32_t*)buffer->data;
	int32_t w = buffer->width;

	if(y < 0 || y >= buffer->height) {
		return;
	}
	if(x < 0 || x >= buffer->width) {
		return;
	}

	data[y * w + x] = px;
}

uint32_t get_pixel(soda_shm_buffer_t *buffer, int32_t x, int32_t y) {
	uint32_t *data = (uint32_t*)buffer->data;
	int32_t w = buffer->width;
	int32_t h = buffer->height;

	if(y < 0 || x < 0) {
		return 0;
	}
	if(y >= h || x >= w) {
		return 0;
	}

	return data[y * w + x];
}

static uint32_t alpha_blend(uint32_t cnew, uint32_t cdst, uint8_t alpha) {
	uint8_t rn = (cnew >> 16) & 0xff;
	uint8_t gn = (cnew >> 8) & 0xff;
	uint8_t bn = (cnew) & 0xff;

	uint8_t rd = (cdst >> 16) & 0xff;
	uint8_t gd = (cdst >> 8) & 0xff;
	uint8_t bd = (cdst) & 0xff;

	uint8_t ro = (alpha * (rn - rd) + (rd << 8)) >> 8;
	uint8_t go = (alpha * (gn - gd) + (gd << 8)) >> 8;
	uint8_t bo = (alpha * (bn - bd) + (bd << 8)) >> 8;

	return MAKE_ARGB(ro, go, bo);
}

static uint32_t desaturate(uint32_t cnew, uint8_t intensity) {
	uint8_t rn = (cnew >> 16) & 0xff;
	uint8_t gn = (cnew >> 8) & 0xff;
	uint8_t bn = (cnew) & 0xff;

	uint8_t ro = (rn * intensity) >> 8;
	uint8_t go = (gn * intensity) >> 8;
	uint8_t bo = (bn * intensity) >> 8;

	return MAKE_ARGB(ro, go, bo);
}

static void render_glyph(soda_shm_buffer_t *buffer, soda_font_t *font, uint32_t glyph_index, int32_t x, int32_t y, uint32_t fg) {
	soda_glyph_t *glyph = soda_font_get_glyph(font, glyph_index, 0, 0);


	for(uint32_t cy = 0; cy < glyph->height; cy++) {
		for(uint32_t cx = 0; cx < glyph->width; cx++) {
			uint8_t alpha = glyph->bitmap[cy * glyph->pitch + cx];
			uint32_t px = get_pixel(buffer, x + cx + glyph->bitmap_left, y + font->ascent + cy - glyph->bitmap_top);
			px = alpha_blend(fg, px, alpha);
			put_pixel(buffer, x + cx, font->ascent + y + cy - glyph->bitmap_top, px);
		}
	}
}

static void render_char(soda_shm_buffer_t *buffer, soda_font_t *font, uint32_t utf, uint32_t x, uint32_t y, uint32_t fg) {
	uint32_t glyph_index = FT_Get_Char_Index(font->face, utf);
	render_glyph(buffer, font, glyph_index, x, y, fg);
}

static void render_term_cell(soda_shm_buffer_t *buffer, soda_font_t *font, uint32_t glyph_index, uint32_t x, uint32_t y, vt_cell_t *cell) {
	uint8_t bold = cell->attributes & TERM_CELL_ATTRIBUTE_BOLD;
	uint8_t italic = cell->attributes & TERM_CELL_ATTRIBUTE_ITALIC;
	soda_glyph_t *glyph = soda_font_get_glyph(font, glyph_index, bold, italic);

	uint32_t rows = glyph->height;
	uint32_t width = glyph->width;
	uint32_t pitch = glyph->pitch;
	uint8_t *bitmap = glyph->bitmap;
	uint32_t *data = buffer->data;
	uint32_t fg = cell->attributes & TERM_CELL_ATTRIBUTE_INVERSE ? cell->bg : cell->fg;
	uint32_t bg = cell->attributes & TERM_CELL_ATTRIBUTE_INVERSE ? cell->fg : cell->bg;

	for(uint32_t cy = 0; cy < font->yadv; cy++) {
		for(uint32_t cx = 0; cx < font->xadv; cx++) {
			data[(y + cy) * buffer->width + (x + cx)] = bg;
		}
	}

	if(cell->attributes & TERM_CELL_ATTRIBUTE_INVIS) {
		return;
	}

	if(cell->attributes & TERM_CELL_ATTRIBUTE_FAINT) {
		fg = desaturate(fg, 0x80);
	}

	for(uint32_t cy = 0; cy < rows; cy++) {
		for(uint32_t cx = 0; cx < width; cx++) {
			uint8_t alpha = bitmap[cy * pitch + cx];
			uint32_t px = alpha_blend(fg, bg, alpha);
			data[(y + cy + font->ascent - glyph->bitmap_top) * buffer->width + (x + cx + glyph->bitmap_left)] = px;
		}
	}

	if(cell->attributes & (TERM_CELL_ATTRIBUTE_CROSSED_OUT)) {
		for(uint32_t cx = 0; cx < font->xadv; cx++) {
			data[(y + (font->yadv >> 1)) * buffer->width + (x + cx)] = fg;
		}
	}

	if(cell->attributes & (TERM_CELL_ATTRIBUTE_UNDERLINE | TERM_CELL_ATTRIBUTE_DBL_UNDERLINE)) {
		for(uint32_t cx = 0; cx < font->xadv; cx++) {
			data[(y + font->ascent) * buffer->width + (x + cx)] = fg;
		}
	}

	if(cell->attributes & (TERM_CELL_ATTRIBUTE_DBL_UNDERLINE)) {
		for(uint32_t cx = 0; cx < font->xadv; cx++) {
			data[(y + font->ascent + 3) * buffer->width + (x + cx)] = fg;
		}
	}


}

static int render_term_text_hb(vt_ctx_t *ctx, soda_font_t *font, soda_shm_buffer_t *buffer) {
	int32_t max_cols = ctx->max_cols;
	int32_t max_rows = ctx->max_rows;

	hb_buffer_t *buf = hb_buffer_create();
	for(int32_t i = 0; i < max_rows; i++) {
		hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
		hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
		hb_buffer_set_language(buf, hb_language_from_string("en", -1));
		vt_line_t line = ctx->screen[i];
		if(line.dirty == 0) {
			break;
		}
		line.dirty = 0;
		if(hb_buffer_allocation_successful(buf) == false) {
			log_error("hb_buffer_create failed: %s\n", strerror(errno));
			return -1;
		}
		for(int32_t x = 0; x < max_cols; x++) {
			hb_buffer_add_utf32(buf, &line.cells[x].utf32, 1, 0, -1);
		}

		hb_shape(font->hb_font, buf, font->features, 1);
		unsigned int glyph_count = 0;
		hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
		for(uint32_t j = 0; j < glyph_count; j++) {
			hb_codepoint_t glyphid = glyph_info[j].codepoint;
			render_term_cell(buffer, font, glyphid, j * font->xadv, font->yadv * i, &line.cells[j]);
		}
		hb_buffer_reset(buf);
	}
	hb_buffer_destroy(buf);
	return 0;
}

static int render_term_text_ft(vt_ctx_t *ctx, soda_font_t *font, soda_shm_buffer_t *buffer) {
	for(int32_t y = 0; y < ctx->max_rows; ++y) {
		for(int32_t x = 0; x < ctx->max_cols; ++x) {
			if((ctx->screen[y].cells[x].attributes & TERM_CELL_ATTRIBUTE_DIRTY) == 0) {
				continue;
			}
			for(uint32_t cy = 0; cy < font->yadv; cy++) {
				for(uint32_t cx = 0; cx < font->xadv; cx++) {
					put_pixel(buffer, x * font->xadv + cx, font->yadv * y + cy, ctx->screen[y].cells[x].bg);
				}
			}

			if(ctx->screen[y].cells[x].utf32) {
				uint32_t gi = FT_Get_Char_Index(font->face, ctx->screen[y].cells[x].utf32);
				render_term_cell(buffer, font, gi, font->xadv * x, y * font->yadv, &ctx->screen[y].cells[x]);
			}
			ctx->screen[y].cells[x].attributes &= ~TERM_CELL_ATTRIBUTE_DIRTY;
		}
	}
	return 0;
}

int draw_frame(soda_ctx_t *ctx, soda_shm_buffer_t *buffer) {
	uint32_t fg = ctx->vt->term_mode & TERM_MODE_REVERSE_VIDEO ? ctx->vt->def_bg : ctx->vt->def_fg;

	if(ctx->no_harfbuzz) {
		render_term_text_ft(ctx->vt, ctx->font, buffer);
	} else {
		render_term_text_hb(ctx->vt, ctx->font, buffer);
	}

	if(ctx->vt->term_mode & TERM_MODE_SHOW_CURSOR) {
		render_char(buffer, ctx->font, ctx->vt->cursor.utf32, ctx->font->xadv * ctx->vt->cursor_pos.x, ctx->vt->cursor_pos.y * ctx->font->yadv, fg);
	}

	return 0;
}

static void send_arrow_key(vt_ctx_t *term, char c) {
	char csi_buffer[4] = "\x1b[0";
	char ss3_buffer[4] = "\x1bO0";

	if(term->term_mode & TERM_MODE_APP_CURSOR_KEYS) {
		ss3_buffer[2] = c;
		write(term->ptmx, ss3_buffer, strlen(ss3_buffer));
	} else {
		csi_buffer[2] = c;
		write(term->ptmx, csi_buffer, strlen(csi_buffer));
	}
}

static void term_mouse_report(soda_ctx_t *ctx, bool motion, uint32_t btn, uint32_t state, int32_t x, int32_t y) {
	char buffer[64] = { 0 };
	size_t len = 0;
	uint32_t i = 0;
	uint8_t c = 0;
	x /= ctx->font->xadv;
	y /= ctx->font->yadv;

	if(motion) {
		for(i = 0; i < 3; i++) {
			if(ctx->button_state & (1 << i)) {
				break;
			}
		}

		c += 32 + i;
	} else {
		c += state ? btn - 1 : 3;
	}

	if(ctx->vt->pointer_mode & TERM_MODE_MOUSE_SGR) {
		len = snprintf(buffer, 64, "\x1b[<%d;%d;%d%c", c, x+1, y+1, (state | motion) ? 'M' : 'm');
		write(ctx->vt->ptmx, buffer, len);
		return;
	}

	if(ctx->vt->pointer_mode & TERM_MODE_MOUSE_X10 && state == 0) return;
	if(x < 223 && y < 223) return;

	if(ctx->vt->pointer_mode & (TERM_MODE_MOUSE_X10 | TERM_MODE_MOUSE_NORMAL | TERM_MODE_MOUSE_MOTION_ALL | TERM_MODE_MOUSE_BUTTON)) {
		snprintf(buffer, 64, "\x1b[M%c%c%c", ' '+c, ' '+x+1, ' '+y+1);
		write(ctx->vt->ptmx, buffer, 6);
	}
}

void term_handle_motion(void *data, int32_t x, int32_t y) {
	soda_ctx_t *ctx = (soda_ctx_t*)data;

	ctx->x = x;
	ctx->y = y;

	if((ctx->button_state && ctx->vt->pointer_mode & TERM_MODE_MOUSE_BUTTON) || ctx->vt->pointer_mode & TERM_MODE_MOUSE_MOTION_ALL) {
		term_mouse_report(ctx, true, 0, 0, ctx->x, ctx->y);
	}
}

void term_handle_pointer_focus(void *data, uint32_t state) {
	char buffer[4] = "\x1b[O";
	soda_ctx_t *ctx = (soda_ctx_t*)data;

	if(state) buffer[2] = 'I';

	if(ctx->vt->pointer_mode & TERM_MODE_MOUSE_FOCUS) {
		write(ctx->vt->ptmx, buffer, 3);
	}
}

void term_handle_button(void *data, uint32_t button, uint32_t state) {
	soda_ctx_t *ctx = (soda_ctx_t*)data;

	if(button > 3) {
		printf("Unrecognized button %d\n", button);
	}

	if(state) {
		ctx->button_state |= 1 << (button - 1);
	} else {
		ctx->button_state &= ~(1<<(button - 1));
	}
	if(ctx->vt->pointer_mode & (TERM_MODE_MOUSE_NORMAL | TERM_MODE_MOUSE_BUTTON | TERM_MODE_MOUSE_MOTION_ALL | TERM_MODE_MOUSE_X10)) {
		term_mouse_report(ctx, false, button, state, ctx->x, ctx->y);
	}
}

void term_handle_configure(void *data, uint32_t width, uint32_t height) {
	soda_ctx_t *ctx = data;
	vt_ctx_t *vt = ctx->vt;
	ctx->width = width;
	ctx->height = height;
	int32_t rows = (ctx->height-1) / ctx->font->yadv;
	int32_t cols = ctx->width / ctx->font->xadv;
	uint32_t tabstop_size = (cols >> 5) + ((cols & 31) ? 1 : 0);

	if(cols != vt->max_cols) {
			uint32_t *tabstops = calloc(tabstop_size, sizeof(uint32_t));
			memset(tabstops, 0x80, tabstop_size * sizeof(uint32_t));
			free(vt->tabstops);
			vt->tabstops = tabstops;
			for(int32_t r = 0; r < vt->max_rows; r++) {
			void *tmp = realloc(vt->primary[r].cells, sizeof(vt_cell_t) * cols);
			void *tmp2 = realloc(vt->alt[r].cells, sizeof(vt_cell_t) * cols);
			vt->primary[r].cells = tmp;
			vt->alt[r].cells = tmp2;
			for(int32_t c = vt->max_cols; c < cols; c++) {
				vt->primary[r].cells[c].utf32 = ' ';
				vt->primary[r].cells[c].fg = vt->fg;
				vt->primary[r].cells[c].bg = vt->bg;
				vt->primary[r].cells[c].attributes = 0;
				vt->alt[r].cells[c].utf32 = ' ';
				vt->alt[r].cells[c].fg = vt->fg;
				vt->alt[r].cells[c].bg = vt->bg;
				vt->alt[r].cells[c].attributes = 0;
			}
		}
		vt->max_cols = cols;
	}


	if(rows != vt->max_rows) {
		for(int32_t r = rows; r < vt->max_rows; r++) {
			free(vt->primary[r].cells);
			free(vt->alt[r].cells);
		}
		void *tmp = realloc(vt->primary, sizeof(vt_line_t) * rows);
		void *tmp2 = realloc(vt->alt, sizeof(vt_line_t) * rows);
		vt->screen = vt->screen == vt->primary ? tmp : tmp2;
		vt->primary = tmp;
		vt->alt = tmp2;
		for(int32_t r = vt->max_rows; r < rows; r++) {
			vt->primary[r].cells = malloc(vt->max_cols * sizeof(vt_cell_t));
			vt->alt[r].cells = malloc(vt->max_cols * sizeof(vt_cell_t));
			for(int32_t c = 0; c < vt->max_cols; ++c) {
				vt->primary[r].cells[c].utf32 = ' ';
				vt->primary[r].cells[c].fg = vt->fg;
				vt->primary[r].cells[c].bg = vt->bg;
				vt->primary[r].cells[c].attributes = 0;
				vt->alt[r].cells[c].utf32 = ' ';
				vt->alt[r].cells[c].fg = vt->fg;
				vt->alt[r].cells[c].bg = vt->bg;
				vt->alt[r].cells[c].attributes = 0;
			}
		}
		vt->max_rows = rows;
		vt->bottom = rows-1;
	}

	if(vt->max_cols <= vt->cursor_pos.x) vt->cursor_pos.x = vt->max_cols - 1;
	if(vt->max_rows <= vt->cursor_pos.y) vt->cursor_pos.y = vt->max_rows - 1;

	struct winsize wsz = { rows, cols, width, height };
	ioctl(vt->ptmx, TIOCSWINSZ, &wsz);
	if(vt->term_mode & TERM_MODE_RESIZE_NOTIFY) {
		char buffer[CSI_BUFFER_LEN] = { 0 };
		snprintf(buffer, CSI_BUFFER_LEN, "\x1b[48;%d;%d;%d;%dt", rows, cols, height, width);
		write(vt->ptmx, buffer, strlen(buffer));
	}

	if(ctx->buffer) {
	ctx->buffer->free_after_use = true;
		if(ctx->buffer && ctx->buffer->in_use == 0) {
			soda_buffer_deinit(ctx->buffer);
			ctx->buffer = NULL;
		}
	}
	ctx->buffer = soda_buffer_init(width, height, width * 4, width * 4 * height, FORMAT_ARGB8888);
	uint32_t bg = ctx->vt->term_mode & TERM_MODE_REVERSE_VIDEO ? ctx->vt->def_fg : ctx->vt->def_bg;
	uint32_t *buffer_data = ctx->buffer->data;
	int32_t size = height * width;

	for(int32_t y = 0; y < (int32_t)size; ++y) {
		((uint32_t*)buffer_data)[y] = bg;
	}

	draw_frame(ctx, ctx->buffer);
	ctx->display->attach_shm(ctx->display, ctx->buffer);
}

void term_handle_cliboard_str(void *data, const char *str) {
	soda_ctx_t *ctx = (soda_ctx_t*)data;
	vt_ctx_t *vt = ctx->vt;

	if(vt->term_mode & TERM_MODE_BRACKTED_PASTE) {
		write(vt->ptmx, TERM_BRACKTED_PASTE_START_STR, strlen(TERM_BRACKTED_PASTE_START_STR));
	}

	write(vt->ptmx, str, strlen(str));

	if(vt->term_mode & TERM_MODE_BRACKTED_PASTE) {
		write(vt->ptmx, TERM_BRACKTED_PASTE_END_STR, strlen(TERM_BRACKTED_PASTE_END_STR));
	}
}

typedef struct term_app_keypad_strs {
	xkb_keysym_t keysym;
	const char *str;
} term_app_keypad_strs_t;

/*Based on VT102/220 keypad*/
static const term_app_keypad_strs_t app_keypad[] = {
	{ XKB_KEY_KP_Space, "\x1bO " },
	{ XKB_KEY_KP_Tab, "\x1bOI" },
	{ XKB_KEY_KP_Enter, "\x1bOM" },
	{ XKB_KEY_KP_Multiply, "\x1bOj" },
	{ XKB_KEY_KP_Add, "\x1bOk" },
	{ XKB_KEY_KP_Separator, "\x1bOl" },
	{ XKB_KEY_KP_Subtract, "\x1bOm" },
	{ XKB_KEY_KP_Decimal, "\x1bOn" },
	{ XKB_KEY_KP_Divide, "\x1bOo" },
	{ XKB_KEY_KP_0, "\x1bOp" },
	{ XKB_KEY_KP_1, "\x1bOq" },
	{ XKB_KEY_KP_2, "\x1bOr" },
	{ XKB_KEY_KP_3, "\x1bOs" },
	{ XKB_KEY_KP_4, "\x1bOt" },
	{ XKB_KEY_KP_5, "\x1bOu" },
	{ XKB_KEY_KP_6, "\x1bOv" },
	{ XKB_KEY_KP_7, "\x1bOw" },
	{ XKB_KEY_KP_8, "\x1bOx" },
	{ XKB_KEY_KP_9, "\x1bOy" },
	{ XKB_KEY_KP_Equal, "\x1bOX" },
	{ XKB_KEY_KP_Up, "\x1bOA" },
	{ XKB_KEY_KP_Down, "\x1bOB" },
	{ XKB_KEY_KP_Left, "\x1bOD" },
	{ XKB_KEY_KP_Right, "\x1bOC" },
	{ XKB_KEY_KP_Insert, "\x1b[2~" },
	{ XKB_KEY_KP_Delete, "\x1b[3~" },
	{ XKB_KEY_KP_Home, "\x1b[1~" },
	{ XKB_KEY_KP_End, "\x1b[4~" },
	{ XKB_KEY_KP_Page_Up, "\x1b[5~" },
	{ XKB_KEY_KP_Page_Down, "\x1b[6~" },
	{ XKB_KEY_F1, "\x1b[11~" },
	{ XKB_KEY_F2, "\x1b[12~" },
	{ XKB_KEY_F3, "\x1b[13~" },
	{ XKB_KEY_F4, "\x1b[14~" },
	{ XKB_KEY_F5, "\x1b[15~" },
	{ XKB_KEY_F6, "\x1b[17~" },
	{ XKB_KEY_F7, "\x1b[18~" },
	{ XKB_KEY_F8, "\x1b[19~" },
	{ XKB_KEY_F9, "\x1b[20~" },
	{ XKB_KEY_F10, "\x1b[21~" },
	{ XKB_KEY_F11, "\x1b[23~" },
	{ XKB_KEY_F12, "\x1b[24~" },
};

int term_handle_key_application(soda_ctx_t *ctx, uint32_t key) {
	xkb_keysym_t keysym = xkb_state_key_get_one_sym(ctx->state, key);

	for(uint32_t i = 0; i < sizeof(app_keypad) / sizeof(app_keypad[0]); i++) {
		if(keysym == app_keypad[i].keysym) {
			write(ctx->vt->ptmx, app_keypad[i].str, strlen(app_keypad[i].str));
			return 1;
		}
	}
	return 0;
}

void term_handle_key(void *data, uint32_t key, uint32_t state) {
	soda_ctx_t *ctx = data;
	vt_ctx_t *vt = ctx->vt;
	xkb_keysym_t keysym = 0;
	char utf8[5] = { 0 };

	if(state == 0) {
		return;
	}
	if(vt->term_mode & TERM_MODE_APP_KEYPAD) {
		if(term_handle_key_application(ctx, key)) {
			return;
		}
	}

	xkb_mod_mask_t shift = xkb_keymap_mod_get_index(ctx->keymap, XKB_MOD_NAME_SHIFT);
	xkb_mod_mask_t ctrl = xkb_keymap_mod_get_index(ctx->keymap, XKB_MOD_NAME_CTRL);

	if(xkb_state_mod_indices_are_active(ctx->state, XKB_STATE_MODS_DEPRESSED, XKB_STATE_MATCH_ALL, shift, ctrl, XKB_MOD_INVALID)) {
		if(xkb_state_key_get_one_sym(ctx->state, key) == XKB_KEY_V) {
			ctx->display->request_cliboard_text(ctx->display);
			return;
		}
	}

	keysym = xkb_state_key_get_one_sym(ctx->state, key);
	if(keysym == XKB_KEY_Up) {
		send_arrow_key(vt, 'A');
	} else if(keysym == XKB_KEY_Down) {
		send_arrow_key(vt, 'B');
	} else if(keysym == XKB_KEY_Left) {
		send_arrow_key(vt, 'D');
	} else if(keysym == XKB_KEY_Right) {
		send_arrow_key(vt, 'C');
	} else {
		/*Convert to UTF8*/
		xkb_state_key_get_utf8(ctx->state, key, utf8, 5);
		if(utf8[0] == 8) {
			utf8[0] = 127;
		}

		write(vt->ptmx, utf8, strlen(utf8));
	}
}

void term_handle_keymap(void *data, struct xkb_keymap *keymap, struct xkb_state *state) {
	soda_ctx_t *ctx = data;

	ctx->state = state;
	ctx->keymap = keymap;
}

void term_handle_close(void *data) {
	soda_ctx_t *ctx = data;

	ctx->running = 0;
}

int strtou32(const char *str, int base, uint32_t *value) {
	errno = 0;
	char *end = NULL;
	unsigned long res = 0;

	if(value == NULL || str == NULL) {
		errno = EINVAL;
		return -1;
	}

	res = strtoul(str, &end, base);
	if(errno != 0) {
		return -1;
	} else if(str == end) {
		errno = EINVAL;
		return -1;
	}

	*value = (uint32_t)res;
	return 0;
}

static void usage(const char *arg0) {
	printf("Usage: %s [OPTIONS]\n", arg0);

	printf("Options:\n");
	printf("%s%s%s%s%s%s",
				 "\t--help\tdisplay this message and exit\n",
				 "\t--font-name\toverride default font name\n",
				 "\t--bg-color\toverride default bg color\n",
				 "\t--fg-color\toverride default fg color\n",
				 "\t--disable-ligatures\tdisable harfbuzz ligatures\n",
				 "\t--disable-harfbuzz\tdisable all harfbuzz shaping\n");

	return;
}

void soda_term_redraw(void *data) {
	soda_ctx_t *ctx = (soda_ctx_t*)data;
	soda_shm_buffer_t *buffer = ctx->buffer;

	if(ctx->buffer->in_use) {
		buffer->free_after_use = true;
		soda_shm_buffer_t *new = soda_buffer_init(buffer->width, buffer->height, buffer->stride, buffer->size, buffer->format);
		memcpy(new->data, buffer->data, buffer->size);
		ctx->buffer = new;
	}
	draw_frame(ctx, ctx->buffer);
	ctx->display->attach_shm(ctx->display, ctx->buffer);
}

int main(int argc, char **argv) {
	srand(time(NULL));
	const char *font_name = "monospace";
	hb_feature_t feature;

	log_set_level(TERM_LOG_LEVEL_INFO, 0);
	log_set_file(stderr);

	soda_ctx_t *soda = calloc(1, sizeof(soda_ctx_t));
	uint32_t bg = 0xff000000;
	uint32_t fg = 0xffffffff;

	feature.tag = HB_TAG('c', 'a', 'l', 't');
	feature.value = 1;
	feature.start = HB_FEATURE_GLOBAL_START;
	feature.end = HB_FEATURE_GLOBAL_END;

	for(int i = 1; i < argc; ++i) {
		if(strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			free(soda);
			return -1;
		} else if(strcmp(argv[i], "--font-name") == 0) {
			if(i == argc - 1) {
				printf("Argument expected for --font-name\n");
				goto err_free_ctx;
			}
			font_name = argv[i+1];
		} else if(strcmp(argv[i], "--bg-color") == 0) {
			if(i == argc - 1) {
				printf("Argument expected for --bg-color\n");
				goto err_free_ctx;
			}
			if(strtou32(argv[i+1], 16, &bg) == -1) {
				printf("error strtou32: %s\n", strerror(errno));
				goto err_free_ctx;
			}
		} else if(strcmp(argv[i], "--fg-color") == 0) {
			if(i == argc - 1) {
				printf("Argument expected for --fg-color\n");
				goto err_free_ctx;
			}
			if(strtou32(argv[i+1], 16, &fg) == -1) {
				printf("error strtou32: %s\n", strerror(errno));
				goto err_free_ctx;
			}
		} else if(strcmp(argv[i], "--disable-harfbuzz") == 0) {
			soda->no_harfbuzz = true;
		} else if(strcmp(argv[i], "--disable-ligatures") == 0) {
			feature.value = false;
		}
	}

	soda->vt = vt_init(fg, bg);
	if(soda->vt == NULL) {
		log_error("Failed to init VT\n");
		goto err_free_ctx;
	}

	soda->font = soda_font_from_name(font_name, 16, &feature, 1);
	if(soda->font == NULL) {
		log_error("soda_font_from_name failed: %s\n", strerror(errno));
		goto err_free_vt;
	}

#if defined(TERM_WL_SUPPORT)
	if(getenv("WAYLAND_DISPLAY")) {
		soda->display = soda_wl_display_init();
	}
#endif
#if defined(TERM_X11_SUPPORT)
	if(soda->display == NULL && getenv("DISPLAY")) {
		soda->display = soda_x11_display_init();
	}
#endif

	if(soda->display == NULL) {
		log_error("Failed to create display\n");
		goto err_free_font;
	}

	soda->display->data = soda;
	soda->running = 1;
	soda->display->callbacks.keymap_change = term_handle_keymap;
	soda->display->callbacks.keypress = term_handle_key;
	soda->display->callbacks.close = term_handle_close;
	soda->display->callbacks.configure = term_handle_configure;
	soda->display->callbacks.clipboard_str_callback = term_handle_cliboard_str;
	soda->display->callbacks.pointer_motion = term_handle_motion;
	soda->display->callbacks.pointer_button = term_handle_button;
	soda->display->callbacks.pointer_focus = term_handle_pointer_focus;
	soda->display->callbacks.redraw = soda_term_redraw;
	struct pollfd *pfds = NULL;
	int *fds = NULL;
	int fdcount = soda->display->display_fds(soda->display, &fds);
	if(fdcount == -1) {
		log_error("getting display pollfds\n");
		goto err_free_display;
	}
	pfds = calloc(fdcount+1, sizeof(struct pollfd));
	if(pfds == NULL) {
		free(fds);
		goto err_free_display;
	}

	for(int i = 0; i < fdcount; ++i) {
		pfds[i].fd = fds[i];
		pfds[i].events = POLLIN;
	}
	free(fds);

	pfds[fdcount].events = POLLIN;
	pfds[fdcount].fd = soda->vt->ptmx;
	fdcount++;

	soda->width = 800;
	soda->height = 600;

	while(soda->buffer == NULL) {
		soda->display->dispatch(soda->display);
	}

	while(soda->running) {
		poll(pfds, fdcount, -1);
		for(int i = 0; i < fdcount - 1; ++i) {
			if(pfds[i].revents & (POLLHUP | POLLERR)) {
				soda->running = 0;
				break;
			} else if(pfds[i].revents & POLLIN) {
				soda->display->dispatch(soda->display);
			}
		}
		if(pfds[fdcount-1].revents & (POLLHUP | POLLERR)) {
			soda->running = 0;
			break;
		} else if(pfds[fdcount-1].revents & POLLIN) {
			vt_event(soda->vt);
			soda->dirty = 1;
		}
		if(soda->dirty) {
			soda->display->request_frame_callback(soda->display);
			soda->dirty = 0;
		}
	}
	free(pfds);
	soda->display->deinit(soda->display);
	soda_buffer_deinit(soda->buffer);
	soda_font_destroy(soda->font);
	vt_deinit(soda->vt);
	free(soda);

	return 0;
err_free_display:
	soda->display->deinit(soda->display);
err_free_font:
	soda_font_destroy(soda->font);
err_free_vt:
	vt_deinit(soda->vt);
err_free_ctx:
	free(soda);
	return -1;
}
