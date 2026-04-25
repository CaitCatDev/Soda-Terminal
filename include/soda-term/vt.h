#ifndef __TERM_VT_H__
#define __TERM_VT_H__

#include <stdint.h>

#include <sys/types.h>
#include <soda-term/font.h>
#include <soda-term/display.h>

typedef uint32_t utf32_t;

#define TERM_STATE_GROUND 0
#define TERM_STATE_ESC_START 1
#define TERM_STATE_CSI_SEQUENCE 2
#define TERM_STATE_DCS_SEQUENCE 3
#define TERM_STATE_OCS_SEQEUNCE 4
#define TERM_STATE_SET_G0_CSET 6

/*VT 100 doesn't wrap when input is inserted
 * the last column of a line. That only sets
 * this flag and it wraps when it prints the
 * character. e.g. "\x1b[1;COL_MAXHZ\b" would
 * cause the cursor position to be max_col - 2
 */
#define TERM_FLAGS_NEED_WRAP (1)

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
#define TERM_CELL_ATTRIBUTE_DIRTY (1 << 11)

#define TERM_MODE_BRACKTED_PASTE (1 << 0)
#define TERM_MODE_APP_KEYPAD (1 << 1)
#define TERM_MODE_APP_CURSOR_KEYS (1 << 2)
#define TERM_MODE_SHOW_CURSOR (1 << 3)
#define TERM_MODE_ALT_SCREEN (1 << 4)
#define TERM_MODE_RESIZE_NOTIFY (1 << 5)
#define TERM_MODE_VT52 (1 << 6)
#define TERM_MODE_ORIGIN (1 << 7)
#define TERM_MODE_WRAPAROUND (1 << 8)
#define TERM_MODE_REVERSE_VIDEO (1 << 9)

#define TERM_MODE_MOUSE_X10 (1 << 1)
#define TERM_MODE_MOUSE_NORMAL (1 << 2)
#define TERM_MODE_MOUSE_BUTTON (1 << 3)
#define TERM_MODE_MOUSE_MOTION_ALL (1 << 4)
#define TERM_MODE_MOUSE_FOCUS (1 << 5)
#define TERM_MODE_MOUSE_SGR (1 << 6)

#define TERM_DECMODE_APP_CURSOR_KEYS 1
#define TERM_DECMODE_VT52_MODE 2
#define TERM_DECMODE_COLUMN_MODE 3
#define TERM_DECMODE_SMOOTH_SCROLL_MODE 4
#define TERM_DECMODE_REVERSE_VIDEO_MODE 5
#define TERM_DECMODE_ORIGIN_MODE 6
#define TERM_DECMODE_AUTO_WRAP 7
#define TERM_DECMODE_AUTO_REPEAT 8
#define TERM_DECMODE_MOUSE_X10 9
#define TERM_DECMODE_SHOW_CURSOR 25
#define TERM_DECMODE_MOUSE_NORMAL 1000
#define TERM_DECMODE_MOUSE_BUTTON 1002
#define TERM_DECMODE_MOUSE_ANY 1003
#define TERM_DECMODE_MOUSE_FOCUS 1004
#define TERM_DECMODE_MOUSE_SGR 1006
#define TERM_DECMODE_ALTSCREEN 1049
#define TERM_DECMODE_BRACKETED_PASTE 2004
#define TERM_DECMODE_IN_BAND_RESIZE 2048

#define TERM_KBMODE_MODIFY_OTHER_KEYS (1)

#define DCS_BUFFER_LEN 4096
#define CSI_BUFFER_LEN 128
#define COLORTABLE_LEN 256
#define CSI_MAX_PARAM 16

#define IN_RANGE(x, l, h) (x >= l && x <= h)
#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)
#define CTRL_CODE(c) ((c <= 0x1f) | IN_RANGE(c, 0x80, 0x9f))

#define MAKE_ARGB(a, r, g, b) (((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | ((uint32_t)b))

typedef struct {
	utf32_t utf32;
	uint32_t fg;
	uint32_t bg;
	uint32_t attributes;
} vt_cell_t;

typedef struct {
	int32_t x;
	int32_t y;
} vt_cursor_t;

typedef struct vt_line {
	uint32_t dirty;
	vt_cell_t *cells;
} vt_line_t;

typedef struct vt_ctx_s {
	int ptmx;
	pid_t child;

	vt_cursor_t cursor_pos;
	vt_cursor_t saved_cursor;

	uint32_t def_fg;
	uint32_t def_bg;
	uint32_t colortable[COLORTABLE_LEN];
	uint8_t csi_buffer[CSI_BUFFER_LEN];
	uint32_t csi_len;
	uint8_t strescape_buffer[DCS_BUFFER_LEN];

	int32_t yabs;
	int32_t max_cols;
	int32_t max_rows;
	int32_t top;
	int32_t bottom;

	uint32_t *tabstops;
	vt_line_t *primary;
	vt_line_t *alt;
	vt_line_t *screen;
	vt_cell_t cursor;
	utf32_t last_char;

	int32_t history_rows;
	vt_line_t *history;

	uint32_t fg;
	uint32_t bg;
	uint32_t attributes;

	uint32_t state;
	uint32_t term_mode;
	uint32_t flags;
	uint32_t charset;
	uint32_t keyboard_mode;
	uint32_t pointer_mode;
} vt_ctx_t;

void vt52_escape_process(vt_ctx_t *vt, uint8_t escape, uint8_t y, uint8_t x);
void vt_csi_exec(vt_ctx_t *term, const char *csi, uint32_t len);
void vt_free_screen(vt_line_t *scr, int32_t rows);
void vt_setcursor_shape(vt_ctx_t *term, uint32_t shape);
vt_line_t *vt_allocate_screen(int32_t rows, int32_t cols);
void vt_scroll(vt_ctx_t *vt);
void vt_handle_ctrl_code(vt_ctx_t *vt, uint32_t code);
vt_ctx_t *vt_init(uint32_t fg, uint32_t bg);
void vt_deinit(vt_ctx_t *vt);
void vt_clear_screen(vt_ctx_t *term);
int vt_event(vt_ctx_t *vt);

#endif
