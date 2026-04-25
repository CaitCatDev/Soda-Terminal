#ifndef __TERM_DISPLAY_H__
#define __TERM_DISPLAY_H__

#include <stdint.h>
#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>

typedef struct soda_shm_buffer {
	void *data;
	void (*buffer_freed)(void *buffer);
	int32_t width, height;
	int32_t stride, size;
	uint32_t format;
	int fd;
	bool in_use;
	bool free_after_use;
} soda_shm_buffer_t;

typedef struct soda_display soda_display_t;
typedef struct {
	void (*keymap_change)(void *data, struct xkb_keymap *keymap, struct xkb_state *state);
	void (*pointer_motion)(void *data, int32_t x, int32_t y);
	void (*pointer_button)(void *data, uint32_t btn, uint32_t state);
	void (*pointer_focus)(void *data, uint32_t state);
	void (*keypress)(void *data, uint32_t key, uint32_t state);
	void (*repeat_info)(void *data, int32_t rate, int32_t delay);
	void (*configure)(void *data, uint32_t width, uint32_t height);
	void (*clipboard_str_callback)(void *data, const char *str);
	void (*close)(void *data);
	void (*redraw)(void *data);
} soda_display_callbacks_t;

struct soda_display {
	int (*attach_shm)(soda_display_t *dpy, soda_shm_buffer_t *buffer);
	void (*dispatch)(soda_display_t *display);
	void (*request_frame_callback)(soda_display_t *display);
	void (*deinit)(soda_display_t *display);
	int (*display_fds)(soda_display_t *dpy, int **fds);
	void (*request_cliboard_text)(soda_display_t *display);
	soda_display_callbacks_t callbacks;
	void *data;
};

soda_display_t *soda_wl_display_init(void);
soda_display_t *soda_x11_display_init(void);

#endif
