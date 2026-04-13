#ifdef __FREEBSD__
#define __BSD_VISIBLE 1
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <poll.h>
#include <pwd.h>
#if defined(__linux__)
#include <pty.h>
#endif

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H

#include <hb.h>
#include <hb-ft.h>

#include <fontconfig/fontconfig.h>
#include <xkbcommon/xkbcommon.h>

#include <term/display.h>

#if defined(__FreeBSD__)
#include <dev/evdev/input-event-codes.h>
#elif defined(__linux__)
#include <linux/input-event-codes.h>
#endif

#include "freetype/freetype.h"

#define FORMAT_ARGB8888 0
#define FORMAT_XRGB8888 1

#define ROW_MAX 30
#define COLUMN_MAX 100
#define BG_COLOR 0xff000000
#define FG_COLOR 0xfff8f8f2
#define CSD_BG_COLOR 0xffd3d3d3
#define CSD_FG_COLOR 0xff000000
#define UTF8_ESCAPE 0x1b

#define CURSOR_MOVE_RIGHT "[C"
#define CURSOR_CLEAR_INLINE "[K"
#define CURSOR_HOME_STR "[H"
#define CLEAR_SCREEN_STR "[2J"
#define CSDS_HEIGHT 20

#define IN_RANGE(x, l, h) (x >= l && x <= h)

#define WIDGET_LEFT 0
#define WIDGET_RIGHT 1
#define WIDGET_CENTER 2

typedef uint32_t utf32_t;

static const uint32_t term_pallete[] = {
	0xff000000,
	0xffff0000,
	0xff00ff00,
	0xffffff00,
	0xff0000ff,
	0xffff00ff,
	0xff00ffff,
	0xffffffff,
};

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

	FT_Library library;
	FT_Face face;
	uint32_t advance;
	hb_font_t *hb_font;
	hb_feature_t features[1];
	bool disable_harfbuzz;

	struct xkb_keymap *keymap;
	struct xkb_state *state;

	term_display_t *dpy;

	/*Hardcoded to 30rows 100cols*/
	term_cell_t screen[ROW_MAX][COLUMN_MAX];
	uint32_t col;
	uint32_t row;
	uint32_t fg;
	uint32_t bg;
	utf32_t cursor;
	uint32_t def_fg;
	uint32_t def_bg;
	uint32_t width;
	uint32_t height;
} term_ctx_t;

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
		printf("posix_openpt: %s\n", strerror(errno));
		return -1;
	}

	if(grantpt(p) < 0) {
		close(p);
		printf("grantpt: %s\n", strerror(errno));
		return -1;
	}

	if(unlockpt(p) < 0) {
		close(p);
		printf("unlockpt: %s\n", strerror(errno));
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
			printf("ptsname: %s\n", strerror(errno));
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
	setenv("TERM", "xterm", 1);

	char *args[2] = { shell, NULL };

	if(setsid() < 0) {
		printf("error setsid: %s\n", strerror(errno));
		close(fd);
		exit(1);
	}

	if(ioctl(fd, TIOCSCTTY, NULL) == -1) {
		printf("error ioctl(TIOCSCTTY): %s\n", strerror(errno));
		close(fd);
		exit(1);
	}

	struct termios termios = { 0 };
	if(tcgetattr(fd, &termios) < 0) {
		printf("error tcgetattr: %s\n", strerror(errno));
		close(fd);
		exit(1);
	}
	termios.c_iflag |= IUTF8;

	if(tcsetattr(fd, TCSANOW, &termios) == -1) {
		printf("error tcsetattr: %s\n", strerror(errno));
		close(fd);
		exit(1);
	}

	dup2(fd, STDIN_FILENO);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	close(fd);

	if(execvp(shell, args) < 0) {
		printf("error execve: %s\n", strerror(errno));
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
		printf("getpwuid failed: %s\n", strerror(errno));
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
	srand(time(NULL));

	do {
		for(uint32_t i = 1; i < 5; i++) {
			template[i] = '0' + rand() % 10;
		}
		fd = shm_open(template, O_RDWR | O_CREAT | O_EXCL, S_IWUSR | S_IRUSR);
	} while(fd < 0 && errno == EEXIST);
	if(fd < 0) {
		printf("error shm_open: %s\n", strerror(errno));
		return -1;
	}
	shm_unlink(template);

	do {
	 res = posix_fallocate(fd, 0, size);
	} while(res < 0 && errno == EINTR);
	if(res < 0) {
		printf("error posix_fallocate: %s\n", strerror(errno));
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

static void render_glyph(FT_Face face, uint32_t glyph_index, uint32_t sz, uint32_t x, uint32_t y, uint32_t *data, uint32_t w, uint32_t h, uint32_t fg) {
	FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
	FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
	FT_GlyphSlot glyph = face->glyph;

	for(uint32_t cy = 0; cy < glyph->bitmap.rows; cy++) {
		for(uint32_t cx = 0; cx < glyph->bitmap.width; cx++) {
			if(glyph->bitmap.buffer[cy * glyph->bitmap.pitch + cx]) {
				float alpha = (float)glyph->bitmap.buffer[cy * glyph->bitmap.pitch + cx] / 255.0f;
				uint32_t px = get_pixel(data, x + cx + glyph->bitmap_left, y + sz + cy - glyph->bitmap_top, w, h);
				px = alpha_blend(fg, px, alpha);
				put_pixel(data, x + cx + glyph->bitmap_left, sz + y + cy - glyph->bitmap_top, w, h, px);
			}
		}
	}
}

static void render_char(FT_Face face, uint32_t utf, uint32_t sz, uint32_t x, uint32_t y, uint32_t *data, uint32_t w, uint32_t h, uint32_t fg) {
	uint32_t glyph_index = FT_Get_Char_Index(face, utf);
	render_glyph(face, glyph_index, sz, x, y, data, w, h, fg);
}

static void render_term_cell(FT_Face face, uint32_t glyph_index, uint32_t sz, uint32_t x, uint32_t y, term_cell_t *cell, uint32_t *data, uint32_t w, uint32_t h) {
	FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
	FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
	FT_GlyphSlot glyph = face->glyph;

	for(uint32_t cy = 0; cy < face->size->metrics.height >> 6; cy++) {
		for(uint32_t cx = 0; cx < face->size->metrics.max_advance >> 6; cx++) {
			put_pixel(data, x + cx, y + cy, w, h, cell->bg);
		}
	}
	for(uint32_t cy = 0; cy < glyph->bitmap.rows; cy++) {
		for(uint32_t cx = 0; cx < glyph->bitmap.width; cx++) {
			float alpha = (float)glyph->bitmap.buffer[cy * glyph->bitmap.pitch + cx] / 255.0f;
			uint32_t px = alpha_blend(cell->fg, cell->bg, alpha);
			put_pixel(data, x + cx + glyph->bitmap_left, sz + y + cy - glyph->bitmap_top, w, h, px);
		}
	}
}

static int render_term_text_hb(term_ctx_t *ctx, int32_t width, int32_t height, int32_t stride, int32_t size, uint32_t *data) {
	for(uint32_t i = 0; i < ROW_MAX; i++) {
		hb_buffer_t *buf = hb_buffer_create();
		if(hb_buffer_allocation_successful(buf) == false) {
			printf("hb_buffer_create failed: %s\n", strerror(errno));
			return -1;
		}
		for(uint32_t x = 0; x < COLUMN_MAX; x++) {
			if(ctx->screen[i][x].utf32 == 0) break;
			hb_buffer_add_utf32(buf, &ctx->screen[i][x].utf32, 1, 0, -1);
		}
		hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
		hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
		hb_buffer_set_language(buf, hb_language_from_string("en", -1));

		hb_shape(ctx->hb_font, buf, ctx->features, 1);
		unsigned int glyph_count = 0;
		hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
		hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(buf, &glyph_count);
		for(uint32_t j = 0; j < glyph_count; j++) {
			hb_codepoint_t glyphid = glyph_info[j].codepoint;
			hb_position_t x_advance = glyph_pos[j].x_advance >> 6;
			render_term_cell(ctx->face, glyphid, 16, j * x_advance, 20 * i, &ctx->screen[i][j], data, width, height);
		}
		hb_buffer_destroy(buf);
	}

	return 0;
	(void)size;
	(void)stride;
}

static int render_term_text_ft(term_ctx_t *ctx, int32_t width, int32_t height, int32_t stride, int32_t size, uint32_t *data) {
	for(uint32_t y = 0; y < ROW_MAX; ++y) {
		for(uint32_t x = 0; x < COLUMN_MAX; ++x) {
			if(ctx->screen[y][x].utf32) {
				FT_Set_Pixel_Sizes(ctx->face, 16, 16);
				render_char(ctx->face, ctx->screen[y][x].utf32, 16, ctx->advance * x, y * 16, data, width, height, ctx->fg);
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

	render_char(ctx->face, ctx->cursor, 16, ctx->advance * ctx->col, ctx->row * 20, data, width, height, ctx->fg);

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
	for(uint32_t y = 0; y < ROW_MAX; y++) {
		for(uint32_t x = 0; x < COLUMN_MAX; x++) {
			ctx->screen[y][x].utf32 = 0;
			ctx->screen[y][x].attributes = 0;
			ctx->screen[y][x].fg = ctx->fg;
			ctx->screen[y][x].bg = ctx->bg;
		}
	}
}

void handle_csi(term_ctx_t *state) {
	char escape[128] = { 0 };
	uint32_t i = 1;
	escape[0] = '[';

	do {
		read(state->ptmx, &escape[i], 1);
		i++;
	} while(i < 127 && !IN_RANGE(escape[i-1], 0x40, 0x7f));


	if(escape[i-1] == 'n') {
		if(escape[i-2] == '6') {
			write(state->ptmx, "\x1b[0;0R", 6);
		} else if(escape[i-2] == '5') {
			write(state->ptmx, "\x1b[0n", 4);
		}
		return;
	} else if(escape[i-1] == 'H') {
		uint32_t r = strtoul(&escape[1], NULL, 10);
		uint32_t c = strtoul(&escape[strcspn(escape, ";")+1], NULL, 10);
		state->row = r;
		state->col = c;
	} else if(escape[i-1] == 'm') {
		uint32_t i = strtoul(&escape[1], NULL, 10);
		if(i >= 30 && i <= 37) {
			i -= 30;
			state->fg = term_pallete[i];
		} else if(i >= 40 && i <= 47) {
			i -= 40;
			state->bg = term_pallete[i];
		} else {
			state->screen[state->row][state->col].fg = state->def_fg;
			state->screen[state->row][state->col].bg = state->def_bg;
			state->screen[state->row][state->col].attributes = 0;
		}
	} else if(strcmp(CLEAR_SCREEN_STR, escape) == 0) {
		for(uint32_t y = 0; y < ROW_MAX; y++) {
			term_clear_screen(state);
		}
	} else if(strcmp(CURSOR_MOVE_RIGHT, escape) == 0) {
		state->col++;
	} else if(strcmp(CURSOR_CLEAR_INLINE, escape) == 0) {
		for(uint32_t i = state->col; i < COLUMN_MAX; i++) {
			state->screen[state->row][i].utf32 = 0;
		}
	} else {
		printf("Unknown Escape Sequence: %s\n", escape);
	}
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
	printf("Unknown Escape Sequence: %s\n", escape);
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
		printf("Unknown Escape Sequence: \\x1b(%c\n", escape);
		return;
	}
	printf("Unknown Escape Format: \\x1b%c\n", escape);
}

uint32_t tty_read_utf32(int fd) {
	uint8_t b1 = 0;
	uint8_t extbytes[3] = { 0 };
	read(fd, &b1, 1);

	if(b1 < 0x80) {
		return (uint32_t)b1;
	} else if((b1 & 0xe0) == 0xc0) {
		read(fd, extbytes, 1);
		return (((uint32_t)b1 << 6) | (uint32_t)(extbytes[0] & 0x3f));
	} else if((b1 & 0xf0) == 0xe0) {
		read(fd, extbytes, 2);
		return (((uint32_t)b1 << 12) | ((uint32_t)(extbytes[0] & 0x3f) << 6) | (uint32_t)(extbytes[1] & 0x3f));
	} else if((b1 & 0xf8) == 0xf0) {
		read(fd, extbytes, 3);
	}

	perror("UTF8 Decode Error: longer than 4bytes\n");
	return 0;
}

void term_event(term_ctx_t *term) {
	struct pollfd pfd = { term->ptmx, POLLIN, 0 };
	uint32_t c = 0;
	int r = 0;
	while((r = poll(&pfd, 1, 100))) {
		if(pfd.revents & POLLHUP || pfd.revents & POLLERR) {
			printf("error poll: %s\n", strerror(errno));
			term->running = 0;
			break;
		} else if(pfd.revents & POLLIN) {
			c = tty_read_utf32(term->ptmx);
			if(term->col >= COLUMN_MAX) {
				term->col = 0;
				term->row++;
			}
			if(term->row >= ROW_MAX) {
				for(uint32_t i = 1; i < ROW_MAX; i++) {
					memcpy(term->screen[i-1], term->screen[i], COLUMN_MAX * sizeof(term_cell_t));
				}
				term->row = ROW_MAX - 1;
				term->col = 0;
				memset(term->screen[term->row], 0, COLUMN_MAX * sizeof(term_cell_t));
			}
			if(c == UTF8_ESCAPE) {
				process_escape(term);
				continue;
			}
			if(c == '\t') {
				for(uint32_t i = 0; i < 8 - (term->col % 8); ++i) {
					term->screen[term->row][term->col + i].utf32 = ' ';
				}
				term->col += 8 - (term->col % 8);
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
			term->col++;
		}
	}

	int fd = draw_frame(term);
	if(fd == -1) {
		printf("draw_frame failed: %s\n", strerror(errno));
		term->running = 0;
		return;
	}
	term->dpy->attach_shm(term->dpy, fd, term->width, term->height, term->width * 4, term->width * 4 * term->height, 0, FORMAT_ARGB8888);
	close(fd);
}

static void send_csi(int ptmx, char c) {
	char buffer[4] = "\x1b[0";

	buffer[2] = c;
	write(ptmx, buffer, sizeof(buffer));
}

void term_handle_configure(void *data, uint32_t width, uint32_t height) {
	term_ctx_t *term = data;
	term->width = width;
	term->height = height;

	int fd = draw_frame(term);
	if(fd == -1) {
		printf("draw_frame failed: %s\n", strerror(errno));
		term->running = 0;
		return;
	}
	term->dpy->attach_shm(term->dpy, fd, term->width, term->height, term->width * 4, term->width * 4 * term->height, 0, FORMAT_ARGB8888);
	close(fd);
}

void term_handle_key(void *data, uint32_t key, uint32_t state) {
	term_ctx_t *term = data;
	xkb_keysym_t keysym = 0;
	char utf8[5] = { 0 };

	if(state == 0) {
		return;
	}

	keysym = xkb_state_key_get_one_sym(term->state, key);
	if(keysym == XKB_KEY_Up) {
		send_csi(term->ptmx, 'A');
	} else if(keysym == XKB_KEY_Down) {
		send_csi(term->ptmx, 'B');
	} else if(keysym == XKB_KEY_Left) {
		send_csi(term->ptmx, 'D');
	} else if(keysym == XKB_KEY_Right) {
		send_csi(term->ptmx, 'C');
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
	int parent = 0;
	int child = 0;
	FT_Error error = 0;
	const char *font_name = "monospace";
	term_ctx_t *term = calloc(1, sizeof(term_ctx_t));
	if(!term) {
		printf("calloc failed: %s\n", strerror(errno));
		return -1;
	}
	term->bg = BG_COLOR;
	term->fg = FG_COLOR;
	term->cursor = L'█';
	term_clear_screen(term);

	term->features[0].tag = HB_TAG('c', 'a', 'l', 't');
	term->features[0].value = 1;
	term->features[0].start = HB_FEATURE_GLOBAL_START;
	term->features[0].end = HB_FEATURE_GLOBAL_END;

	for(int i = 1; i < argc; ++i) {
		if(strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
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

	term->def_fg = term->fg;
	term->def_bg = term->bg;

	const char *fname = find_font_file(font_name);
	if(fname == NULL) {
		printf("error getting found file from fcconfig: %s\n", strerror(errno));
		goto err_free_term;
	}
	printf("Chosen Font: %s\n", fname);

	error = FT_Init_FreeType(&term->library);
	if(error) {
		free((void*)fname);
		printf("Freetype Library Init failed %s\n", FT_Error_String(error));
		goto err_free_term;
	}

	error = FT_New_Face(term->library, fname, 0, &term->face);
	free((void*)fname);
	if(error) {
		printf("Freetype Library Init failed %s\n", FT_Error_String(error));
		goto err_free_freetype;
	}
	FT_Set_Pixel_Sizes(term->face, 16, 16);
	term->advance = term->face->size->metrics.max_advance >> 6;

	term->hb_font = hb_ft_font_create_referenced(term->face);
	hb_ft_font_set_load_flags(term->hb_font, FT_LOAD_DEFAULT);

	if(getpty(&parent, &child) == -1) {
		printf("getpty failed: %s\n", strerror(errno));
		goto err_free_face;
	}

	if(forkshell(parent, child) == -1) {
		printf("forkshell failed: %s\n", strerror(errno));
		close(child);
		goto err_close_pty;
	}

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
		printf("Failed to create display\n");
		return -1;
	}

	term->dpy->data = term;
	term->ptmx = parent;
	term->running = 1;
	term->dpy->callbacks.keymap_change = term_handle_keymap;
	term->dpy->callbacks.keypress = term_handle_key;
	term->dpy->callbacks.close = term_handle_close;
	term->dpy->callbacks.configure = term_handle_configure;

	struct pollfd pfds[1] = { 0 };

	pfds[0].events = POLLIN;
	pfds[0].fd = term->ptmx;

	term->width = 800;
	term->height = 600;

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
