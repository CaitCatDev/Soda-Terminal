#ifdef __linux__
	#define _XOPEN_SOURCE 600
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <poll.h>

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <hb.h>

#include <xkbcommon/xkbcommon.h>

#include <term/vt.h>
#include <term/display.h>
#include <term/font.h>
#include <term/log.h>

#include "freetype/freetype.h"

#define FORMAT_ARGB8888 0
#define FORMAT_XRGB8888 1

#define INITIAL_ROW_MAX 30
#define INITIAL_COLUMN_MAX 100
#define INITIAL_WIN_WIDTH 800
#define INITIAL_WIN_HEIGHT 600

#define BG_COLOR 0xff000000
#define FG_COLOR 0xfff8f8f2
#define CSD_BG_COLOR 0xffd3d3d3
#define CSD_FG_COLOR 0xff000000
#define UTF8_ESCAPE 0x1b

#define CSDS_HEIGHT 20

#define WIDGET_LEFT 0
#define WIDGET_RIGHT 1
#define WIDGET_CENTER 2

typedef struct {
	int32_t x, y;
	int32_t w, h;
	uint32_t anchor;
	uint32_t fg, bg;
	uint32_t hfg, hbg;
	uint32_t is_hovered;

	const char *label;
	void (*on_click)(void *data);
	void *data;
} widget_button_t;

typedef struct widget_label {
	int32_t x, y;
	int32_t w, h;
	uint32_t anchor;
	uint32_t fg;

	const char *label;
} widget_label_t;

#define TERM_BRACKTED_PASTE_START_STR "\x1b[200~"
#define TERM_BRACKTED_PASTE_END_STR "\x1b[201~"

int allocate_shm_file(int32_t size) {
	char template[] = "/xxxx-term-wlshm";
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

void put_pixel(uint32_t *data, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t px) {
	if(y <= 0 || x <= 0) {
		return;
	}
	if(y >= h || x >= w) {
		return;
	}

	data[y * w + x] = px;
}

uint32_t get_pixel(uint32_t *data, int32_t x, int32_t y, int32_t w, int32_t h) {
	if(y < 0 || x < 0) {
		return 0;
	}
	if(y >= h || x >= w) {
		return 0;
	}

	return data[y * w + x];
}

static uint32_t alpha_blend(uint32_t cnew, uint32_t cdst, float alpha) {
	uint8_t rn = (cnew >> 16) & 0xff;
	uint8_t gn = (cnew >> 8) & 0xff;
	uint8_t bn = (cnew) & 0xff;

	uint8_t rd = (cdst >> 16) & 0xff;
	uint8_t gd = (cdst >> 8) & 0xff;
	uint8_t bd = (cdst) & 0xff;

	uint8_t ro = rn * alpha + rd * (1.0f-alpha);
	uint8_t go = gn * alpha + gd * (1.0f-alpha);
	uint8_t bo = bn * alpha + bd * (1.0f-alpha);

	return MAKE_ARGB(ro, go, bo);
}

static void render_glyph(term_font_t *font, uint32_t glyph_index, uint32_t x, uint32_t y, uint32_t *data, uint32_t w, uint32_t h, uint32_t fg) {
	FT_Face face = font->face;
	FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
	FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
	FT_GlyphSlot glyph = face->glyph;

	for(uint32_t cy = 0; cy < glyph->bitmap.rows; cy++) {
		for(uint32_t cx = 0; cx < glyph->bitmap.width; cx++) {
			float alpha = (float)glyph->bitmap.buffer[cy * glyph->bitmap.pitch + cx] / 255.0f;
			uint32_t px = get_pixel(data, x + cx + glyph->bitmap_left, y + font->ascent + cy - glyph->bitmap_top, w, h);
			px = alpha_blend(fg, px, alpha);
			put_pixel(data, x + cx + glyph->bitmap_left, font->ascent + y + cy - glyph->bitmap_top, w, h, px);
		}
	}
}

static void render_char(term_font_t *font, uint32_t utf, uint32_t x, uint32_t y, uint32_t *data, uint32_t w, uint32_t h, uint32_t fg) {
	uint32_t glyph_index = FT_Get_Char_Index(font->face, utf);
	render_glyph(font, glyph_index, x, y, data, w, h, fg);
}

static void render_term_cell(term_font_t *font, uint32_t glyph_index, uint32_t x, uint32_t y,
														 term_cell_t *cell, uint32_t *data, uint32_t w, uint32_t h) {
	FT_Face face = font->face;
	FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
	if(cell->attributes == 1 && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
		FT_Outline_Embolden(&face->glyph->outline, 1 * 64);
	}
	FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
	FT_GlyphSlot glyph = face->glyph;
	if(glyph->bitmap.rows == 0 || face->glyph->bitmap.width == 0) {
		/*Skip rendering glyphs that don't have any image*/
		return;
	}


	for(uint32_t cy = 0; cy < glyph->bitmap.rows; cy++) {
		for(uint32_t cx = 0; cx < glyph->bitmap.width; cx++) {
			float alpha = (float)glyph->bitmap.buffer[cy * glyph->bitmap.pitch + cx] / 255.0f;
			uint32_t px = alpha_blend(cell->fg, cell->bg, alpha);
			put_pixel(data, x + cx + glyph->bitmap_left, font->ascent + y + cy - glyph->bitmap_top, w, h, px);
		}
	}
}

static int render_term_text_hb(term_ctx_t *ctx, int32_t width, int32_t height, int32_t stride, int32_t size, uint32_t *data) {
	for(int32_t i = 0; i < ctx->max_rows; i++) {
		hb_buffer_t *buf = hb_buffer_create();
		if(hb_buffer_allocation_successful(buf) == false) {
			log_error("hb_buffer_create failed: %s\n", strerror(errno));
			return -1;
		}
		for(int32_t x = 0; x < ctx->max_cols; x++) {
			if(ctx->screen[i][x].utf32 == 0) break;
			hb_buffer_add_utf32(buf, &ctx->screen[i][x].utf32, 1, 0, -1);
			for(uint32_t cy = 0; cy < ctx->font->yadv; cy++) {
				for(uint32_t cx = 0; cx < ctx->font->xadv; cx++) {
					put_pixel(data, x * ctx->font->xadv + cx, ctx->font->yadv * i + cy, ctx->width, ctx->height, ctx->screen[i][x].bg);
				}
			}
		}
		hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
		hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
		hb_buffer_set_language(buf, hb_language_from_string("en", -1));

		hb_shape(ctx->font->hb_font, buf, ctx->features, 1);
		unsigned int glyph_count = 0;
		hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
		for(uint32_t j = 0; j < glyph_count; j++) {
			hb_codepoint_t glyphid = glyph_info[j].codepoint;
			render_term_cell(ctx->font, glyphid, j * ctx->font->xadv, ctx->font->yadv * i, &ctx->screen[i][j], data, width, height);
		}
		hb_buffer_destroy(buf);
	}

	return 0;
	(void)size;
	(void)stride;
}

static int render_term_text_ft(term_ctx_t *ctx, int32_t width, int32_t height, int32_t stride, int32_t size, uint32_t *data) {
	for(int32_t y = 0; y < ctx->max_rows; ++y) {
		for(int32_t x = 0; x < ctx->max_cols; ++x) {
			if(ctx->screen[y][x].utf32) {
				uint32_t gi = FT_Get_Char_Index(ctx->font->face, ctx->screen[y][x].utf32);
				render_term_cell(ctx->font, gi, ctx->font->xadv * x, y * ctx->font->yadv, &ctx->screen[y][x], data, width, height);
			}
		}
	}
	return 0;
	(void)stride;
	(void)size;
}

static int draw_frame(term_ctx_t *ctx) {
	int32_t width = ctx->width;
	int32_t height = ctx->height;
	int32_t stride = width * sizeof(uint32_t);
	int32_t size = stride * height;
	uint32_t *data = NULL;

	int fd = allocate_shm_file(size);
	if(fd < 0) {
		return -1;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(data == MAP_FAILED) {
		close(fd);
		return -1;
	}

	for(int32_t y = 0; y < height; ++y) {
		for(int32_t x = 0; x < width; ++x) {
			data[y * width + x] = ctx->def_bg;
		}
	}

	if(ctx->disable_harfbuzz) {
		render_term_text_ft(ctx, width, height, stride, size, data);
	} else {
		render_term_text_hb(ctx, width, height, stride, size, data);
	}

	if(ctx->mode & TERM_MODE_SHOW_CURSOR) {
		render_char(ctx->font, ctx->cursor, ctx->font->xadv * ctx->col, ctx->row * ctx->font->yadv, data, width, height, ctx->def_fg);
	}

	munmap(data, size);
	return fd;
}
/*
static void draw_button(FT_Face face, widget_button_t *btn, uint32_t *data, int32_t w, int32_t h, int32_t stride) {
	int32_t x = 0;
	int32_t y = 0;
	uint32_t bg = btn->is_hovered ? btn->hbg : btn->bg;
	uint32_t fg = btn->is_hovered ? btn->hfg : btn->fg;

	if(btn->anchor == WIDGET_LEFT) {
		x = btn->x;
	} else if(btn->anchor == WIDGET_RIGHT) {
		x = w + btn->x;
	} else if(btn->anchor == WIDGET_CENTER) {
		x = w / 2 - btn->w / 2;
	}

	for(uint32_t yp = y; yp < y + btn->h; yp++) {
		for(uint32_t xp = x; xp < x + btn->w; xp++) {
			put_pixel(data, xp, yp, w, h, bg);
		}
	}

	for(uint32_t i = 0; btn->label[i]; i++) {
		render_char(face, btn->label[i], 16, x + 10 / 2, y, data, w, h, fg);
	}
}

static void draw_label(FT_Face face, widget_label_t *label, void *data, int32_t w, int32_t h) {
	int32_t x = 0;
	int32_t y = 0;
	uint32_t fg = label->fg;
	uint32_t x_advance = face->size->metrics.max_advance >> 6;

	if(label->anchor == WIDGET_LEFT) {
		x = label->x;
	} else if(label->anchor == WIDGET_RIGHT) {
		x = w + label->x;
	} else if(label->anchor == WIDGET_CENTER) {
		x = w / 2 - label->w / 2;
	}

	uint32_t labelwidth = x_advance * strlen(label->label);

	for(uint32_t i = 0; label->label[i]; i++) {
		render_char(face, label->label[i], 16, (x - labelwidth / 2) + i * x_advance, y, data, w, h, fg);
	}
}
*/

uint32_t tty_read_utf32(int fd) {
	uint8_t b1 = 0;
	uint8_t extbytes[3] = { 0 };
	read(fd, &b1, 1);

	if(b1 < 0x80) {
		return (uint32_t)b1;
	} else if((b1 & 0xe0) == 0xc0) {
		read(fd, extbytes, 1);
		return (((uint32_t)(b1 & 0x1f) << 6) | (uint32_t)(extbytes[0] & 0x3f));
	} else if((b1 & 0xf0) == 0xe0) {
		read(fd, extbytes, 2);
		return (((uint32_t)(b1 & 0x0f) << 12) | ((uint32_t)(extbytes[0] & 0x3f) << 6) | (uint32_t)(extbytes[1] & 0x3f));
	} else if((b1 & 0xf8) == 0xf0) {
		read(fd, extbytes, 3);
		return (((uint32_t)(b1 & 0x07) << 18) | ((uint32_t)(extbytes[0] & 0x3f) << 12) | ((uint32_t)(extbytes[1] & 0x3f) << 6) | ((uint32_t)extbytes[2] & 0x3f));
	}

	log_warn("Invalid UTF-8 sequence returning as CTRL char %x\n", b1);
	return b1;
}

void term_event(term_ctx_t *term) {
	struct pollfd pfd = { term->ptmx, POLLIN, 0 };
	uint32_t c = 0;
	while(poll(&pfd, 1, 0)) {
		if(pfd.revents & POLLHUP || pfd.revents & POLLERR) {
			log_error("poll: %s\n", strerror(errno));
			term->running = 0;
			break;
		} else if(pfd.revents & POLLIN) {
			c = tty_read_utf32(term->ptmx);
			if(c == UTF8_ESCAPE) {
				vt_escape_process(term);
				continue;
			}
			if(c == '\a') {
				continue;
			}
			if(c == '\b') {
				if(term->col)
					term->col--;
				continue;
			}
			if(c == '\r') {
				term->col = 0;
				continue;
			}
			if(term->col >= term->max_cols) {
				term->row++;
				term->col = 0;
			}
			if(term->row >= term->max_rows) {
				for(int32_t i = 1; i < term->max_rows; i++) {
					memcpy(term->screen[i-1], term->screen[i], term->max_cols * sizeof(term_cell_t));
				}
				term->row = term->max_rows - 1;
				for(int32_t i = 0; i < term->max_cols; i++) {
					term->screen[term->row][i].utf32 = ' ';
				}
			}
			if(c == '\t') {
				for(int32_t i = 0; i < 8 - (term->col % 8); ++i) {
					term->screen[term->row][term->col + i].utf32 = ' ';
				}
				term->col += 8 - (term->col % 8);
				continue;
			}
			if(c == 0x84 || c == '\n') {
				term->row++;
				continue;
			}
			if(c == '\r') {
				term->col = 0;
				continue;
			}
			term->screen[term->row][term->col].utf32 = c;
			term->screen[term->row][term->col].fg = term->fg;
			term->screen[term->row][term->col].bg = term->bg;
			term->screen[term->row][term->col].attributes = term->attributes;
			term->col++;
		}
	}

	int fd = draw_frame(term);
	if(fd == -1) {
		log_error("draw_frame failed: %s\n", strerror(errno));
		term->running = 0;
		return;
	}
	term->dpy->attach_shm(term->dpy, fd, term->width, term->height, term->width * 4, term->width * 4 * term->height, 0, FORMAT_ARGB8888);
	close(fd);
}

static void send_arrow_key(term_ctx_t *term, char c) {
	char csi_buffer[4] = "\x1b[0";
	char ss3_buffer[4] = "\x1bO0";

	if(term->mode & TERM_MODE_APP_CURSOR_KEYS) {
		ss3_buffer[2] = c;
		write(term->ptmx, ss3_buffer, strlen(ss3_buffer));
	} else {
		csi_buffer[2] = c;
		write(term->ptmx, csi_buffer, strlen(csi_buffer));
	}
}

static void term_free_screen(term_cell_t **scr, int32_t rows) {
	for(int32_t r = 0; r < rows; r++) {
		free(scr[r]);
	}
	free(scr);
}

static term_cell_t **term_allocate_screen(int32_t rows, int32_t cols) {
	term_cell_t **new = malloc(rows * sizeof(term_cell_t*));
	if(new == NULL) return NULL;

	for(int32_t r = 0; r < rows; r++) {
		new[r] = malloc(cols * sizeof(term_cell_t));
		if(new[r] == NULL) {
			term_free_screen(new, r);
			return NULL;
		}
	}

	return new;
}

static void term_mouse_report(term_ctx_t *term, bool motion, uint32_t btn, uint32_t state, int32_t x, int32_t y) {
	char buffer[64] = { 0 };
	size_t len = 0;
	uint32_t i = 0;
	uint8_t c = 0;
	x /= term->font->xadv;
	y /= term->font->yadv;

	if(motion) {
		for(i = 0; i < 3; i++) {
			if(term->button_state & (1 << i)) {
				break;
			}
		}

		c += 32 + i;
	} else {
		c += state ? btn - 1 : 3;
	}


	if(term->mode & TERM_MODE_MOUSE_SGR) {
		len = snprintf(buffer, 64, "\x1b[<%d;%d;%d%c", c, x+1, y+1, (state | motion) ? 'M' : 'm');
		write(term->ptmx, buffer, len);
		return;
	}
	
	if(term->mode & TERM_MODE_MOUSE_X10 && state == 0) return;
	if(x < 223 && y < 223) return;

	if(term->mode & (TERM_MODE_MOUSE_X10 | TERM_MODE_MOUSE_NORMAL | TERM_MODE_MOUSE_MOTION_ALL | TERM_MODE_MOUSE_BUTTON)) {
		snprintf(buffer, 64, "\x1b[M%c%c%c", ' '+c, ' '+x+1, ' '+y+1);
		write(term->ptmx, buffer, 6);
	}
}

void term_handle_motion(void *data, int32_t x, int32_t y) {
	term_ctx_t *term = (term_ctx_t*)data;

	term->x = x;
	term->y = y;

	if((term->button_state && term->mode & TERM_MODE_MOUSE_BUTTON) || term->mode & TERM_MODE_MOUSE_MOTION_ALL) {
		term_mouse_report(term, true, 0, 0, term->x, term->y);
	}
}

void term_handle_pointer_focus(void *data, uint32_t state) {
	char buffer[4] = "\x1b[O";
	term_ctx_t *term = (term_ctx_t*)data;

	if(state) buffer[2] = 'I';

	if(term->mode & TERM_MODE_MOUSE_FOCUS) {
		write(term->ptmx, buffer, 3);
	}
}

void term_handle_button(void *data, uint32_t button, uint32_t state) {
	term_ctx_t *term = (term_ctx_t*)data;

	if(button > 3) {
		printf("Unrecognized button %d\n", button);
	}

	if(state) {
		term->button_state |= 1 << (button - 1);
	} else {
		term->button_state &= ~(1<<(button - 1));
	}
	if(term->mode & (TERM_MODE_MOUSE_NORMAL | TERM_MODE_MOUSE_BUTTON | TERM_MODE_MOUSE_MOTION_ALL | TERM_MODE_MOUSE_X10)) {
		term_mouse_report(term, false, button, state, term->x, term->y);
	}
}

void term_handle_configure(void *data, uint32_t width, uint32_t height) {
	term_ctx_t *term = data;
	term->width = width;
	term->height = height;
	int32_t rows = term->height / term->font->yadv;
	int32_t cols = term->width / term->font->xadv;

	if(rows != term->max_rows || cols != term->max_cols) {
		term_cell_t **new_primary = term_allocate_screen(rows, cols);
		term_cell_t **new_alt = term_allocate_screen(rows, cols);
		for(int32_t r = 0; r < rows; ++r) {
			for(int32_t c = 0; c < cols; ++c) {
				new_primary[r][c].utf32 = ' ';
				new_primary[r][c].fg = term->fg;
				new_primary[r][c].bg = term->bg;
				new_primary[r][c].attributes = 0;
				new_alt[r][c].utf32 = ' ';
				new_alt[r][c].fg = term->fg;
				new_alt[r][c].bg = term->bg;
				new_alt[r][c].attributes = 0;
			}
		}
		for(int32_t r = 0; r < MIN(term->max_rows, rows); r++) {
				memcpy(new_primary[r], term->primary[r], MIN(cols, term->max_cols) * sizeof(term_cell_t));
				memcpy(new_alt[r], term->altscreen[r], MIN(cols, term->max_cols) * sizeof(term_cell_t));
		}
		if(term->screen == term->altscreen) {
			term->screen = new_alt;
		} else {
			term->screen = new_primary;
		}
		term_free_screen(term->primary, term->max_rows);
		term_free_screen(term->altscreen, term->max_rows);
		term->primary = new_primary;
		term->altscreen = new_alt;
		term->max_cols = cols;
		term->max_rows = rows;
	}

	if(term->max_cols <= term->col) term->col = term->max_cols - 1;
	if(term->max_rows <= term->row) term->row = term->max_rows - 1;

	struct winsize wsz = { rows, cols, width, height };
	ioctl(term->ptmx, TIOCSWINSZ, &wsz);

	int fd = draw_frame(term);
	if(fd == -1) {
		log_error("draw_frame failed: %s\n", strerror(errno));
		term->running = 0;
		return;
	}
	term->dpy->attach_shm(term->dpy, fd, term->width, term->height, term->width * 4, term->width * 4 * term->height, 0, FORMAT_ARGB8888);
	close(fd);
}

void term_handle_cliboard_str(void *data, const char *str) {
	term_ctx_t *term = (term_ctx_t*)data;

	if(term->mode & TERM_MODE_BRACKTED_PASTE) {
		write(term->ptmx, TERM_BRACKTED_PASTE_START_STR, strlen(TERM_BRACKTED_PASTE_START_STR));
	}

	write(term->ptmx, str, strlen(str));

	if(term->mode & TERM_MODE_BRACKTED_PASTE) {
		write(term->ptmx, TERM_BRACKTED_PASTE_END_STR, strlen(TERM_BRACKTED_PASTE_END_STR));
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

int term_handle_key_application(term_ctx_t *ctx, uint32_t key) {
	xkb_keysym_t keysym = xkb_state_key_get_one_sym(ctx->state, key);

	for(uint32_t i = 0; i < sizeof(app_keypad) / sizeof(app_keypad[0]); i++) {
		if(keysym == app_keypad[i].keysym) {
			write(ctx->ptmx, app_keypad[i].str, strlen(app_keypad[i].str));
			return 1;
		}
	}
	return 0;
}

void term_handle_key(void *data, uint32_t key, uint32_t state) {
	term_ctx_t *term = data;
	xkb_keysym_t keysym = 0;
	char utf8[5] = { 0 };

	if(state == 0) {
		return;
	}
	if(term->mode & TERM_MODE_APP_KEYPAD) {
		if(term_handle_key_application(term, key)) {
			return;
		}
	}

	xkb_mod_mask_t shift = xkb_keymap_mod_get_index(term->keymap, XKB_MOD_NAME_SHIFT);
	xkb_mod_mask_t ctrl = xkb_keymap_mod_get_index(term->keymap, XKB_MOD_NAME_CTRL);

	if(xkb_state_mod_indices_are_active(term->state, XKB_STATE_MODS_DEPRESSED, XKB_STATE_MATCH_ALL, shift, ctrl, XKB_MOD_INVALID)) {
		if(xkb_state_key_get_one_sym(term->state, key) == XKB_KEY_V) {
			term->dpy->request_cliboard_text(term->dpy);
			return;
		}
	}

	keysym = xkb_state_key_get_one_sym(term->state, key);
	if(keysym == XKB_KEY_Up) {
		send_arrow_key(term, 'A');
	} else if(keysym == XKB_KEY_Down) {
		send_arrow_key(term, 'B');
	} else if(keysym == XKB_KEY_Left) {
		send_arrow_key(term, 'D');
	} else if(keysym == XKB_KEY_Right) {
		send_arrow_key(term, 'C');
	} else {
		/*Convert to UTF8*/
		xkb_state_key_get_utf8(term->state, key, utf8, 5);
		if(utf8[0] == 8) {
			utf8[0] = 127;
		}

		write(term->ptmx, utf8, strlen(utf8));
	}

	term->dirty = 1;
}

void term_handle_keymap(void *data, struct xkb_keymap *keymap, struct xkb_state *state) {
	term_ctx_t *term = data;

	term->state = state;
	term->keymap = keymap;
}

void term_handle_close(void *data) {
	term_ctx_t *term = data;

	term->running = 0;
}

/*
static int btn_is_in(widget_button_t *btn, int32_t x, int32_t y, int32_t w, int32_t h) {
	int32_t bx = 0;
	int32_t by = btn->y;
	int32_t bx1 = 0;
	int32_t by1 = 0;

	if(btn->anchor == WIDGET_LEFT) {
		bx = btn->x;
	} else if(btn->anchor == WIDGET_RIGHT) {
		bx = w + btn->x;
	} else if(btn->anchor == WIDGET_CENTER) {
		bx = w / 2 + btn->w / 2 + btn->x;
	}

	bx1 = bx + btn->w;
	by = btn->y;
	by1 = by + btn->h;

	if(x >= bx && x <= bx1 && y >= by && y <= by1) {
		return 1;
	}

	return 0;
}
*/
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

int main(int argc, char **argv) {
	srand(time(NULL));
	int parent = 0;
	int child = 0;
	const char *font_name = "monospace";

	log_set_level(TERM_LOG_LEVEL_INFO, 0);
	log_set_file(stderr);

	term_ctx_t *term = calloc(1, sizeof(term_ctx_t));
	if(!term) {
		log_error("calloc failed: %s\n", strerror(errno));
		return -1;
	}
	term->bg = BG_COLOR;
	term->fg = FG_COLOR;
	vt_setcursor_shape(term, 2);

	term->max_rows = INITIAL_ROW_MAX;
	term->max_cols = INITIAL_COLUMN_MAX;

	term->features[0].tag = HB_TAG('c', 'a', 'l', 't');
	term->features[0].value = 1;
	term->features[0].start = HB_FEATURE_GLOBAL_START;
	term->features[0].end = HB_FEATURE_GLOBAL_END;

	for(int i = 1; i < argc; ++i) {
		if(strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			free(term);
			return -1;
		} else if(strcmp(argv[i], "--font-name") == 0) {
			if(i == argc - 1) {
				printf("Argument expected for --font-name\n");
				goto err_free_term;
			}
			font_name = argv[i+1];
		} else if(strcmp(argv[i], "--bg-color") == 0) {
			if(i == argc - 1) {
				printf("Argument expected for --bg-color\n");
				goto err_free_term;
			}
			if(strtou32(argv[i+1], 16, &term->bg) == -1) {
				printf("error strtou32: %s\n", strerror(errno));
				goto err_free_term;
			}
		} else if(strcmp(argv[i], "--fg-color") == 0) {
			if(i == argc - 1) {
				printf("Argument expected for --fg-color\n");
				goto err_free_term;
			}
			if(strtou32(argv[i+1], 16, &term->fg) == -1) {
				printf("error strtou32: %s\n", strerror(errno));
				goto err_free_term;
			}
		} else if(strcmp(argv[i], "--disable-harfbuzz") == 0) {
			term->disable_harfbuzz = true;
		} else if(strcmp(argv[i], "--disable-ligatures") == 0) {
			term->features[0].value = false;
		}
	}
	term->primary = term_allocate_screen(term->max_rows, term->max_cols);
	term->altscreen = term_allocate_screen(term->max_rows, term->max_cols);
	term->screen = term->primary;
	vt_fallback_color_table(term);
	vt_clear_screen(term);

	term->def_fg = term->fg;
	term->def_bg = term->bg;
	term->mode |= TERM_MODE_SHOW_CURSOR;
	term->font = term_font_from_name(font_name, 16);
	if(term->font == NULL) {
		log_error("font creation failed\n");
		goto err_free_term;
	}

	if(getpty(&parent, &child) == -1) {
		log_error("getpty failed: %s\n", strerror(errno));
		goto err_font_destroy;
	}

	if(forkshell(parent, child) == -1) {
		log_error("forkshell failed: %s\n", strerror(errno));
		close(child);
		goto err_close_pty;
	}

	struct winsize wsz = { term->max_rows, term->max_cols, INITIAL_WIN_WIDTH, INITIAL_WIN_HEIGHT };
	ioctl(term->ptmx, TIOCSWINSZ, &wsz);


#if defined(TERM_WL_SUPPORT)
	if(getenv("WAYLAND_DISPLAY")) {
		term->dpy = term_wl_display_init();
	}
#endif
#if defined(TERM_X11_SUPPORT)
	if(term->dpy == NULL && getenv("DISPLAY")) {
		term->dpy = term_x11_display_init();
	}
#endif

	if(term->dpy == NULL) {
		log_error("Failed to create display\n");
		return -1;
	}

	term->dpy->data = term;
	term->ptmx = parent;
	term->running = 1;
	term->dpy->callbacks.keymap_change = term_handle_keymap;
	term->dpy->callbacks.keypress = term_handle_key;
	term->dpy->callbacks.close = term_handle_close;
	term->dpy->callbacks.configure = term_handle_configure;
	term->dpy->callbacks.clipboard_str_callback = term_handle_cliboard_str;
	term->dpy->callbacks.pointer_motion = term_handle_motion;
	term->dpy->callbacks.pointer_button = term_handle_button;
	term->dpy->callbacks.pointer_focus = term_handle_pointer_focus;
	struct pollfd pfds[1] = { 0 };

	pfds[0].events = POLLIN;
	pfds[0].fd = term->ptmx;

	term->width = INITIAL_WIN_WIDTH;
	term->height = INITIAL_WIN_HEIGHT;

	while(term->running) {
		term->dpy->dispatch(term->dpy);
		poll(pfds, 1, 50);
		if(pfds[0].revents & POLLIN) {
			term_event(term);
		} else if(pfds[0].revents & (POLLHUP | POLLERR)) {
			term->running = 0;
			break;
		}
		if(term->dirty) {
			if(term->dirty == 2) continue;
			int fd = draw_frame(term);
			if(fd == -1) {
				log_error("draw_frame failed: %s\n", strerror(errno));
				term->running = 0;
			}
			term->dpy->attach_shm(term->dpy, fd, term->width, term->height, term->width * 4, term->width * 4 * term->height, 0, FORMAT_ARGB8888);
			close(fd);
			term->dirty = 0;
		}
	}

	term_free_screen(term->primary, term->max_rows);
	term_free_screen(term->altscreen, term->max_rows);

	term->dpy->deinit(term->dpy);
	term_font_destroy(term->font);
	
	close(term->ptmx);
	free(term);

	return 0;
err_close_pty:
	close(parent);
err_font_destroy:
	term_font_destroy(term->font);
err_free_term:
	free(term);
	return -1;
}
