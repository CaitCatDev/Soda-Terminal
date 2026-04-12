#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include <poll.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include <term/display.h>

#include <xkbcommon/xkbcommon.h>

#include "../xdg-shell-client-protocol.h"

typedef struct wl_ctx_s {
	term_display_t base;
	struct wl_display *display;
	struct wl_registry *registry;

	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;

	struct wl_keyboard *keyboard;
	struct xkb_context *ctx;
	struct xkb_keymap *keymap;
	struct xkb_state *state;

	struct wl_pointer *pointer;

	struct xdg_wm_base *wm_base;
	uint32_t width;
	uint32_t height;
} wayland_ctx_t;

void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	wayland_ctx_t *wl = (wayland_ctx_t*)data;
	if(wl->base.callbacks.close) {
		wl->base.callbacks.close(wl->base.data);
	}
}

void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states) {
	wayland_ctx_t *wl = (wayland_ctx_t*)data;

	wl->width = width ? width : 800;
	wl->height = height ? height : 600;
}

void xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height) {

}

void xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel, struct wl_array *caps) {
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.close = xdg_toplevel_close,
	.configure = xdg_toplevel_configure,
#if defined(XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
	.wm_capabilities = xdg_toplevel_wm_capabilities,
#endif
#if defined(XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION)
	.configure_bounds = xdg_toplevel_configure_bounds,
#endif
};

void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
	wayland_ctx_t *wl = (wayland_ctx_t*)data;

	xdg_surface_ack_configure(surface, serial);

	if(wl->base.callbacks.configure) {
		wl->base.callbacks.configure(wl->base.data, wl->width, wl->height);
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

void wl_keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) {
	wayland_ctx_t *wl = (wayland_ctx_t*)data;
	struct xkb_keymap *keymap = NULL;
	struct xkb_state *state = NULL;

	char *buffer = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if(buffer == MAP_FAILED) {
		return;
	}

	keymap = xkb_keymap_new_from_buffer(wl->ctx, buffer, size, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	state = xkb_state_new(keymap);

	if(wl->base.callbacks.keymap_change) {
		wl->base.callbacks.keymap_change(wl->base.data, keymap, state);
	}

	if(wl->keymap) {
		xkb_keymap_unref(wl->keymap);
	}
	if(wl->state) {
		xkb_state_unref(wl->state);
	}
	wl->keymap = keymap;
	wl->state = state;
}

void wl_keyboard_handle_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {

}

void wl_keyboard_handle_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {

}

void wl_keyboard_handle_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	wayland_ctx_t *wl = (wayland_ctx_t*)data;
	key += 8;

	if(wl->base.callbacks.keypress) {
		wl->base.callbacks.keypress(wl->base.data, key, state);
	}
}

void wl_keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
	wayland_ctx_t *wl = data;

	xkb_state_update_mask(wl->state, depressed, latched, locked, 0, 0, group);
}

void wl_keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {

}

static const struct wl_keyboard_listener wl_keyboard_listener = {
	.enter = wl_keyboard_handle_enter,
	.leave = wl_keyboard_handle_leave,
	.key = wl_keyboard_handle_key,
	.keymap = wl_keyboard_handle_keymap,
	.modifiers = wl_keyboard_handle_modifiers,
#if defined(WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
	.repeat_info = wl_keyboard_handle_repeat_info,
#endif
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
#if defined(WL_POINTER_FRAME_SINCE_VERSION)
	.frame = wl_pointer_handle_frame,
#endif
#if defined(WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
	.axis_source = wl_pointer_handle_axis_source,
#endif
#if defined(WL_POINTER_AXIS_STOP_SINCE_VERSION)
	.axis_stop = wl_pointer_handle_axis_stop,
#endif
#if defined(WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
	.axis_discrete = wl_pointer_handle_axis_discrete,
#endif
#if defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
	.axis_value120 = wl_pointer_handle_axis_value120,
#endif
#if defined(WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
	.axis_relative_direction = wl_pointer_handle_axis_relative_direction,
#endif
};

void wl_seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	wayland_ctx_t *wl = (wayland_ctx_t*)data;
	printf("Seat Caps: %x\n", caps);

	if(caps & WL_SEAT_CAPABILITY_KEYBOARD) {
		if(wl->keyboard == NULL) {
			wl->keyboard = wl_seat_get_keyboard(seat);
			wl_keyboard_add_listener(wl->keyboard, &wl_keyboard_listener, data);
		}
	} else if(wl->keyboard) {
		wl_keyboard_destroy(wl->keyboard);
		wl->keyboard = NULL;
	}

	if(caps & WL_SEAT_CAPABILITY_POINTER) {
		if(wl->pointer == NULL) {
			wl->pointer = wl_seat_get_pointer(seat);
			wl_pointer_add_listener(wl->pointer, &wl_pointer_listener, data);
		}
	} else if(wl->pointer) {
		wl_pointer_destroy(wl->pointer);
		wl->pointer = NULL;
	}
}

#if defined(WL_SEAT_NAME_SINCE_VERSION)
void wl_seat_name(void *data, struct wl_seat *seat, const char *name) {
	printf("Seat Name: %s\n", name);
}
#endif

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = wl_seat_capabilities,
#if defined(WL_SEAT_NAME_SINCE_VERSION)
	.name = wl_seat_name,
#endif
};


void wl_shm_format(void *data, struct wl_shm *shm, uint32_t format) {

}

static const struct wl_shm_listener wl_shm_listener = {
	.format = wl_shm_format,
};

void xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
	wayland_ctx_t *wl = (wayland_ctx_t*)data;
	xdg_wm_base_pong(wm_base, serial);
	wl_display_flush(wl->display);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

void wl_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
	wayland_ctx_t *wl = (wayland_ctx_t*)data;
	if(strcmp(interface, wl_compositor_interface.name) == 0) {
		wl->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version);
	} else if(strcmp(interface, wl_shm_interface.name) == 0) {
		wl->shm = wl_registry_bind(registry, name, &wl_shm_interface, version);
		wl_shm_add_listener(wl->shm, &wl_shm_listener, wl);
	} else if(strcmp(interface, wl_seat_interface.name) == 0) {
		wl->seat = wl_registry_bind(registry, name, &wl_seat_interface, version);
		wl_seat_add_listener(wl->seat, &wl_seat_listener, wl);
	} else if(strcmp(interface, wl_subcompositor_interface.name) == 0) {
		wl->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, version);
	} else if(strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wl->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, version);
		xdg_wm_base_add_listener(wl->wm_base, &xdg_wm_base_listener, wl);
	}
}

void wl_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {

}

static const struct wl_registry_listener wl_registry_listener = {
	.global = wl_registry_global,
	.global_remove = wl_registry_global_remove,
};

void wl_buffer_release(void *data, struct wl_buffer *buffer) {
	wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

int term_wl_display_attach_shm(term_display_t *dpy, int fd, uint32_t width, uint32_t height, uint32_t stride, uint32_t size, uint32_t offset, uint32_t format) {
	wayland_ctx_t *wl = (wayland_ctx_t*)dpy;

	struct wl_shm_pool *pool = wl_shm_create_pool(wl->shm, fd, size);
	if(!pool) {
		return -1;
	}

	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);

	wl_surface_attach(wl->surface, buffer, 0, 0);
#if defined(WL_SURFACE_OFFSET_SINCE_VERSION)
	wl_surface_offset(wl->surface, 0, 0);
#endif
#if defined(WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
	wl_surface_damage_buffer(wl->surface, 0, 0, width, height);
#else
	wl_surface_damage(wl->surface, 0, 0, width, height);
#endif
	wl_surface_commit(wl->surface);
	return 0;
}

void term_wl_display_dispatch(term_display_t *dpy) {
	int ret = 0;
	wayland_ctx_t *wl = (wayland_ctx_t *)dpy;
	struct pollfd pfds[1] = { 0 };

	pfds[0].events = POLLIN;
	pfds[0].fd = wl_display_get_fd(wl->display);

	while(1) {
		while(wl_display_prepare_read(wl->display) != 0) {
			wl_display_dispatch_pending(wl->display);
		}
		wl_display_flush(wl->display);

		ret = poll(pfds, 1, 0);
		if(pfds[0].revents & POLLIN) {
			wl_display_read_events(wl->display);
			wl_display_dispatch_pending(wl->display);
		} else {
			wl_display_cancel_read(wl->display);
			break;
		}
	}
}

void term_wl_display_deinit(term_display_t *dpy) {
	wayland_ctx_t *wl = (wayland_ctx_t*)dpy;

	wl_surface_attach(wl->surface, NULL, 0, 0);
	wl_display_roundtrip(wl->display);

	xdg_toplevel_destroy(wl->xdg_toplevel);
	xdg_surface_destroy(wl->xdg_surface);
	wl_surface_destroy(wl->surface);

	if(wl->keyboard) wl_keyboard_destroy(wl->keyboard);
	if(wl->pointer) wl_pointer_destroy(wl->pointer);


	xdg_wm_base_destroy(wl->wm_base);
	wl_shm_destroy(wl->shm);
	wl_compositor_destroy(wl->compositor);
	wl_subcompositor_destroy(wl->subcompositor);
	wl_seat_destroy(wl->seat);

	xkb_state_unref(wl->state);
	xkb_keymap_unref(wl->keymap);
	xkb_context_unref(wl->ctx);

	wl_registry_destroy(wl->registry);

	wl_display_disconnect(wl->display);
	free(wl);
}

term_display_t *term_wl_display_init(void) {
	wayland_ctx_t *wl = calloc(1, sizeof(wayland_ctx_t));

	wl->display = wl_display_connect(NULL);
	if(wl->display == NULL) {
		printf("wl_display_connect failed: %s\n", strerror(errno));
		goto err_free_ctx;
	}
	wl->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	wl->registry = wl_display_get_registry(wl->display);
	if(wl->registry == NULL) {
		printf("wl_display_get_registry failed: %s\n", strerror(errno));
		goto err_disconnect;
	}
	wl_registry_add_listener(wl->registry, &wl_registry_listener, wl);

	if(wl_display_roundtrip(wl->display) == -1) {
		printf("wl_display_roundtrip failed: %s\n", strerror(errno));
		goto err_free_globals;
	}

	if(wl->compositor == NULL) {
		printf("no wl_compositor is a compositor running?\n");
		goto err_free_globals;
	}

	if(wl->subcompositor == NULL) {
		printf("No wl_subcompositor\n");
		goto err_free_globals;
	}

	if(wl->seat == NULL) {
		printf("No wl_seat\n");
		goto err_free_globals;
	}

	if(wl->wm_base == NULL) {
		printf("No xdg_wm_base\n");
		goto err_free_globals;
	}	

	if(wl->shm == NULL) {
		printf("No wl_shm\n");
		goto err_free_globals;
	}

	wl->surface = wl_compositor_create_surface(wl->compositor);
	if(wl->surface == NULL) {
		printf("wl_compositor_create_surface failed: %s\n", strerror(errno));
		goto err_free_globals;
	}

	wl->xdg_surface = xdg_wm_base_get_xdg_surface(wl->wm_base, wl->surface);
	if(wl->xdg_surface == NULL) {
		printf("xdg_wm_base_get_xdg_surface failed: %s\n", strerror(errno));
		goto err_free_surface;
	}
	xdg_surface_add_listener(wl->xdg_surface, &xdg_surface_listener, wl);

	wl->xdg_toplevel = xdg_surface_get_toplevel(wl->xdg_surface);
	xdg_toplevel_set_app_id(wl->xdg_toplevel, "terminal");
	xdg_toplevel_set_title(wl->xdg_toplevel, "project-terminal");
	if(wl->xdg_surface == NULL) {
		printf("xdg_surface_get_toplevel failed: %s\n", strerror(errno));
		goto err_free_surface;
	}
	xdg_toplevel_add_listener(wl->xdg_toplevel, &xdg_toplevel_listener, wl);

	wl_surface_commit(wl->surface);
	wl_display_roundtrip(wl->display);

	wl->base.dispatch = term_wl_display_dispatch;
	wl->base.attach_shm = term_wl_display_attach_shm;
	wl->base.deinit = term_wl_display_deinit;

	return &wl->base;

err_free_surface:
	if(wl->xdg_toplevel) xdg_toplevel_destroy(wl->xdg_toplevel);
	if(wl->xdg_surface) xdg_surface_destroy(wl->xdg_surface);
	if(wl->surface) wl_surface_destroy(wl->surface);

err_free_globals:
	if(wl->subcompositor) wl_subcompositor_destroy(wl->subcompositor);
	if(wl->compositor) wl_compositor_destroy(wl->compositor);
	if(wl->seat) wl_seat_destroy(wl->seat);
	if(wl->shm) wl_shm_destroy(wl->shm);
	if(wl->wm_base) xdg_wm_base_destroy(wl->wm_base);
	wl_registry_destroy(wl->registry);
err_disconnect:
	wl_display_disconnect(wl->display);
err_free_ctx:
	free(wl);
	return NULL;

}
