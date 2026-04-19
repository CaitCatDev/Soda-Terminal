#ifndef __TERM_VT_H__
#define __TERM_VT_H__

#include <term/font.h>
#include <term/display.h>

typedef uint32_t utf32_t;

typedef struct {
	utf32_t utf32;
	uint32_t fg;
	uint32_t bg;
	uint32_t attributes;
} term_cell_t;

typedef struct term_ctx_s {
	int ptmx;
	int running;
	int dirty;

	term_font_t *font;
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
	int32_t x, y;
	uint32_t button_state;
} term_ctx_t;

#define TERM_CELL_ATTRIBUTE_NONE (0)
#define TERM_CELL_ATTRIBUTE_BOLD (1)
#define TERM_CELL_ATTRIBUTE_FAINT (1 << 1)
#define TERM_CELL_ATTRIBUTE_ITALIC (1 << 2)
#define TERM_CELL_ATTRIBUTE_UNDERLINE (1 << 3)
#define TERM_CELL_ATTRIBUTE_BLINK (1 << 4)
#define TERM_CELL_ATTRIBUTE_INVERSE (1 << 5)
#define TERM_CELL_ATTRIBUTE_INVIS (1 << 8)
#define TERM_CELL_ATTRIBUTE_CROSSED_OUT (1 << 9)
#define TERM_CELL_ATTRIBUTE_DBL_UNDERLINE (1 << 10)

#define TERM_MODE_BRACKTED_PASTE (1 << 0)
#define TERM_MODE_APP_KEYPAD (1 << 1)
#define TERM_MODE_APP_CURSOR_KEYS (1 << 2)
#define TERM_MODE_SHOW_CURSOR (1 << 3)
#define TERM_MODE_ALT_SCREEN (1 << 4)
#define TERM_MODE_MOUSE_X10 (1 << 5)
#define TERM_MODE_MOUSE_NORMAL (1 << 6)
#define TERM_MODE_MOUSE_BUTTON (1 << 7)
#define TERM_MODE_MOUSE_MOTION_ALL (1 << 8)
#define TERM_MODE_MOUSE_FOCUS (1 << 9)
#define TERM_MODE_MOUSE_SGR (1 << 10)

#define TERM_DECMODE_APP_CURSOR_KEYS 1
#define TERM_DECMODE_MOUSE_X10 2
#define TERM_DECMODE_SHOW_CURSOR 25
#define TERM_DECMODE_MOUSE_NORMAL 1000
#define TERM_DECMODE_MOUSE_BUTTON 1002
#define TERM_DECMODE_MOUSE_ANY 1003
#define TERM_DECMODE_MOUSE_FOCUS 1004
#define TERM_DECMODE_MOUSE_SGR 1006
#define TERM_DECMODE_ALTSCREEN 1049
#define TERM_DECMODE_BRACKETED_PASTE 2004

#define CSI_BUFFER_LEN 128
#define CSI_MAX_PARAM 16

#define IN_RANGE(x, l, h) (x >= l && x <= h)
#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)
#define MAKE_ARGB(r, g, b) ((uint32_t)0xff000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | ((uint32_t)b))


void vt_fallback_color_table(term_ctx_t *term);	
void vt_decsetmode(term_ctx_t *term, uint32_t mode);
void vt_decresetmode(term_ctx_t *term, uint32_t mode);
void vt_setcursor_pos(term_ctx_t *term, int32_t x, int32_t y);
void vt_setcursor_shape(term_ctx_t *term, uint32_t shape);
void vt_clear_screen(term_ctx_t *term);
void vt_clear_line(term_ctx_t *term, uint32_t mode);
void vt_set_sgr(term_ctx_t *term, uint32_t *params, uint32_t pcount);
void vt_escape_process(term_ctx_t *state);
int getpty(int *parent, int *child);
int forkshell(int parent, int child);

#endif
