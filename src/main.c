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
#include <pwd.h>

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_OUTLINE_H

#include <hb.h>
#include <hb-ft.h>

#include <fontconfig/fontconfig.h>
#include <xkbcommon/xkbcommon.h>

#include <term/display.h>
#include <term/log.h>

#if defined(__FreeBSD__)
#include <dev/evdev/input-event-codes.h>
#elif defined(__linux__)
#include <linux/input-event-codes.h>
#endif

#include "freetype/freetype.h"

#define FORMAT_ARGB8888 0
#define FORMAT_XRGB8888 1

#define INITIAL_ROW_MAX 30
#define INITIAL_COLUMN_MAX 100
#define INITIAL_WIN_WIDTH 800
#define INITIAL_WIN_HEIGHT 600

#define CSI_MAX_PARAM 16

#define BG_COLOR 0xff000000
#define FG_COLOR 0xfff8f8f2
#define CSD_BG_COLOR 0xffd3d3d3
#define CSD_FG_COLOR 0xff000000
#define UTF8_ESCAPE 0x1b

#define CURSOR_MOVE_RIGHT "C"
#define CURSOR_CLEAR_INLINE "K"
#define CURSOR_HOME_STR "H"
#define CLEAR_SCREEN_STR "2J"
#define CSDS_HEIGHT 20

#define IN_RANGE(x, l, h) (x >= l && x <= h)
#define MIN(a, b) (a < b ? a : b)

#define WIDGET_LEFT 0
#define WIDGET_RIGHT 1
#define WIDGET_CENTER 2

typedef uint32_t utf32_t;

static uint32_t cursors[] = { L'█', L'_', L'|' };

typedef struct {
	utf32_t utf32;
	uint32_t fg;
	uint32_t bg;
	uint32_t attributes;
} term_cell_t;

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

typedef struct term_ctx_s {
	int ptmx;
	int running;

	FcConfig *fcconfig;
	FT_Library library;
	FT_Face face;
	uint32_t x_advance;
	uint32_t y_advance;
	hb_font_t *hb_font;
	hb_feature_t features[1];
	bool disable_harfbuzz;

	struct xkb_keymap *keymap;
	struct xkb_state *state;

	term_display_t *dpy;

	term_cell_t **primary;
	term_cell_t **altscreen;
	term_cell_t **screen;
	int32_t col;
	int32_t row;
	int32_t saved_col;
	int32_t saved_row;
	int32_t max_cols;
	int32_t max_rows;

	uint32_t fg;
	uint32_t bg;
	uint32_t attributes;

	utf32_t cursor;
	uint32_t def_fg;
	uint32_t def_bg;
	uint32_t colortable[256];
	uint32_t width;
	uint32_t height;
	uint32_t mode;
} term_ctx_t;

#define TERM_MODE_BRACKTED_PASTE (1 << 0)
#define TERM_MODE_APP_KEYPAD (1 << 1)
#define TERM_MODE_APP_CURSOR_KEYS (1 << 2)
#define TERM_MODE_SHOW_CURSOR (1 << 3)
#define TERM_MODE_ALT_SCREEN (1 << 4)

#define TERM_BRACKTED_PASTE_START_STR "\x1b[200~"
#define TERM_BRACKTED_PASTE_END_STR "\x1b[201~"

const char *find_font_file(const char *name) {
	FcConfig *config = NULL;
	FcPattern *pattern, *font;
	FcResult res = 0;
	FcChar8 *filename = NULL;
	unsigned long len = 0;
	char *output = NULL;

	config = FcInitLoadConfigAndFonts();
	if(config == NULL) {
		return NULL;
	}

	pattern = FcNameParse((const FcChar8*)name);
	if(pattern == NULL) {
		goto __error_config;
	}

	if(FcConfigSubstitute(config, pattern, FcMatchPattern) == FcFalse) {
		goto __error_pattern;
	}
	FcDefaultSubstitute(pattern);


	font = FcFontMatch(config, pattern, &res);
	if(font) {
		if(FcPatternGetString(font, FC_FILE, 0, &filename) == FcResultMatch) {
			len = strlen((char*)filename);
			output = calloc(1, len + 1);
			if(output) {
				memcpy(output, filename, len);
			}
		}
		FcPatternDestroy(font);
	}

__error_pattern:
	FcPatternDestroy(pattern);
__error_config:
	FcConfigDestroy(config);
	return output;
}

int getpty(int *parent, int *child) {
	int p = -1;
	int c = -1;
	char *child_path = NULL;

	if(parent == NULL || child == NULL) {
		errno = EINVAL;
		return -1;
	}

	p = posix_openpt(O_RDWR | O_NOCTTY);
	if(p < 0) {
		log_error("posix_openpt: %s\n", strerror(errno));
		return -1;
	}

	if(grantpt(p) < 0) {
		close(p);
		log_error("grantpt: %s\n", strerror(errno));
		return -1;
	}

	if(unlockpt(p) < 0) {
		close(p);
		log_error("unlockpt: %s\n", strerror(errno));
		return -1;
	}

#if defined(TIOCGPTPEER)
	/* If available try Linux's TIOCGPTPEER allocation method
	 * it seems ptsname method can be disabled by disabling
	 * CONFIG_UNIX98_PTYS kernel option. So try the TIOCGPTPEER
	 * first and fallback to ptsname if it doesn't work or isn't present
	 */
	c = ioctl(p, TIOCGPTPEER, O_RDWR | O_NOCTTY);
#endif
	if(c < 0) {
		child_path = ptsname(p);
		if(child_path == NULL) {
			close(p);
			log_error("ptsname: %s\n", strerror(errno));
			return -1;
		}
		c = open(child_path, O_RDWR | O_NOCTTY);
	}
	/*both approaches failed*/
	if(c < 0) {
		close(p);
		return -1;
	}
	
	*child = c;
	*parent = p;
	return 0;
}

static void child_proc_exec(const struct passwd *pw, char *shell, int fd) {
	unsetenv("COLUMNS");
	unsetenv("LINES");
	setenv("USER", pw->pw_name, 1);
	setenv("SHELL", shell, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("TERM", "project-term", 1);

	char *args[2] = { shell, NULL };

	if(setsid() < 0) {
		log_error("setsid: %s\n", strerror(errno));
		close(fd);
		exit(1);
	}

	if(ioctl(fd, TIOCSCTTY, NULL) == -1) {
		log_error("ioctl(TIOCSCTTY): %s\n", strerror(errno));
		close(fd);
		exit(1);
	}

	struct termios termios = { 0 };
	if(tcgetattr(fd, &termios) < 0) {
		log_error("tcgetattr: %s\n", strerror(errno));
		close(fd);
		exit(1);
	}
	termios.c_iflag |= IUTF8;

	if(tcsetattr(fd, TCSANOW, &termios) == -1) {
		log_error("tcsetattr: %s\n", strerror(errno));
		close(fd);
		exit(1);
	}

	dup2(fd, STDIN_FILENO);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	close(fd);

	if(execvp(shell, args) < 0) {
		log_error("execve: %s\n", strerror(errno));
		close(fd);
	}
	exit(1);
}

int forkshell(int parent, int child) {
	pid_t pid;
	char *shell = getenv("SHELL");
	const struct passwd *pw;

	pw = getpwuid(getuid());
	if(pw == NULL) {
		log_error("getpwuid failed: %s\n", strerror(errno));
		return -1;
	}

	if(shell == NULL && pw->pw_shell[0]) {
		shell = pw->pw_shell;
	}

	if(shell == NULL) {
		printf("Neither $SHELL or pw->pw_shell have a shell set\n");
		return -1;
	}
	pid = fork();

	if(pid < 0) {
		printf("error fork: %s\n", strerror(errno));
		return -1;
	} else if(pid == 0) {
		close(parent);
		child_proc_exec(pw, shell, child);
	}

	close(child);
	return 0;
}

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

#define MAKE_ARGB(r, g, b) ((uint32_t)0xff000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | ((uint32_t)b))

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

static void render_glyph(FT_Face face, uint32_t glyph_index, uint32_t x, uint32_t y, uint32_t *data, uint32_t w, uint32_t h, uint32_t fg, uint32_t yadv) {
	FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
	FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
	FT_GlyphSlot glyph = face->glyph;

	for(uint32_t cy = 0; cy < glyph->bitmap.rows; cy++) {
		for(uint32_t cx = 0; cx < glyph->bitmap.width; cx++) {
			float alpha = (float)glyph->bitmap.buffer[cy * glyph->bitmap.pitch + cx] / 255.0f;
			uint32_t px = get_pixel(data, x + cx + glyph->bitmap_left, y + yadv + cy - glyph->bitmap_top, w, h);
			px = alpha_blend(fg, px, alpha);
			put_pixel(data, x + cx + glyph->bitmap_left, yadv + y + cy - glyph->bitmap_top, w, h, px);
		}
	}
}

static void render_char(FT_Face face, uint32_t utf, uint32_t x, uint32_t y, uint32_t *data, uint32_t w, uint32_t h, uint32_t fg, uint32_t yadv) {
	uint32_t glyph_index = FT_Get_Char_Index(face, utf);
	render_glyph(face, glyph_index, x, y, data, w, h, fg, yadv);
}

static void render_term_cell(FT_Face face, uint32_t glyph_index, uint32_t x, uint32_t y,
														 term_cell_t *cell, uint32_t *data, uint32_t w, uint32_t h, uint32_t xadv, uint32_t yadv) {
	FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
	if(cell->attributes == 1 && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
		FT_Outline_Embolden(&face->glyph->outline, 1 * 64);
	}
	FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
	FT_GlyphSlot glyph = face->glyph;

	for(uint32_t cy = 0; cy < yadv; cy++) {
		for(uint32_t cx = 0; cx < xadv; cx++) {
			put_pixel(data, x + cx, yadv + y + cy, w, h, cell->bg);
		}
	}
	for(uint32_t cy = 0; cy < glyph->bitmap.rows; cy++) {
		for(uint32_t cx = 0; cx < glyph->bitmap.width; cx++) {
			float alpha = (float)glyph->bitmap.buffer[cy * glyph->bitmap.pitch + cx] / 255.0f;
			uint32_t px = alpha_blend(cell->fg, cell->bg, alpha);
			put_pixel(data, x + cx + glyph->bitmap_left, yadv + y + cy - glyph->bitmap_top, w, h, px);
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
		}
		hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
		hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
		hb_buffer_set_language(buf, hb_language_from_string("en", -1));

		hb_shape(ctx->hb_font, buf, ctx->features, 1);
		unsigned int glyph_count = 0;
		hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
		for(uint32_t j = 0; j < glyph_count; j++) {
			hb_codepoint_t glyphid = glyph_info[j].codepoint;
			render_term_cell(ctx->face, glyphid, j * ctx->x_advance, ctx->y_advance * i, &ctx->screen[i][j], data, width, height, ctx->x_advance, ctx->y_advance);
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
				uint32_t gi = FT_Get_Char_Index(ctx->face, ctx->screen[y][x].utf32);
				render_term_cell(ctx->face, gi, ctx->x_advance * x, y * ctx->y_advance, &ctx->screen[y][x], data, width, height, ctx->x_advance, ctx->y_advance);
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
		render_char(ctx->face, ctx->cursor, ctx->x_advance * ctx->col, ctx->row * ctx->y_advance, data, width, height, ctx->def_fg, ctx->y_advance);
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

void term_clear_screen(term_ctx_t *ctx) {
	for(int32_t y = 0; y < ctx->max_rows; y++) {
		for(int32_t x = 0; x < ctx->max_cols; x++) {
			ctx->screen[y][x].utf32 = ' ';
			ctx->screen[y][x].attributes = 0;
			ctx->screen[y][x].fg = ctx->fg;
			ctx->screen[y][x].bg = ctx->bg;
		}
	}
}

void term_set_attributes(term_ctx_t *term, uint32_t *params, uint32_t pcount) {
	if(pcount == 0) {
		term->attributes = 0;
		term->fg = term->def_fg;
		term->bg = term->def_bg;
		return;
	}

	for(uint32_t p = 0; p < pcount; ++p) {
		uint32_t param = params[p];
		switch(param) {
			case 0:
				term->attributes = 0;
				term->fg = term->def_fg;
				term->bg = term->def_bg;
				break;
			case 1:
				term->attributes |= 1;
				break;
			case 2:
				break;
			case 3:
				break;
			case 4:
				break;
			case 5:
				break;
			case 7:
				break;
			case 8:
				break;
			case 9:
				break;
			case 22:
				term->attributes &= ~(1);
				break;
			case 23:
				break;
			case 24:
				break;
			case 25:
				break;
			case 27:
				break;
			case 28:
				break;
			case 29:
				break;
			case 30:
			case 31:
			case 32:
			case 33:
			case 34:
			case 35:
			case 36:
			case 37:
				term->fg = term->colortable[param - 30];
				break;
			case 38:
				switch(params[p+1]) {
					case 2:
						term->fg = MAKE_ARGB(params[p + 2], params[p + 3], params[p + 4]);
						p+=4;
						break;
					case 5:
						term->fg = term->colortable[params[p + 2] & 0xff];
						p+=2;
						break;
					default:
						break;
				}
			break;
			case 39:
				term->fg = term->def_fg;
				break;
			case 40:
			case 41:
			case 42:
			case 43:
			case 44:
			case 45:
			case 46:
			case 47:
				term->bg = term->colortable[param - 40];
				break;
			case 48:
				switch(params[p+1]) {
					case 2:
						term->bg = MAKE_ARGB(params[p + 2], params[p + 3], params[p + 4]);
						p+=4;
						break;
					case 5:
						term->bg = term->colortable[params[p + 2] & 0xff];
						p+=2;
						break;
					default:
						break;
				}
				break;
			case 49:
				term->bg = term->def_bg;
				break;
			default:
				if(IN_RANGE(param, 90, 97)) {
					term->fg = term->colortable[param-90+8];
				} else if(IN_RANGE(param, 100, 107)) {
					term->fg = term->colortable[param-100+8];
				}
				break;
		}
	}
}

void exec_csi(term_ctx_t *term, const char *csi, uint32_t len) {
	uint8_t private = 0;
	uint8_t intermediate = 0;
	uint8_t mode = 0;
	uint32_t param_count = 0;
	uint32_t parameters[CSI_MAX_PARAM] = { 0 };
	uint32_t i = 0;

	if(csi[i] >= '<' && csi[i] <= '?') {
		private = csi[i];
		i++;
	}

	const char *p = &csi[i];
	while(p < csi + len) {
		char *np = NULL;
		if(param_count == CSI_MAX_PARAM) break;
		if(!IN_RANGE(p[0], '0', ';')) break;
		while(p[0] == ':' || p[0] == ';') {
			param_count++;
			p++;
		}

		parameters[param_count] = strtoul(p, &np, 10);
		p = np;
		param_count++;
		if(*p == ';' || *p == ':') p++;
	}

	if(IN_RANGE(p[0], 0x20, 0x2f)) {
		intermediate = p[0];
		p++;
	}
	mode = p[0];

	switch(private) {
		case '<':
			goto unknown_csi;
			break;
		case '=':
			goto unknown_csi;
			break;
		case '>':
			goto unknown_csi;
			break;
		case '?':
			switch(mode) {
				case 'h':
					if(parameters[0] == 1) {
						term->mode |= (TERM_MODE_APP_CURSOR_KEYS);
					} else if(parameters[0] == 1049) {
						term->mode |= (TERM_MODE_ALT_SCREEN);
						term->screen = term->altscreen;
						term->saved_row = term->row;
						term->saved_col = term->col;
					} else if(parameters[0] == 2004) {
						term->mode |= (TERM_MODE_BRACKTED_PASTE);
					} else if(parameters[0] == 25) {
						term->mode |= (TERM_MODE_SHOW_CURSOR);
					} else {
						goto unknown_csi;
					}
					break;
				case 'l':
					if(parameters[0] == 1) {
						term->mode &= ~(TERM_MODE_APP_CURSOR_KEYS);
					} else if(parameters[0] == 1049) {
						term->mode &= ~(TERM_MODE_ALT_SCREEN);
						term->col = term->saved_col;
						term->row = term->saved_row;
						term->screen = term->primary;
					} else if(parameters[0] == 2004) {
						term->mode &= ~(TERM_MODE_BRACKTED_PASTE);
					} else if(parameters[0] == 25) {
						term->mode &= ~(TERM_MODE_SHOW_CURSOR);
					} else {
						goto unknown_csi;
					}
					break;
				default:
					goto unknown_csi;
					break;
			}
			break;
		default:
			switch(mode) {
				case 'H':
					if(param_count == 0) {
						term->row = 0;
						term->col = 0;
					} else {
						term->row = parameters[0] ? parameters[0]-1 : parameters[0];
						term->col = parameters[1]-1;
					}
					break;
				case 'J':
					switch (parameters[0]) {
						case 0:
							for(int32_t r = term->row; r < term->max_rows; r++) {
								for(int32_t c = r == 0 ? term->col : 0; c < term->max_cols; c++) {
									term->screen[r][c].utf32 = ' ';
									term->screen[r][c].bg = term->bg;
									term->screen[r][c].fg = term->fg;
								}
							}
							break;
						case 1:
							for(int32_t r = term->row; r > 0; r--) {
								for(int32_t c = r == 0 ? term->col : term->max_cols; c > 0; c--) {
									term->screen[r-1][c-1].utf32 = ' ';
									term->screen[r-1][c-1].bg = term->bg;
									term->screen[r-1][c-1].fg = term->fg;
								}
							}
							break;
						case 2:
							term_clear_screen(term);
							break;
						case 3:
							log_warn("Erase Scrollback TODO\n");
							break;
						default:
							goto unknown_csi;
					}
					break;
				case 'K':
					switch(parameters[0]) {
						case 0:
							for(int32_t c = term->col; c < term->max_cols; c++) {
								term->screen[term->row][c].utf32 = ' ';
							}
							break;
						case 1:
							for(int32_t c = 0; c < term->col; c++) {
								term->screen[term->row][c].utf32 = ' ';
							}
							break;
						case 2:
							for(int32_t c = 0; c < term->max_cols; c++) {
								term->screen[term->row][c].utf32 = ' ';
							}
							break;
						default:
							goto unknown_csi;
					}
					break;
				case 'n':
					switch(parameters[0]) {
						case 5: /*Device status report hardcode response 0 aka OK*/
							write(term->ptmx, "\x1b[0n", strlen("\x1b[0n"));
							break;
						default: goto unknown_csi;
					}
					break;
				case 'c':
					write(term->ptmx, "\x1b[?1;0c", strlen("\x1b[?1;0c"));
					break;
				case 'A':
					if(parameters[0] == 0) parameters[0]++;
					while(parameters[0]--) {
						term->row--;
					};
					break;
				case 'B':
					if(parameters[0] == 0) parameters[0]++;
					while(parameters[0]--) {
						term->row++;
					}
					break;
				case 'C':
					if(parameters[0] == 0) parameters[0]++;
					while(parameters[0]--) {
						term->col++;
					}
					break;
				case 'D':
					if(parameters[0] == 0) parameters[0]++;
					while(parameters[0]--) {
						term->col--;
					}
					break;
				case 'G':
					if(parameters[0] == 0) parameters[0]++;
					term->col = parameters[0] - 1;
					break;
				case 'X':
					if(parameters[0] == 0) parameters[0]++;
					for(uint32_t k = term->col; k < term->col + parameters[0]; ++k) {
						term->screen[term->row][k].utf32 = ' ';
					}
					break;
				case 'd':
					if(parameters[0] == 0) parameters[0]++;
					term->row = parameters[0] - 1;
					break;
				case 'm':
					term_set_attributes(term, parameters, param_count);
					break;
				case 'r':
					log_warn("TODO set scroll region\n");
					term->row = 0;
					term->col = 0;
					break;
				case 'q':
					switch(intermediate) {
						case ' ':
							if(parameters[1] < 3) {
								term->cursor = cursors[0];
							} else if(parameters[1] < 5) {
								term->cursor = cursors[1];
							} else {
								term->cursor = cursors[2];
							}
					}
				default:
					goto unknown_csi;
			}
			break;
	}

	return;
unknown_csi:
	log_debug("Unknown CSI(%s):\n\tPrivate: %c(%x)\n\tIntermediate: %c(%x)\n\tMode: %c\n\tParameters: [", csi, private, private, intermediate, intermediate, mode);
	for(uint32_t p = 0; p < param_count; ++p) {
		printf(" %d,", parameters[p]);
	}
	printf("]\n");
}

void handle_csi(term_ctx_t *state) {
	char escape[128] = { 0 };
	uint32_t i = 0;

	do {
		read(state->ptmx, &escape[i], 1);
		i++;
	} while(i < 127 && !IN_RANGE(escape[i-1], 0x40, 0x7f));

	exec_csi(state, escape, i);
}

void handle_strescape(term_ctx_t *state, char byte) {
	char escape[4096] = { 0 };
	uint32_t i = 1;
	escape[0] = byte;

	do {
		read(state->ptmx, &escape[i], 1);
		if(escape[i] == '\a') break;
		if(strcmp(&escape[i-1], "\x1b\\") == 0) break;
		i++;
	} while(i < 4095);
	log_debug("Unknown Escape Sequence: %s\n", escape);
}


void process_escape(term_ctx_t *state) {
	char escape = 0;

	read(state->ptmx, &escape, 1);
	if(escape == '[') {
		handle_csi(state);
		return;
	} else if(escape == ']' || escape == 'P') {
		handle_strescape(state, escape);
		return;
	} else if(escape == '(') {
		read(state->ptmx, &escape, 1);
		return;
	} else if(escape == '=') {
		state->mode |= TERM_MODE_APP_KEYPAD;
	} else if(escape == '>') {
		state->mode &= ~(TERM_MODE_APP_KEYPAD);
	} else if(escape == '7') {
		state->saved_col = state->col;
		state->saved_row = state->row;
	} else if(escape == '8') {
		state->col = state->saved_col;
		state->row = state->saved_row;
	} else {
		log_debug("Unknown Escape Format: \\x1b%c\n", escape);
	}
}

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

	log_error("UTF8 sequence longer than 4 bytes");
	return 0;
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
				process_escape(term);
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
			if(term->col == term->max_cols) {
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
			if(c == '\n') {
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
		write(term->ptmx, ss3_buffer, sizeof(ss3_buffer));
	} else {
		csi_buffer[2] = c;
		write(term->ptmx, csi_buffer, sizeof(csi_buffer));
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

void term_handle_configure(void *data, uint32_t width, uint32_t height) {
	term_ctx_t *term = data;
	term->width = width;
	term->height = height;
	int32_t rows = term->height / term->y_advance;
	int32_t cols = term->width / term->x_advance;

	if(rows != term->max_rows && cols != term->max_cols) {
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
		if(term_handle_key_application(term, key))
			return;
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

void term_fallback_color_table(term_ctx_t *term) {
	/*Standard Term Colors [30-37m*/
	term->colortable[0] = 0xff000000;
	term->colortable[1] = 0xffc00000;
	term->colortable[2] = 0xff00c000;
	term->colortable[3] = 0xffc0c000;
	term->colortable[4] = 0xff0000c0;
	term->colortable[5] = 0xffc000c0;
	term->colortable[6] = 0xff00c0c0;
	term->colortable[7] = 0xffe1e1e1;

	/*Bright Term Colors [90-97m*/
	term->colortable[8] = 0xff808080;
	term->colortable[9] = 0xffff0000;
	term->colortable[10] = 0xff00ff00;
	term->colortable[11] = 0xffffff00;
	term->colortable[12] = 0xff0000ff;
	term->colortable[13] = 0xffff00ff;
	term->colortable[14] = 0xff00ffff;
	term->colortable[15] = 0xffffffff;

	for(uint32_t r = 0; r < 6; r++) {
		for(uint32_t g = 0; g < 6; g++) {
			for(uint32_t b = 0; b < 6; b++) {
				term->colortable[16 + 36 * r + 6 * g + b] = MAKE_ARGB(r * 41, g * 41, b * 41);
			}
		}
	}

	for(uint32_t i = 232; i <= 255; ++i) {
		uint8_t p = (uint32_t)(10.625 * (i - 232));
		term->colortable[i] = MAKE_ARGB(p, p, p);
	}
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
	FT_Error error = 0;
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
	term->cursor = cursors[0];

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
		} else if (strcmp(argv[i], "--disable-ligatures") == 0) {
			term->features[0].value = 0;
		}
	}
	term->primary = term_allocate_screen(term->max_rows, term->max_cols);
	term->altscreen = term_allocate_screen(term->max_rows, term->max_cols);
	term->screen = term->primary;
	term_fallback_color_table(term);
	term_clear_screen(term);

	term->def_fg = term->fg;
	term->def_bg = term->bg;
	term->mode |= TERM_MODE_SHOW_CURSOR;

	const char *fname = find_font_file(font_name);
	if(fname == NULL) {
		log_error("getting font file from fcconfig: %s\n", strerror(errno));
		goto err_free_term;
	}
	log_info("Chosen Font: %s\n", fname);

	error = FT_Init_FreeType(&term->library);
	if(error) {
		free((void*)fname);
		log_error("Freetype Library Init failed %s\n", FT_Error_String(error));
		goto err_free_term;
	}

	error = FT_New_Face(term->library, fname, 0, &term->face);
	free((void*)fname);
	if(error) {
		log_error("Freetype Library Init failed %s\n", FT_Error_String(error));
		goto err_free_freetype;
	}
	FT_Set_Pixel_Sizes(term->face, 16, 16);

	/*NotoSansMono Max Advanced is not the same as rest of font
	 *So use M glyph's advance this has the added benefit of making
	 *non monospaced also render better
	 */
	FT_Load_Char(term->face, 'M', FT_LOAD_DEFAULT);
	term->x_advance = term->face->glyph->metrics.horiAdvance >> 6;
	term->y_advance = term->face->size->metrics.height >> 6;
	term->hb_font = hb_ft_font_create_referenced(term->face);
	hb_ft_font_set_load_flags(term->hb_font, FT_LOAD_DEFAULT);

	if(getpty(&parent, &child) == -1) {
		log_error("getpty failed: %s\n", strerror(errno));
		goto err_free_face;
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

	struct pollfd pfds[1] = { 0 };

	pfds[0].events = POLLIN;
	pfds[0].fd = term->ptmx;

	term->width = INITIAL_WIN_WIDTH;
	term->height = INITIAL_WIN_WIDTH;

	while(term->running) {
		term->dpy->dispatch(term->dpy);
		poll(pfds, 1, 0);
		if(pfds[0].revents & POLLIN) {
			term_event(term);
		} else if(pfds[0].revents & (POLLHUP | POLLERR)) {
			term->running = 0;
			break;
		}
	}

	term_free_screen(term->primary, term->max_rows);
	term_free_screen(term->altscreen, term->max_rows);

	term->dpy->deinit(term->dpy);
	hb_font_destroy(term->hb_font);

	FT_Done_Face(term->face);
	FT_Done_FreeType(term->library);
	
	close(term->ptmx);
	free(term);

	return 0;
err_close_pty:
	close(parent);
err_free_face:
	FT_Done_Face(term->face);
err_free_freetype:
	FT_Done_FreeType(term->library);
err_free_term:
	free(term);
	return -1;
}
