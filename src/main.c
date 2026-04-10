#define _XOPEN_SOURCE 600

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

#include <wayland-util.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include <poll.h>
#include <pwd.h>
#include <pty.h>

#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H

#include <hb.h>
#include <hb-ft.h>

#include <fontconfig/fontconfig.h>

#include <xkbcommon/xkbcommon.h>

#include "../xdg-shell-client-protocol.h"
#include "freetype/freetype.h"

#define ROW_MAX 30
#define COLUMN_MAX 100
#define BG_COLOR 0xff000000
#define FG_COLOR 0xf8f8f8f2
#define UTF8_ESCAPE 0x1b
#define CURSOR_HOME_STR "[H"
#define CLEAR_SCREEN_STR "[2J"

#define IN_RANGE(x, l, h) (x >= l && x <= h)

typedef struct wayland_ctx_s {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct wl_compositor *compositor;
	struct xdg_wm_base *wm_base;
	struct wl_surface *wl_surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	int32_t width, height;
	int32_t size;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
} wayland_ctx_t;

typedef struct term_ctx_s {
	int ptmx;
	int running;

	FT_Library library;
	FT_Face face;
	uint32_t advance;
	hb_font_t *hb_font;
	hb_feature_t features[1];
	bool disable_harfbuzz;

	struct xkb_context *xkb_ctx;
	struct xkb_keymap *keymap;
	struct xkb_state *state;

	wayland_ctx_t *wl;
	/*Hardcoded to 30rows 100cols*/
	uint32_t screen[ROW_MAX][COLUMN_MAX];
	uint32_t col;
	uint32_t row;
	uint32_t fg;
	uint32_t bg;
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

#define SHELL_PATH "/bin/dash"

int forkshell(int parent, int child) {
	pid_t pid = fork();
	char *envp[] = { "TERM=xterm", "SHELL=/bin/dash", NULL };

	if(pid < 0) {
		printf("error fork: %s\n", strerror(errno));
		return -1;
	} else if(pid == 0) {
		close(parent);
		if(setsid() < 0) {
			printf("error setsid: %s\n", strerror(errno));
			close(child);
			exit(1);
		}

		if(ioctl(child, TIOCSCTTY, NULL) == -1) {
			printf("error ioctl(TIOCSCTTY): %s\n", strerror(errno));
			close(child);
			exit(1);
		}

		struct termios termios = { 0 };
		if(tcgetattr(child, &termios) < 0) {
			printf("error tcgetattr: %s\n", strerror(errno));
			exit(1);
		}
		termios.c_iflag |= IUTF8;

		if(tcsetattr(child, TCSANOW, &termios) == -1) {
			printf("error tcsetattr: %s\n", strerror(errno));
			exit(1);
		}

		dup2(child, STDIN_FILENO);
		dup2(child, STDOUT_FILENO);
		dup2(child, STDERR_FILENO);

		if(execle(SHELL_PATH, "-" SHELL_PATH, NULL, envp) < 0) {
			printf("error execve: %s\n", strerror(errno));
		}
		exit(1);
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
	}}

static void render_char(FT_Face face, uint32_t utf, uint32_t sz, uint32_t x, uint32_t y, uint32_t *data, uint32_t w, uint32_t h, uint32_t fg) {
	uint32_t glyph_index = FT_Get_Char_Index(face, utf);
	render_glyph(face, glyph_index, sz, x, y, data, w, h, fg);
}

void wl_buffer_release(void *data, struct wl_buffer *buffer) {
	wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

static int render_term_text_hb(term_ctx_t *ctx, int32_t width, int32_t height, int32_t stride, int32_t size, uint32_t *data) {
	for(uint32_t i = 0; i < ROW_MAX; i++) {
		hb_buffer_t *buf = hb_buffer_create();
		if(hb_buffer_allocation_successful(buf) == false) {
			printf("hb_buffer_create failed: %s\n", strerror(errno));
			return -1;
		}
		hb_buffer_add_utf32(buf, ctx->screen[i], -1, 0, -1);
		hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
		hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
		hb_buffer_set_language(buf, hb_language_from_string("en", -1));

		hb_shape(ctx->hb_font, buf, ctx->features, 1);
		unsigned int glyph_count = 0;
		hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
		hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(buf, &glyph_count);
		for(uint32_t j = 0; j < glyph_count; j++) {
			hb_codepoint_t glyphid = glyph_info[j].codepoint;
			hb_position_t x_offset  = glyph_pos[j].x_offset >> 6;
			hb_position_t y_offset  = glyph_pos[j].y_offset >> 6;
			hb_position_t x_advance = glyph_pos[j].x_advance >> 6;
			hb_position_t y_advance = glyph_pos[j].y_advance >> 6;
			render_glyph(ctx->face, glyphid, 16, j * x_advance, 16 * i, data, width, height, ctx->fg);
		}
		hb_buffer_destroy(buf);
	}

	return 0;
}

static int render_term_text_ft(term_ctx_t *ctx, int32_t width, int32_t height, int32_t stride, int32_t size, uint32_t *data) {
	for(uint32_t y = 0; y < ROW_MAX; ++y) {
		for(uint32_t x = 0; x < COLUMN_MAX; ++x) {
			if(ctx->screen[y][x]) {
				FT_Set_Pixel_Sizes(ctx->face, 16, 16);
				render_char(ctx->face, ctx->screen[y][x], 16, ctx->advance * x, y * 16, data, width, height, ctx->fg);
			}
		}
	}
	return 0;
}

static struct wl_buffer *draw_frame(term_ctx_t *ctx) {
	struct wl_buffer *buffer = NULL;
	struct wl_shm_pool *pool = NULL;
	int32_t width = ctx->wl->width;
	int32_t height = ctx->wl->height;
	int32_t stride = width * sizeof(uint32_t);
	int32_t size = stride * height;
	uint32_t *data = NULL;

	int fd = allocate_shm_file(size);
	if(fd < 0) {
		return NULL;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	pool = wl_shm_create_pool(ctx->wl->shm, fd, size);
	if(!pool) {
		close(fd);
		return NULL;
	}
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	for(int32_t y = 0; y < height; ++y) {
		for(int32_t x = 0; x < width; ++x) {
			data[y * width + x] = ctx->bg;
		}
	}

	if(ctx->disable_harfbuzz) {
		render_term_text_ft(ctx, width, height, stride, size, data);
	} else {
		render_term_text_hb(ctx, width, height, stride, size, data);
	}

	munmap(data, size);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	return buffer;
}

void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	term_ctx_t *state = (term_ctx_t*)data;

	state->running = 0;
}

void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states) {
	term_ctx_t *ctx = (term_ctx_t*)data;
	fprintf(stderr, "Configure: %d %d\n", ctx->wl->width, ctx->wl->height);

	ctx->wl->width = width ? width : 800;
	ctx->wl->height = height ? height : 600;

	/*TODO Change ROW_MAX and COLUMN MAX based on window/font size*/
	struct winsize wsz = { ROW_MAX, COLUMN_MAX, ctx->wl->width, ctx->wl->height };
	ioctl(ctx->ptmx, TIOCSWINSZ, &wsz);
}

void xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height) {

}

void xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel, struct wl_array *caps) {
	term_ctx_t *ctx = data;
	printf("wm caps size: %zu %zu\n", caps->size, caps->size / sizeof(uint32_t));
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.close = xdg_toplevel_close,
	.configure = xdg_toplevel_configure,
	.wm_capabilities = xdg_toplevel_wm_capabilities,
	.configure_bounds = xdg_toplevel_configure_bounds,
};

void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
	term_ctx_t *ctx = (term_ctx_t*)data;
	wayland_ctx_t *state = ctx->wl;
	xdg_surface_ack_configure(surface, serial);

	struct wl_buffer *buffer = draw_frame(data);
	if(buffer == NULL) {
		printf("draw_frame failed: %s\n", strerror(errno));
		ctx->running = 0;
		return;
	}

	wl_surface_attach(state->wl_surface, buffer, 0, 0);
	wl_surface_offset(state->wl_surface, 0, 0);
	wl_surface_damage_buffer(state->wl_surface, 0, 0, state->width, state->height);
	wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

void xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
	term_ctx_t *ctx = (term_ctx_t*)data;
	xdg_wm_base_pong(wm_base, serial);
	wl_display_flush(ctx->wl->display);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

void wl_surface_enter(void *data, struct wl_surface *surface, struct wl_output *output) {
}

void wl_surface_leave(void *data, struct wl_surface *surface, struct wl_output *output) {
}

void wl_surface_preffered_buffer_scale(void *data, struct wl_surface *surface, int32_t facator) {

}

void wl_surface_preffered_buffer_transform(void *data, struct wl_surface *surface, uint32_t transform) {

}

static const struct wl_surface_listener wl_surface_listener = {
	.enter = wl_surface_enter,
	.leave = wl_surface_leave,
	.preferred_buffer_scale = wl_surface_preffered_buffer_scale,
	.preferred_buffer_transform = wl_surface_preffered_buffer_transform,
};

void wl_shm_format(void *data, struct wl_shm *shm, uint32_t format) {

}

static const struct wl_shm_listener wl_shm_listener = {
	.format = wl_shm_format,
};

void wl_keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) {
	term_ctx_t *term = data;
	if(term->state) {
		xkb_state_unref(term->state);
		term->state = NULL;
	}
	if(term->keymap) {
		xkb_keymap_unref(term->keymap);
		term->keymap = NULL;
	}
	if(term->xkb_ctx) {
		xkb_context_unref(term->xkb_ctx);
		term->xkb_ctx = NULL;
	}
	char *buffer = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if(buffer == MAP_FAILED) {
		term->running = 0;
		return;
	}

	term->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	term->keymap = xkb_keymap_new_from_buffer(term->xkb_ctx, buffer, size, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	term->state = xkb_state_new(term->keymap);

	munmap(buffer, size);
}

void wl_keyboard_handle_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {

}

void wl_keyboard_handle_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {

}

void handle_csi(term_ctx_t *state) {
	char escape[128] = { 0 };
	uint32_t i = 1;
	escape[0] = '[';

	do {
		read(state->ptmx, &escape[i], 1);
		i++;
	} while(i < 127 && !IN_RANGE(escape[i-1], 0x40, 0x7f));

	if(strcmp(CURSOR_HOME_STR, escape) == 0) {
		state->col = 0;
		state->row = 0;
	} else if(strcmp(CLEAR_SCREEN_STR, escape) == 0) {
		for(uint32_t y = 0; y < ROW_MAX; y++) {
			memset(state->screen[y], 0, COLUMN_MAX*sizeof(uint32_t));
		}
	} else {
		fprintf(stderr, "Unknown Escape Sequence: %s\n", escape);
	}
}

void process_escape(term_ctx_t *state) {
	char escape = 0;

	read(state->ptmx, &escape, 1);
	if(escape == '[') {
		handle_csi(state);
		return;
	}
	printf("Unknown Escape Format: \\x1b%c\n", escape);
}

uint32_t tty_read_utf32(int fd) {
	uint32_t i = 0;
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
					memcpy(term->screen[i-1], term->screen[i], COLUMN_MAX * 4);
				}
				term->row = ROW_MAX - 1;
				term->col = 0;
				memset(term->screen[term->row], 0, COLUMN_MAX * 4);
			}
			if(c == UTF8_ESCAPE) {
				process_escape(term);
				continue;
			}
			if(c == '\t') {
				for(uint32_t i = 0; i < 8 - (term->col % 8); ++i) {
					term->screen[term->row][term->col + i] = ' ';
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

			term->screen[term->row][term->col] = c;
			term->col++;
		}
		c = 0;
	}

	struct wl_buffer *buffer = draw_frame(term);
	if(buffer == NULL) {
		printf("draw_frame failed: %s\n", strerror(errno));
		term->running = 0;
		return;
	}

	wl_surface_attach(term->wl->wl_surface, buffer, 0, 0);
	wl_surface_offset(term->wl->wl_surface, 0, 0);
	wl_surface_damage_buffer(term->wl->wl_surface, 0, 0, term->wl->width, term->wl->height);
	wl_surface_commit(term->wl->wl_surface);
}

void wl_keyboard_handle_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	term_ctx_t *term = data;
	char utf8[5] = { 0 };
	key += 8;

	if(state == 0) {
		return;
	}

	/*Convert to UTF8*/
	xkb_state_key_get_utf8(term->state, key, utf8, 5);
	if(utf8[0] == 8) {
		utf8[0] = 127;
	}

	write(term->ptmx, utf8, strlen(utf8));
}

void wl_keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
	term_ctx_t *ctx = data;

	xkb_state_update_mask(ctx->state, depressed, latched, locked, 0, 0, group);
}

void wl_keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {

}

static const struct wl_keyboard_listener wl_keyboard_listener = {
	.enter = wl_keyboard_handle_enter,
	.leave = wl_keyboard_handle_leave,
	.key = wl_keyboard_handle_key,
	.keymap = wl_keyboard_handle_keymap,
	.modifiers = wl_keyboard_handle_modifiers,
	.repeat_info = wl_keyboard_handle_repeat_info,
};

void wl_pointer_handle_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
}

void wl_pointer_handle_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {
}

void wl_pointer_handle_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
}

void wl_pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
}

void wl_pointer_handle_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {

}

void wl_pointer_handle_frame(void *data, struct wl_pointer *pointer) {

}

void wl_pointer_handle_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source) {

}

void wl_pointer_handle_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {

}

void wl_pointer_handle_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {

}

void wl_pointer_handle_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120) {

}

void wl_pointer_handle_axis_relative_direction(void *data, struct wl_pointer *pointer, uint32_t axis, uint32_t direction) {

}

static const struct wl_pointer_listener wl_pointer_listener = { 
	.enter = wl_pointer_handle_enter,
	.leave = wl_pointer_handle_leave,
	.motion = wl_pointer_handle_motion,
	.button = wl_pointer_handle_button,
	.axis = wl_pointer_handle_axis,
	.frame = wl_pointer_handle_frame,
	.axis_source = wl_pointer_handle_axis_source,
	.axis_stop = wl_pointer_handle_axis_stop,
	.axis_discrete = wl_pointer_handle_axis_discrete,
	.axis_value120 = wl_pointer_handle_axis_value120,
	.axis_relative_direction = wl_pointer_handle_axis_relative_direction,
};

void wl_seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	term_ctx_t *ctx = (term_ctx_t*)data;
	wayland_ctx_t *state = ctx->wl;
	printf("Seat Caps: %x\n", caps);

	if(caps & WL_SEAT_CAPABILITY_KEYBOARD) {
		if(state->keyboard == NULL) {
			state->keyboard = wl_seat_get_keyboard(seat);
			wl_keyboard_add_listener(state->keyboard, &wl_keyboard_listener, data);
		}
	} else if(state->keyboard) {
		wl_keyboard_destroy(state->keyboard);
		state->keyboard = NULL;
	}

	if(caps & WL_SEAT_CAPABILITY_POINTER) {
		if(state->pointer == NULL) {
			state->pointer = wl_seat_get_pointer(seat);
			wl_pointer_add_listener(state->pointer, &wl_pointer_listener, state);
		}
	} else if(state->pointer) {
		wl_pointer_destroy(state->pointer);
		state->pointer = NULL;
	}
}

void wl_seat_name(void *data, struct wl_seat *seat, const char *name) {
	printf("Seat Name: %s\n", name);
}

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = wl_seat_capabilities,
	.name = wl_seat_name,
};

void wl_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
	term_ctx_t *ctx = (term_ctx_t*)data;
	wayland_ctx_t *state = ctx->wl;
	if(strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version);
	} else if(strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, version);
		wl_shm_add_listener(state->shm, &wl_shm_listener, ctx);
	} else if(strcmp(interface, wl_seat_interface.name) == 0) {
		state->seat = wl_registry_bind(registry, name, &wl_seat_interface, version);
		wl_seat_add_listener(state->seat, &wl_seat_listener, ctx);
	} else if(strcmp(interface, xdg_wm_base_interface.name) == 0) {
		state->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, version);
		xdg_wm_base_add_listener(state->wm_base, &xdg_wm_base_listener, ctx);
	}
}

void wl_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {

}

static const struct wl_registry_listener wl_registry_listener = {
	.global = wl_registry_global,
	.global_remove = wl_registry_global_remove,
};

wayland_ctx_t *wayland_init(term_ctx_t *term) {
	wayland_ctx_t *ctx = calloc(1, sizeof(wayland_ctx_t));
	if(ctx == NULL) {
		return NULL;
	}
	term->wl = ctx;

	ctx->display = wl_display_connect(NULL);
	if(ctx->display == NULL) {
		printf("wl_display_connect failed: %s\n", strerror(errno));
		goto err_free_ctx;
	}

	ctx->registry = wl_display_get_registry(ctx->display);
	if(ctx->registry == NULL) {
		printf("wl_display_get_registry failed: %s\n", strerror(errno));
		goto err_disconnect;
	}
	wl_registry_add_listener(ctx->registry, &wl_registry_listener, term);

	if(wl_display_roundtrip(ctx->display) == -1) {
		printf("wl_display_roundtrip failed: %s\n", strerror(errno));
		goto err_free_globals;
	}

	if(ctx->compositor == NULL) {
		printf("no wl_compositor is a compositor running?\n");
		goto err_free_globals;
	}

	ctx->wl_surface = wl_compositor_create_surface(ctx->compositor);
	if(ctx->wl_surface == NULL) {
		printf("wl_compositor_create_surface failed: %s\n", strerror(errno));
		return NULL;
	}

	wl_surface_add_listener(ctx->wl_surface, &wl_surface_listener, term);

	ctx->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->wm_base, ctx->wl_surface);
	if(ctx->xdg_surface == NULL) {
		printf("xdg_wm_base_get_xdg_surface failed: %s\n", strerror(errno));
		return NULL;
	}
	xdg_surface_add_listener(ctx->xdg_surface, &xdg_surface_listener, term);

	ctx->xdg_toplevel = xdg_surface_get_toplevel(ctx->xdg_surface);
	if(ctx->xdg_surface == NULL) {
		printf("xdg_surface_get_toplevel failed: %s\n", strerror(errno));
		return NULL;
	}
	xdg_toplevel_add_listener(ctx->xdg_toplevel, &xdg_toplevel_listener, term);

	ctx->height = 600;
	ctx->width = 800;
	return ctx;

err_free_surface:
	if(ctx->xdg_toplevel) xdg_toplevel_destroy(ctx->xdg_toplevel);
	if(ctx->xdg_surface) xdg_surface_destroy(ctx->xdg_surface);
	if(ctx->wl_surface) wl_surface_destroy(ctx->wl_surface);

err_free_globals:
	if(ctx->compositor) wl_compositor_destroy(ctx->compositor);
	if(ctx->seat) wl_seat_destroy(ctx->seat);
	if(ctx->shm) wl_shm_destroy(ctx->shm);
	if(ctx->wm_base) xdg_wm_base_destroy(ctx->wm_base);
wl_registry_destroy(ctx->registry);
err_disconnect:
	wl_display_disconnect(ctx->display);
err_free_ctx:
	free(ctx);
	return NULL;
}

void wayland_deinit(wayland_ctx_t *wl) {
	wl_surface_attach(wl->wl_surface, NULL, 0, 0);
	wl_surface_commit(wl->wl_surface);
	wl_display_roundtrip(wl->display);
	wl_display_roundtrip(wl->display);


	wl_keyboard_destroy(wl->keyboard);
	wl_pointer_destroy(wl->pointer);

	xdg_toplevel_destroy(wl->xdg_toplevel);
	xdg_surface_destroy(wl->xdg_surface);
	xdg_wm_base_destroy(wl->wm_base);
	wl_surface_destroy(wl->wl_surface);

	wl_seat_destroy(wl->seat);
	wl_compositor_destroy(wl->compositor);
	wl_shm_destroy(wl->shm);

	wl_registry_destroy(wl->registry);
	wl_display_disconnect(wl->display);

	free(wl);
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

	term->ptmx = parent;
	term->running = 1;

	if(wayland_init(term) == NULL) {
		goto err_close_pty;
	}

	int ret = 0;
	struct pollfd pfds[2] = { 0 };

	pfds[0].events = POLLIN;
	pfds[0].fd = wl_display_get_fd(term->wl->display);
	pfds[1].events = POLLIN;
	pfds[1].fd = term->ptmx;

	wl_surface_commit(term->wl->wl_surface);
	wl_display_roundtrip(term->wl->display);
	while(term->running) {
		while(wl_display_prepare_read(term->wl->display) != 0) {
			wl_display_dispatch_pending(term->wl->display);
		}
		wl_display_flush(term->wl->display);

		ret = poll(pfds, 2, -1);
		if(pfds[0].revents & POLLIN) {
			wl_display_read_events(term->wl->display);
			wl_display_dispatch_pending(term->wl->display);
		} else if(pfds[0].revents & (POLLHUP | POLLERR)) {
			wl_display_cancel_read(term->wl->display);
			term->running = 0;
			break;
		} else {
			wl_display_cancel_read(term->wl->display);
		}

		if(pfds[1].revents & POLLIN) {
			term_event(term);
		} else if(pfds[1].revents & (POLLHUP | POLLERR)) {
			term->running = 0;
			break;
		}
	}

	wayland_deinit(term->wl);
	hb_font_destroy(term->hb_font);

	FT_Done_Face(term->face);
	FT_Done_FreeType(term->library);
	if(term->state) {
		xkb_state_unref(term->state);
	}
	if(term->keymap) {
		xkb_keymap_unref(term->keymap);
	}
	if(term->xkb_ctx) {
		xkb_context_unref(term->xkb_ctx);
	}
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
